#ifndef __APPLE__
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include "thrum_rt.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char** environ;

/* ============================== Shared Builtin Constructors ============================== */
TVal thrum_make_builtin(TRuntime* rt, const char* name, TBuiltinPtr fn, int min_arity, int max_arity) {
    const char* iname = thrum_intern(rt, name);
    TBuiltin* b = (TBuiltin*)thrum_alloc_obj(rt, OBJ_BUILTIN, sizeof(TBuiltin));
    b->name = iname;
    b->fn = fn;
    b->cfn = NULL;
    b->ctx = NULL;
    b->owner = NULL;
    b->min_arity = min_arity;
    b->max_arity = max_arity;
    return thrum_obj((TObj*)b);
}

TVal thrum_make_builtin_ctx(TRuntime* rt, const char* name, TBuiltinCtxPtr fn, void* ctx, TObj* owner, int min_arity, int max_arity) {
    T_FRAME(rt);
    TVal ov = owner ? thrum_obj(owner) : thrum_nil();
    thrum_push_root(rt, &ov);
    const char* iname = thrum_intern(rt, name);
    TBuiltin* b = (TBuiltin*)thrum_alloc_obj(rt, OBJ_BUILTIN, sizeof(TBuiltin));
    b->name = iname;
    b->fn = NULL;
    b->cfn = fn;
    b->ctx = ctx;
    b->owner = owner;
    b->min_arity = min_arity;
    b->max_arity = max_arity;
    T_END_FRAME(rt);
    return thrum_obj((TObj*)b);
}

/* ============================== String Builder ============================== */
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} CSB;

static void csb_app_n(CSB* sb, const char* s, size_t n) {
    if (sb->len + n + 1 > sb->cap) {
        size_t nc = sb->cap ? sb->cap * 2 : 128;
        while (nc < sb->len + n + 1) nc *= 2;
        sb->data = (char*)realloc(sb->data, nc);
        sb->cap = nc;
    }
    if (n > 0) memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void csb_app(CSB* sb, const char* s) {
    if (s) csb_app_n(sb, s, strlen(s));
}

static void csb_ch(CSB* sb, char c) {
    csb_app_n(sb, &c, 1);
}

/* ============================== JSON Codec + $q Support ============================== */
typedef struct {
    const char* s;
    size_t len;
    size_t i;
    TRuntime* rt;
    bool decode_qty;
    int line;
    int col;
} JParser;

static void j_skip(JParser* p) {
    while (p->i < p->len) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

static _Noreturn void j_err(JParser* p, const char* msg) {
    char buf[160];
    snprintf(buf, sizeof(buf), "json parse error at byte %zu: %s", p->i, msg);
    thrum_error(p->rt, p->line, p->col, buf, NULL);
}

static void append_utf8(CSB* sb, uint32_t cp) {
    if (cp <= 0x7F) {
        csb_ch(sb, (char)cp);
    } else if (cp <= 0x7FF) {
        csb_ch(sb, (char)(0xC0 | (cp >> 6)));
        csb_ch(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        csb_ch(sb, (char)(0xE0 | (cp >> 12)));
        csb_ch(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        csb_ch(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        csb_ch(sb, (char)(0xF0 | (cp >> 18)));
        csb_ch(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        csb_ch(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        csb_ch(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

static uint32_t j_hex4(JParser* p) {
    if (p->i + 4 > p->len) j_err(p, "truncated \\uXXXX escape");
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        unsigned char c = (unsigned char)p->s[p->i++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else j_err(p, "invalid hex escape");
    }
    return v;
}

static TVal j_str(JParser* p) {
    if (p->s[p->i] != '"') j_err(p, "expected '\"'");
    p->i++;
    CSB sb = {0};
    while (p->i < p->len) {
        char c = p->s[p->i++];
        if (c == '"') {
            TVal out = thrum_str(p->rt, sb.data ? sb.data : "", sb.len);
            free(sb.data);
            return out;
        }
        if (c == '\\') {
            if (p->i >= p->len) { free(sb.data); j_err(p, "unterminated escape"); }
            char e = p->s[p->i++];
            switch (e) {
                case '"': csb_ch(&sb, '"'); break;
                case '\\': csb_ch(&sb, '\\'); break;
                case '/': csb_ch(&sb, '/'); break;
                case 'b': csb_ch(&sb, '\b'); break;
                case 'f': csb_ch(&sb, '\f'); break;
                case 'n': csb_ch(&sb, '\n'); break;
                case 'r': csb_ch(&sb, '\r'); break;
                case 't': csb_ch(&sb, '\t'); break;
                case 'u': {
                    uint32_t cp = j_hex4(p);
                    if (cp >= 0xD800 && cp <= 0xDBFF && p->i + 6 <= p->len &&
                        p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                        p->i += 2;
                        uint32_t lo = j_hex4(p);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                        }
                    }
                    append_utf8(&sb, cp);
                    break;
                }
                default:
                    free(sb.data);
                    j_err(p, "invalid escape sequence");
            }
        } else {
            csb_ch(&sb, c);
        }
    }
    free(sb.data);
    j_err(p, "unterminated string");
}

static TFrac parse_frac_str(const char* s) {
    const char* sl = strchr(s, '/');
    if (!sl) {
        return thrum_frac(strtoll(s, NULL, 10), 1);
    }
    int64_t num = strtoll(s, NULL, 10);
    int64_t den = strtoll(sl + 1, NULL, 10);
    return thrum_frac(num, den ? den : 1);
}

static TVal j_val(JParser* p);

static TVal j_arr(JParser* p) {
    p->i++; /* '[' */
    T_FRAME(p->rt);
    T_VAR(p->rt, lst, thrum_list(p->rt, 4));
    TList* l = (TList*)lst.as.obj;
    j_skip(p);
    if (p->i < p->len && p->s[p->i] == ']') {
        p->i++;
        T_END_FRAME(p->rt);
        return lst;
    }
    for (;;) {
        j_skip(p);
        T_VAR(p->rt, item, j_val(p));
        thrum_list_push(p->rt, l, item);
        j_skip(p);
        if (p->i >= p->len) j_err(p, "unterminated array");
        if (p->s[p->i] == ']') { p->i++; break; }
        if (p->s[p->i] != ',') j_err(p, "expected ',' or ']'");
        p->i++;
    }
    T_END_FRAME(p->rt);
    return lst;
}

static TVal j_obj(JParser* p) {
    p->i++; /* '{' */
    T_FRAME(p->rt);
    T_VAR(p->rt, mp, thrum_map(p->rt));
    TMap* m = (TMap*)mp.as.obj;
    j_skip(p);
    if (p->i < p->len && p->s[p->i] == '}') {
        p->i++;
        T_END_FRAME(p->rt);
        return mp;
    }
    for (;;) {
        j_skip(p);
        if (p->i >= p->len || p->s[p->i] != '"') j_err(p, "expected string key");
        T_VAR(p->rt, k, j_str(p));
        j_skip(p);
        if (p->i >= p->len || p->s[p->i] != ':') j_err(p, "expected ':'");
        p->i++;
        j_skip(p);
        T_VAR(p->rt, v, j_val(p));
        thrum_map_set(p->rt, m, k, v);
        j_skip(p);
        if (p->i >= p->len) j_err(p, "unterminated object");
        if (p->s[p->i] == '}') { p->i++; break; }
        if (p->s[p->i] != ',') j_err(p, "expected ',' or '}'");
        p->i++;
    }
    if (p->decode_qty && m->count == 1) {
        TVal qk = thrum_cstr(p->rt, "$q");
        TVal qv;
        if (thrum_map_get(m, qk, &qv) && qv.kind == TV_OBJ && qv.as.obj->kind == OBJ_LIST) {
            TList* arr = (TList*)qv.as.obj;
            if (arr->len == 4 && arr->items[0].kind == TV_NUM &&
                arr->items[1].kind == TV_OBJ && arr->items[1].as.obj->kind == OBJ_LIST &&
                arr->items[2].kind == TV_NUM) {
                TList* darr = (TList*)arr->items[1].as.obj;
                if (darr->len == 7) {
                    TDim d = thrum_d0();
                    for (int k = 0; k < 7; k++) {
                        if (darr->items[k].kind == TV_OBJ && darr->items[k].as.obj->kind == OBJ_STR) {
                            d.e[k] = parse_frac_str(((TStr*)darr->items[k].as.obj)->data);
                        }
                    }
                    double disp_sc = 1.0;
                    const char* disp_sym = NULL;
                    if (arr->items[3].kind == TV_OBJ && arr->items[3].as.obj->kind == OBJ_LIST) {
                        TList* disp = (TList*)arr->items[3].as.obj;
                        if (disp->len == 2 && disp->items[0].kind == TV_NUM &&
                            disp->items[1].kind == TV_OBJ && disp->items[1].as.obj->kind == OBJ_STR) {
                            disp_sc = disp->items[0].as.q.v;
                            disp_sym = ((TStr*)disp->items[1].as.obj)->data;
                        }
                    }
                    TVal out = thrum_qty(p->rt, arr->items[0].as.q.v, arr->items[2].as.q.v, d, disp_sc, disp_sym);
                    T_END_FRAME(p->rt);
                    return out;
                }
            }
        }
    }
    T_END_FRAME(p->rt);
    return mp;
}

static TVal j_val(JParser* p) {
    j_skip(p);
    if (p->i >= p->len) j_err(p, "unexpected end of JSON");
    char c = p->s[p->i];
    if (c == '"') return j_str(p);
    if (c == '[') return j_arr(p);
    if (c == '{') return j_obj(p);
    if (c == 't' && p->i + 4 <= p->len && strncmp(p->s + p->i, "true", 4) == 0) {
        p->i += 4;
        return thrum_bool(true);
    }
    if (c == 'f' && p->i + 5 <= p->len && strncmp(p->s + p->i, "false", 5) == 0) {
        p->i += 5;
        return thrum_bool(false);
    }
    if (c == 'n' && p->i + 4 <= p->len && strncmp(p->s + p->i, "null", 4) == 0) {
        p->i += 4;
        return thrum_nil();
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char* end = NULL;
        double d = strtod(p->s + p->i, &end);
        if (end == p->s + p->i) j_err(p, "invalid number");
        p->i = (size_t)(end - p->s);
        return thrum_num_dimless(d);
    }
    j_err(p, "unexpected token");
}

TVal thrum_json_parse_str(TRuntime* rt, const char* s, size_t len, bool decode_qty, int line, int col) {
    JParser p = {s ? s : "", len, 0, rt, decode_qty, line, col};
    TVal v = j_val(&p);
    j_skip(&p);
    if (p.i < p.len) j_err(&p, "trailing content after JSON value");
    return v;
}

static void j_dump_str(CSB* sb, const char* s, size_t len) {
    csb_ch(sb, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': csb_app(sb, "\\\""); break;
            case '\\': csb_app(sb, "\\\\"); break;
            case '\b': csb_app(sb, "\\b"); break;
            case '\f': csb_app(sb, "\\f"); break;
            case '\n': csb_app(sb, "\\n"); break;
            case '\r': csb_app(sb, "\\r"); break;
            case '\t': csb_app(sb, "\\t"); break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    csb_app(sb, buf);
                } else {
                    csb_ch(sb, (char)c);
                }
        }
    }
    csb_ch(sb, '"');
}

static void j_indent(CSB* sb, int indent, int depth) {
    if (indent < 0) return;
    csb_ch(sb, '\n');
    int tot = indent * depth;
    for (int i = 0; i < tot; i++) csb_ch(sb, ' ');
}

static void j_dump_val(TRuntime* rt, CSB* sb, TVal v, bool qty_mode, int indent, int depth, int line, int col) {
    switch (v.kind) {
        case TV_NIL:
            csb_app(sb, "null");
            return;
        case TV_BOOL:
            csb_app(sb, v.as.b ? "true" : "false");
            return;
        case TV_NUM: {
            TQ q = v.as.q;
            if (!qty_mode) {
                if (!isfinite(q.v)) {
                    thrum_error(rt, line, col, "JSON cannot encode non-finite number", NULL);
                }
                double sc = q.disp_sc != 0.0 ? q.disp_sc : 1.0;
                double val = q.v / sc;
                char buf[64];
                if (fabs(val) < 1e15 && val == (double)(int64_t)val) {
                    snprintf(buf, sizeof(buf), "%lld", (long long)(int64_t)val);
                } else {
                    snprintf(buf, sizeof(buf), "%.15g", val);
                }
                csb_app(sb, buf);
                return;
            }
            /* {"$q": [v, [...], u, [sc, sym] | null]} */
            csb_app(sb, "{\"$q\":");
            csb_app(sb, indent >= 0 ? " " : " ");
            csb_ch(sb, '[');
            char numbuf[64];
            if (!isfinite(q.v)) {
                snprintf(numbuf, sizeof(numbuf), "%s", isnan(q.v) ? "NaN" : (q.v > 0 ? "Infinity" : "-Infinity"));
            } else {
                snprintf(numbuf, sizeof(numbuf), "%.17g", q.v);
            }
            csb_app(sb, numbuf);
            csb_app(sb, ", [");
            for (int k = 0; k < 7; k++) {
                if (k > 0) csb_app(sb, ", ");
                char fb[48];
                if (q.d.e[k].den == 1) snprintf(fb, sizeof(fb), "\"%lld\"", (long long)q.d.e[k].num);
                else snprintf(fb, sizeof(fb), "\"%lld/%lld\"", (long long)q.d.e[k].num, (long long)q.d.e[k].den);
                csb_app(sb, fb);
            }
            csb_app(sb, "], ");
            snprintf(numbuf, sizeof(numbuf), "%.17g", q.u);
            csb_app(sb, numbuf);
            csb_app(sb, ", ");
            if (q.disp_sym) {
                csb_ch(sb, '[');
                snprintf(numbuf, sizeof(numbuf), "%.17g", q.disp_sc);
                csb_app(sb, numbuf);
                csb_app(sb, ", ");
                j_dump_str(sb, q.disp_sym, strlen(q.disp_sym));
                csb_ch(sb, ']');
            } else {
                csb_app(sb, "null");
            }
            csb_app(sb, "]}");
            return;
        }
        case TV_OBJ: {
            TObj* o = v.as.obj;
            if (o->kind == OBJ_STR) {
                TStr* s = (TStr*)o;
                j_dump_str(sb, s->data, s->len);
                return;
            }
            if (o->kind == OBJ_LIST) {
                TList* l = (TList*)o;
                if (l->len == 0) { csb_app(sb, "[]"); return; }
                csb_ch(sb, '[');
                for (size_t i = 0; i < l->len; i++) {
                    if (i > 0) {
                        csb_ch(sb, ',');
                        if (indent < 0) csb_ch(sb, ' ');
                    }
                    if (indent >= 0) j_indent(sb, indent, depth + 1);
                    j_dump_val(rt, sb, l->items[i], qty_mode, indent, depth + 1, line, col);
                }
                if (indent >= 0) j_indent(sb, indent, depth);
                csb_ch(sb, ']');
                return;
            }
            if (o->kind == OBJ_MAP) {
                TMap* m = (TMap*)o;
                if (m->count == 0) { csb_app(sb, "{}"); return; }
                csb_ch(sb, '{');
                bool first = true;
                for (size_t i = 0; i < m->order_len; i++) {
                    TVal k = m->order_keys[i];
                    TVal val;
                    if (!thrum_map_get(m, k, &val)) continue;
                    if (!first) {
                        csb_ch(sb, ',');
                        if (indent < 0) csb_ch(sb, ' ');
                    }
                    first = false;
                    if (indent >= 0) j_indent(sb, indent, depth + 1);
                    char* ks = thrum_vstr_alloc(rt, k, false);
                    j_dump_str(sb, ks, strlen(ks));
                    free(ks);
                    csb_app(sb, ": ");
                    j_dump_val(rt, sb, val, qty_mode, indent, depth + 1, line, col);
                }
                if (indent >= 0) j_indent(sb, indent, depth);
                csb_ch(sb, '}');
                return;
            }
            if (o->kind == OBJ_RECVAL && !qty_mode) {
                TRecVal* rv = (TRecVal*)o;
                if (rv->rtype->nfields == 0) { csb_app(sb, "{}"); return; }
                csb_ch(sb, '{');
                for (int i = 0; i < rv->rtype->nfields; i++) {
                    if (i > 0) {
                        csb_ch(sb, ',');
                        if (indent < 0) csb_ch(sb, ' ');
                    }
                    if (indent >= 0) j_indent(sb, indent, depth + 1);
                    j_dump_str(sb, rv->rtype->fields[i].name, strlen(rv->rtype->fields[i].name));
                    csb_app(sb, ": ");
                    j_dump_val(rt, sb, rv->fields[i], qty_mode, indent, depth + 1, line, col);
                }
                if (indent >= 0) j_indent(sb, indent, depth);
                csb_ch(sb, '}');
                return;
            }
            if (qty_mode) {
                char* s = thrum_vstr_alloc(rt, v, false);
                j_dump_str(sb, s, strlen(s));
                free(s);
                return;
            }
            char msg[96];
            snprintf(msg, sizeof(msg), "cannot encode %s as JSON", thrum_tname(v));
            thrum_error(rt, line, col, msg, NULL);
        }
    }
}

char* thrum_json_dump_alloc(TRuntime* rt, TVal v, bool qty_mode, int indent, int line, int col) {
    CSB sb = {0};
    j_dump_val(rt, &sb, v, qty_mode, indent, 0, line, col);
    if (!sb.data) return strdup("null");
    return sb.data;
}

/* ============================== Journal ============================== */
void thrum_journal_save(TRuntime* rt) {
    if (!rt->journal_path) return;
    FILE* fh = fopen(rt->journal_path, "wb");
    if (!fh) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot write journal '%s'", rt->journal_path);
        thrum_error(rt, 0, 0, msg, NULL);
    }
    T_FRAME(rt);
    T_VAR(rt, lst, thrum_list(rt, (size_t)rt->jlen));
    TList* arr = (TList*)lst.as.obj;
    for (int i = 0; i < rt->jlen; i++) {
        T_VAR(rt, ev, thrum_map(rt));
        TMap* em = (TMap*)ev.as.obj;
        thrum_map_set_cs(rt, em, "kind", thrum_cstr(rt, rt->jevents[i].kind ? rt->jevents[i].kind : ""));
        thrum_map_set_cs(rt, em, "key", thrum_cstr(rt, rt->jevents[i].key ? rt->jevents[i].key : ""));
        thrum_map_set_cs(rt, em, "value", rt->jevents[i].value);
        thrum_list_push(rt, arr, ev);
    }
    char* json = thrum_json_dump_alloc(rt, lst, true, -1, 0, 0);
    fputs(json, fh);
    free(json);
    fclose(fh);
    T_END_FRAME(rt);
}

void thrum_journal_load(TRuntime* rt, const char* path) {
    FILE* fh = fopen(path, "rb");
    if (!fh) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot read journal '%s'", path);
        thrum_error(rt, 0, 0, msg, NULL);
    }
    fseek(fh, 0, SEEK_END);
    long sz = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)(sz > 0 ? sz : 1) + 1);
    size_t nread = sz > 0 ? fread(buf, 1, (size_t)sz, fh) : 0;
    buf[nread] = '\0';
    fclose(fh);

    T_FRAME(rt);
    T_VAR(rt, v, thrum_json_parse_str(rt, buf, nread, true, 0, 0));
    free(buf);
    if (v.kind != TV_OBJ || v.as.obj->kind != OBJ_LIST) {
        thrum_error(rt, 0, 0, "invalid journal format (expected list)", NULL);
    }
    TList* arr = (TList*)v.as.obj;
    rt->jcap = (int)arr->len + 16;
    rt->jevents = (TJournalEv*)calloc((size_t)rt->jcap, sizeof(TJournalEv));
    rt->jlen = 0;
    rt->jidx = 0;
    for (size_t i = 0; i < arr->len; i++) {
        if (arr->items[i].kind != TV_OBJ || arr->items[i].as.obj->kind != OBJ_MAP) continue;
        TMap* em = (TMap*)arr->items[i].as.obj;
        TVal kv = thrum_nil(), keyv = thrum_nil(), vv = thrum_nil();
        thrum_map_get(em, thrum_cstr(rt, "kind"), &kv);
        thrum_map_get(em, thrum_cstr(rt, "key"), &keyv);
        thrum_map_get(em, thrum_cstr(rt, "value"), &vv);
        rt->jevents[rt->jlen].kind = thrum_vstr_alloc(rt, kv, false);
        rt->jevents[rt->jlen].key = thrum_vstr_alloc(rt, keyv, false);
        rt->jevents[rt->jlen].value = vv;
        rt->jlen++;
    }
    T_END_FRAME(rt);
}

TVal thrum_journal_get(TRuntime* rt, const char* kind, const char* key, TVal (*produce)(TRuntime*, void*), void* ctx) {
    if (rt->journal_replay) {
        if (rt->jidx >= rt->jlen) {
            char msg[160];
            snprintf(msg, sizeof(msg), "replay journal exhausted at '%s'", kind);
            thrum_error(rt, 0, 0, msg, NULL);
        }
        TJournalEv* ev = &rt->jevents[rt->jidx++];
        if (strcmp(ev->kind, kind) != 0) {
            char msg[192];
            snprintf(msg, sizeof(msg), "replay divergence: expected '%s', journal has '%s'", kind, ev->kind);
            thrum_error(rt, 0, 0, msg, NULL);
        }
        return ev->value;
    }
    T_FRAME(rt);
    T_VAR(rt, val, produce(rt, ctx));
    if (rt->journal_record) {
        if (rt->jlen >= rt->jcap) {
            rt->jcap = rt->jcap ? rt->jcap * 2 : 64;
            rt->jevents = (TJournalEv*)realloc(rt->jevents, (size_t)rt->jcap * sizeof(TJournalEv));
        }
        rt->jevents[rt->jlen].kind = strdup(kind ? kind : "");
        rt->jevents[rt->jlen].key = strdup(key ? key : "");
        rt->jevents[rt->jlen].value = val;
        rt->jlen++;
    }
    T_END_FRAME(rt);
    return val;
}

static TVal j_ret_v(TRuntime* rt, void* ctx) {
    (void)rt;
    return *(TVal*)ctx;
}

static void record_out_divergence(TRuntime* rt, const char* jkind, const char* txt) {
    TVal tv = thrum_cstr(rt, txt);
    T_FRAME(rt);
    T_VAR(rt, rec, thrum_journal_get(rt, jkind, "", j_ret_v, &tv));
    if (rt->journal_replay) {
        char* rstr = thrum_vstr_alloc(rt, rec, false);
        if (rec.kind != TV_OBJ || rec.as.obj->kind != OBJ_STR || strcmp(rstr, txt) != 0) {
            rt->jdiv++;
        }
        free(rstr);
    }
    T_END_FRAME(rt);
}

/* ============================== Cap Helper ============================== */
static TCap* mk_cap(TRuntime* rt, const char* name, TCapCleanup cleanup, void (*free_state)(void*), void* state) {
    T_FRAME(rt);
    TCap* c = (TCap*)thrum_alloc_obj(rt, OBJ_CAP, sizeof(TCap));
    c->name = thrum_intern(rt, name);
    T_VAR(rt, cv, thrum_obj((TObj*)c));
    c->methods = (TMap*)thrum_map(rt).as.obj;
    c->cleanup = cleanup;
    c->free_state = free_state;
    c->state = state;
    if (cleanup) thrum_push_cleanup(rt, c);
    T_END_FRAME(rt);
    return c;
}

static void cap_add(TRuntime* rt, TCap* cap, const char* mname, TBuiltinCtxPtr fn, int min_a, int max_a) {
    T_FRAME(rt);
    char full[96];
    snprintf(full, sizeof(full), "%s.%s", cap->name, mname);
    T_VAR(rt, bv, thrum_make_builtin_ctx(rt, full, fn, cap->state, (TObj*)cap, min_a, max_a));
    thrum_map_set_cs(rt, cap->methods, mname, bv);
    T_END_FRAME(rt);
}

TCap* thrum_alloc_cap(TRuntime* rt, const char* name, TCapCleanup cleanup, void (*free_state)(void*), void* state) {
    return mk_cap(rt, name, cleanup, free_state, state);
}

void thrum_cap_add_method(TRuntime* rt, TCap* cap, const char* mname, TBuiltinCtxPtr fn, int min_a, int max_a) {
    cap_add(rt, cap, mname, fn, min_a, max_a);
}

/* ============================== cap_io ============================== */
static TVal io_say(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)line; (void)col;
    CSB sb = {0};
    for (int i = 0; i < argc; i++) {
        if (i > 0) csb_ch(&sb, ' ');
        char* s = thrum_vstr_alloc(rt, argv[i], false);
        csb_app(&sb, s);
        free(s);
    }
    const char* txt = sb.data ? sb.data : "";
    record_out_divergence(rt, "out", txt);
    FILE* out = rt->out ? rt->out : stdout;
    fputs(txt, out);
    fputc('\n', out);
    free(sb.data);
    return thrum_nil();
}

static TVal io_print(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)line; (void)col;
    CSB sb = {0};
    for (int i = 0; i < argc; i++) {
        if (i > 0) csb_ch(&sb, ' ');
        char* s = thrum_vstr_alloc(rt, argv[i], false);
        csb_app(&sb, s);
        free(s);
    }
    const char* txt = sb.data ? sb.data : "";
    record_out_divergence(rt, "out", txt);
    FILE* out = rt->out ? rt->out : stdout;
    fputs(txt, out);
    fflush(out);
    free(sb.data);
    return thrum_nil();
}

static TVal io_ask_prod(TRuntime* rt, void* ctx) {
    const char* prompt = (const char*)ctx;
    if (prompt && prompt[0]) {
        fputs(prompt, rt->out ? rt->out : stdout);
        fflush(rt->out ? rt->out : stdout);
    }
    CSB sb = {0};
    int ch;
    while ((ch = fgetc(stdin)) != EOF) {
        if (ch == '\n') break;
        csb_ch(&sb, (char)ch);
    }
    if (sb.len > 0 && sb.data[sb.len - 1] == '\r') sb.len--;
    TVal out = thrum_str(rt, sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return out;
}

static TVal io_ask(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)line; (void)col;
    char* prompt = argc > 0 ? thrum_vstr_alloc(rt, argv[0], false) : strdup("");
    TVal out = thrum_journal_get(rt, "io.ask", prompt, io_ask_prod, prompt);
    free(prompt);
    return out;
}

static TVal io_warn(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)line; (void)col;
    FILE* out = rt->out ? rt->out : stdout;
    fputs("! ", out);
    for (int i = 0; i < argc; i++) {
        if (i > 0) fputc(' ', out);
        char* s = thrum_vstr_alloc(rt, argv[i], false);
        fputs(s, out);
        free(s);
    }
    fputc('\n', out);
    return thrum_nil();
}

static TVal io_err(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)line; (void)col;
    for (int i = 0; i < argc; i++) {
        if (i > 0) fputc(' ', stderr);
        char* s = thrum_vstr_alloc(rt, argv[i], false);
        fputs(s, stderr);
        free(s);
    }
    fputc('\n', stderr);
    return thrum_nil();
}

static TVal io_table(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx;
    if (argv[0].kind != TV_OBJ || argv[0].as.obj->kind != OBJ_LIST) {
        thrum_error(rt, line, col, "io.table expects a list of rows", NULL);
    }
    TList* rows = (TList*)argv[0].as.obj;
    TList* headers = NULL;
    if (argc > 1) {
        if (argv[1].kind != TV_OBJ || argv[1].as.obj->kind != OBJ_LIST) {
            thrum_error(rt, line, col, "io.table headers must be a list", NULL);
        }
        headers = (TList*)argv[1].as.obj;
    }
    if (rows->len == 0 && (!headers || headers->len == 0)) return thrum_nil();

    bool is_map_rows = (rows->len > 0 && rows->items[0].kind == TV_OBJ && rows->items[0].as.obj->kind == OBJ_MAP);
    int ncols = 0;
    char** col_names = NULL;
    TVal* col_keys = NULL;
    bool has_cols = false;

    if (is_map_rows) {
        has_cols = true;
        if (headers) {
            ncols = (int)headers->len;
            col_keys = (TVal*)malloc((size_t)(ncols > 0 ? ncols : 1) * sizeof(TVal));
            col_names = (char**)malloc((size_t)(ncols > 0 ? ncols : 1) * sizeof(char*));
            for (int j = 0; j < ncols; j++) {
                col_keys[j] = headers->items[j];
                col_names[j] = thrum_vstr_alloc(rt, col_keys[j], false);
            }
        } else {
            TMap* m0 = (TMap*)rows->items[0].as.obj;
            col_keys = (TVal*)malloc((size_t)(m0->order_len > 0 ? m0->order_len : 1) * sizeof(TVal));
            col_names = (char**)malloc((size_t)(m0->order_len > 0 ? m0->order_len : 1) * sizeof(char*));
            for (size_t k = 0; k < m0->order_len; k++) {
                if (!thrum_map_has(m0, m0->order_keys[k])) continue;
                col_keys[ncols] = m0->order_keys[k];
                col_names[ncols] = thrum_vstr_alloc(rt, col_keys[ncols], false);
                ncols++;
            }
        }
    } else if (headers) {
        has_cols = true;
        ncols = (int)headers->len;
        col_names = (char**)malloc((size_t)(ncols > 0 ? ncols : 1) * sizeof(char*));
        for (int j = 0; j < ncols; j++) col_names[j] = thrum_vstr_alloc(rt, headers->items[j], false);
    }

    size_t nrows = rows->len;
    char*** cells = (char***)calloc(nrows > 0 ? nrows : 1, sizeof(char**));
    int* row_lens = (int*)calloc(nrows > 0 ? nrows : 1, sizeof(int));
    for (size_t r = 0; r < nrows; r++) {
        if (is_map_rows) {
            TMap* rm = (rows->items[r].kind == TV_OBJ && rows->items[r].as.obj->kind == OBJ_MAP)
                           ? (TMap*)rows->items[r].as.obj
                           : NULL;
            row_lens[r] = ncols;
            cells[r] = (char**)calloc((size_t)(ncols > 0 ? ncols : 1), sizeof(char*));
            for (int j = 0; j < ncols; j++) {
                TVal v = thrum_nil();
                if (rm) thrum_map_get(rm, col_keys[j], &v);
                cells[r][j] = thrum_vstr_alloc(rt, v, false);
            }
        } else if (rows->items[r].kind == TV_OBJ && rows->items[r].as.obj->kind == OBJ_LIST) {
            TList* rl = (TList*)rows->items[r].as.obj;
            row_lens[r] = (int)rl->len;
            if ((int)rl->len > ncols) ncols = (int)rl->len;
            cells[r] = (char**)calloc((size_t)(rl->len > 0 ? rl->len : 1), sizeof(char*));
            for (size_t j = 0; j < rl->len; j++) cells[r][j] = thrum_vstr_alloc(rt, rl->items[j], false);
        } else {
            row_lens[r] = 1;
            if (1 > ncols) ncols = 1;
            cells[r] = (char**)malloc(sizeof(char*));
            cells[r][0] = thrum_vstr_alloc(rt, rows->items[r], false);
        }
    }

    if (ncols == 0 && !has_cols) {
        free(col_keys); free(col_names); free(cells); free(row_lens);
        return thrum_nil();
    }
    int* widths = (int*)calloc((size_t)(ncols > 0 ? ncols : 1), sizeof(int));
    if (has_cols) {
        for (int j = 0; j < ncols; j++) {
            int l = (int)strlen(col_names[j]);
            if (l > widths[j]) widths[j] = l;
        }
    }
    for (size_t r = 0; r < nrows; r++) {
        for (int j = 0; j < row_lens[r]; j++) {
            int l = (int)strlen(cells[r][j]);
            if (l > widths[j]) widths[j] = l;
        }
    }

    CSB sb = {0};
    if (has_cols) {
        CSB line1 = {0}, line2 = {0};
        for (int j = 0; j < ncols; j++) {
            if (j > 0) { csb_app(&line1, "  "); csb_app(&line2, "  "); }
            const char* s = col_names[j];
            int sl = (int)strlen(s);
            csb_app(&line1, s);
            for (int k = sl; k < widths[j]; k++) csb_ch(&line1, ' ');
            for (int k = 0; k < widths[j]; k++) csb_ch(&line2, '-');
        }
        while (line1.len > 0 && line1.data[line1.len - 1] == ' ') line1.len--;
        if (line1.data) csb_app_n(&sb, line1.data, line1.len);
        csb_ch(&sb, '\n');
        if (line2.data) csb_app_n(&sb, line2.data, line2.len);
        free(line1.data);
        free(line2.data);
    }
    for (size_t r = 0; r < nrows; r++) {
        if (has_cols || r > 0) csb_ch(&sb, '\n');
        CSB rowb = {0};
        for (int j = 0; j < ncols; j++) {
            if (j > 0) csb_app(&rowb, "  ");
            const char* s = (j < row_lens[r]) ? cells[r][j] : "";
            int sl = (int)strlen(s);
            csb_app(&rowb, s);
            for (int k = sl; k < widths[j]; k++) csb_ch(&rowb, ' ');
        }
        while (rowb.len > 0 && rowb.data[rowb.len - 1] == ' ') rowb.len--;
        if (rowb.data) csb_app_n(&sb, rowb.data, rowb.len);
        free(rowb.data);
    }

    const char* txt = sb.data ? sb.data : "";
    record_out_divergence(rt, "out", txt);
    FILE* out = rt->out ? rt->out : stdout;
    fputs(txt, out);
    fputc('\n', out);

    free(sb.data);
    free(widths);
    free(col_keys);
    if (col_names) {
        for (int j = 0; j < ncols; j++) free(col_names[j]);
        free(col_names);
    }
    for (size_t r = 0; r < nrows; r++) {
        for (int j = 0; j < row_lens[r]; j++) free(cells[r][j]);
        free(cells[r]);
    }
    free(cells);
    free(row_lens);
    return thrum_nil();
}

static TCap* mk_cap_io(TRuntime* rt) {
    TCap* c = mk_cap(rt, "io", NULL, NULL, NULL);
    cap_add(rt, c, "say", io_say, 0, -1);
    cap_add(rt, c, "print", io_print, 0, -1);
    cap_add(rt, c, "ask", io_ask, 0, 1);
    cap_add(rt, c, "warn", io_warn, 0, -1);
    cap_add(rt, c, "err", io_err, 0, -1);
    cap_add(rt, c, "table", io_table, 1, 2);
    return c;
}

/* ============================== cap_clock ============================== */
static TVal prod_clock_now(TRuntime* rt, void* ctx) {
    (void)rt; (void)ctx;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return thrum_num_dimless((double)tv.tv_sec + (double)tv.tv_usec / 1e6);
}

static TVal prod_clock_mono(TRuntime* rt, void* ctx) {
    (void)rt; (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return thrum_num_dimless((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

static TVal clock_now(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)argc; (void)argv; (void)line; (void)col;
    TVal r = thrum_journal_get(rt, "clock.now", "", prod_clock_now, NULL);
    return thrum_qty(rt, r.as.q.v, 0.0, thrum_dim(0, 0, 1, 0, 0, 0, 0), 1.0, "s");
}

static TVal clock_mono(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)argc; (void)argv; (void)line; (void)col;
    TVal r = thrum_journal_get(rt, "clock.mono", "", prod_clock_mono, NULL);
    return thrum_qty(rt, r.as.q.v, 0.0, thrum_dim(0, 0, 1, 0, 0, 0, 0), 1.0, "s");
}

static TVal clock_sleep(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)argc;
    TQ sec = thrum_need_q(rt, argv[0], line, col);
    if (!thrum_dim_eq(sec.d, thrum_dim(0, 0, 1, 0, 0, 0, 0))) {
        thrum_error(rt, line, col, "sleep expects a duration (s)", NULL);
    }
    TVal sv = thrum_num_dimless(sec.v);
    thrum_journal_get(rt, "clock.sleep", "", j_ret_v, &sv);
    if (!rt->journal_replay && sec.v > 0.0) {
        struct timespec ts;
        ts.tv_sec = (time_t)floor(sec.v);
        ts.tv_nsec = (long)((sec.v - (double)ts.tv_sec) * 1e9);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (ts.tv_nsec < 0) ts.tv_nsec = 0;
        nanosleep(&ts, NULL);
    }
    return thrum_nil();
}

static TCap* mk_cap_clock(TRuntime* rt) {
    TCap* c = mk_cap(rt, "clock", NULL, NULL, NULL);
    cap_add(rt, c, "now", clock_now, 0, 0);
    cap_add(rt, c, "mono", clock_mono, 0, 0);
    cap_add(rt, c, "sleep", clock_sleep, 1, 1);
    return c;
}

/* ============================== cap_rand (Python-exact MT19937) ============================== */
#define MT_N 624
#define MT_M 397
#define MT_MATRIX_A 0x9908b0dfU
#define MT_UPPER_MASK 0x80000000U
#define MT_LOWER_MASK 0x7fffffffU

typedef struct {
    uint32_t mt[MT_N];
    int mti;
    bool has_gauss;
    double gauss_next;
} TRandState;

static void mt_init_genrand(TRandState* s, uint32_t seed) {
    s->mt[0] = seed;
    for (s->mti = 1; s->mti < MT_N; s->mti++) {
        s->mt[s->mti] = 1812433253U * (s->mt[s->mti - 1] ^ (s->mt[s->mti - 1] >> 30)) + (uint32_t)s->mti;
    }
}

static void mt_init_by_array(TRandState* s, const uint32_t* key, int key_len) {
    mt_init_genrand(s, 19650218U);
    int i = 1, j = 0;
    int k = MT_N > key_len ? MT_N : key_len;
    for (; k; k--) {
        s->mt[i] = (s->mt[i] ^ ((s->mt[i - 1] ^ (s->mt[i - 1] >> 30)) * 1664525U)) + key[j] + (uint32_t)j;
        i++;
        j++;
        if (i >= MT_N) { s->mt[0] = s->mt[MT_N - 1]; i = 1; }
        if (j >= key_len) j = 0;
    }
    for (k = MT_N - 1; k; k--) {
        s->mt[i] = (s->mt[i] ^ ((s->mt[i - 1] ^ (s->mt[i - 1] >> 30)) * 1566083941U)) - (uint32_t)i;
        i++;
        if (i >= MT_N) { s->mt[0] = s->mt[MT_N - 1]; i = 1; }
    }
    s->mt[0] = 0x80000000U;
}

static void mt_seed_u64(TRandState* s, uint64_t seed) {
    uint32_t key[2];
    int n = 1;
    key[0] = (uint32_t)(seed & 0xffffffffU);
    if (seed >> 32) {
        key[1] = (uint32_t)(seed >> 32);
        n = 2;
    }
    mt_init_by_array(s, key, n);
    s->has_gauss = false;
}

static uint32_t mt_uint32(TRandState* s) {
    static const uint32_t mag01[2] = {0x0U, MT_MATRIX_A};
    uint32_t y;
    if (s->mti >= MT_N) {
        int kk;
        for (kk = 0; kk < MT_N - MT_M; kk++) {
            y = (s->mt[kk] & MT_UPPER_MASK) | (s->mt[kk + 1] & MT_LOWER_MASK);
            s->mt[kk] = s->mt[kk + MT_M] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        for (; kk < MT_N - 1; kk++) {
            y = (s->mt[kk] & MT_UPPER_MASK) | (s->mt[kk + 1] & MT_LOWER_MASK);
            s->mt[kk] = s->mt[kk + (MT_M - MT_N)] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        y = (s->mt[MT_N - 1] & MT_UPPER_MASK) | (s->mt[0] & MT_LOWER_MASK);
        s->mt[MT_N - 1] = s->mt[MT_M - 1] ^ (y >> 1) ^ mag01[y & 0x1U];
        s->mti = 0;
    }
    y = s->mt[s->mti++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680U;
    y ^= (y << 15) & 0xefc60000U;
    y ^= (y >> 18);
    return y;
}

static double mt_random(TRandState* s) {
    uint32_t a = mt_uint32(s) >> 5;
    uint32_t b = mt_uint32(s) >> 6;
    return ((double)a * 67108864.0 + (double)b) * (1.0 / 9007199254740992.0);
}

static uint64_t mt_randbelow(TRandState* s, uint64_t n) {
    if (n == 0) return 0;
    int k = 0;
    uint64_t tmp = n;
    while (tmp) { k++; tmp >>= 1; }
    for (;;) {
        uint64_t r = 0;
        if (k <= 32) {
            r = mt_uint32(s) >> (32 - k);
        } else {
            uint64_t lo = mt_uint32(s);
            uint64_t hi = mt_uint32(s) >> (64 - k);
            r = (hi << 32) | lo;
        }
        if (r < n) return r;
    }
}

static TVal prod_rand_float(TRuntime* rt, void* ctx) {
    (void)rt;
    return thrum_num_dimless(mt_random((TRandState*)ctx));
}

typedef struct {
    TRandState* rng;
    int64_t lo;
    int64_t hi;
    uint64_t n;
    int k;
} TRandCall;

static TVal prod_rand_int(TRuntime* rt, void* ctx) {
    (void)rt;
    TRandCall* rc = (TRandCall*)ctx;
    uint64_t width = (uint64_t)(rc->hi - rc->lo + 1);
    return thrum_num_dimless((double)(rc->lo + (int64_t)mt_randbelow(rc->rng, width)));
}

static TVal prod_rand_pick(TRuntime* rt, void* ctx) {
    (void)rt;
    TRandCall* rc = (TRandCall*)ctx;
    return thrum_num_dimless((double)mt_randbelow(rc->rng, rc->n));
}

static TVal prod_rand_sample(TRuntime* rt, void* ctx) {
    TRandCall* rc = (TRandCall*)ctx;
    int n = (int)rc->n, k = rc->k;
    int* pool = (int*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    for (int i = 0; i < n; i++) pool[i] = i;
    T_FRAME(rt);
    T_VAR(rt, out, thrum_list(rt, (size_t)k));
    TList* l = (TList*)out.as.obj;
    for (int i = 0; i < k; i++) {
        int j = (int)mt_randbelow(rc->rng, (uint64_t)(n - i));
        thrum_list_push(rt, l, thrum_num_dimless((double)pool[j]));
        pool[j] = pool[n - i - 1];
    }
    free(pool);
    T_END_FRAME(rt);
    return out;
}

typedef struct {
    TRandState* rng;
    double mu;
    double sig;
} TRandGauss;

static TVal prod_rand_gauss(TRuntime* rt, void* ctx) {
    (void)rt;
    TRandGauss* rg = (TRandGauss*)ctx;
    TRandState* s = rg->rng;
    double z;
    if (s->has_gauss) {
        z = s->gauss_next;
        s->has_gauss = false;
    } else {
        double two_pi = 2.0 * 3.14159265358979323846;
        double x2pi = mt_random(s) * two_pi;
        double r2 = 1.0 - mt_random(s);
        double g2rad = sqrt(-2.0 * log(r2));
        z = cos(x2pi) * g2rad;
        s->gauss_next = sin(x2pi) * g2rad;
        s->has_gauss = true;
    }
    return thrum_num_dimless(rg->mu + z * rg->sig);
}

static TVal rand_float(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)argv; (void)line; (void)col;
    return thrum_journal_get(rt, "rand.float", "", prod_rand_float, ctx);
}

static TVal rand_int(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    int64_t lo = thrum_need_int(rt, argv[0], line, col);
    int64_t hi = thrum_need_int(rt, argv[1], line, col);
    if (hi < lo) thrum_error(rt, line, col, "empty range for randint", NULL);
    TRandCall rc = {(TRandState*)ctx, lo, hi, 0, 0};
    char key[64];
    snprintf(key, sizeof(key), "%lld,%lld", (long long)lo, (long long)hi);
    return thrum_journal_get(rt, "rand.int", key, prod_rand_int, &rc);
}

static TVal rand_pick(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    if (argv[0].kind != TV_OBJ || argv[0].as.obj->kind != OBJ_LIST || ((TList*)argv[0].as.obj)->len == 0) {
        thrum_error(rt, line, col, "pick needs a non-empty list", NULL);
    }
    TList* xs = (TList*)argv[0].as.obj;
    TRandCall rc = {(TRandState*)ctx, 0, 0, xs->len, 0};
    TVal idx = thrum_journal_get(rt, "rand.pick", "", prod_rand_pick, &rc);
    return xs->items[(size_t)idx.as.q.v];
}

static TVal rand_shuffle(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    if (argv[0].kind != TV_OBJ || argv[0].as.obj->kind != OBJ_LIST) {
        thrum_error(rt, line, col, "shuffle needs a list", NULL);
    }
    T_FRAME(rt);
    T_VAR(rt, xsv, argv[0]);
    TList* xs = (TList*)xsv.as.obj;
    TRandCall rc = {(TRandState*)ctx, 0, 0, xs->len, (int)xs->len};
    char key[32];
    snprintf(key, sizeof(key), "%zu", xs->len);
    T_VAR(rt, ord, thrum_journal_get(rt, "rand.shuffle", key, prod_rand_sample, &rc));
    TList* ol = (TList*)ord.as.obj;
    T_VAR(rt, out, thrum_list(rt, xs->len));
    TList* nl = (TList*)out.as.obj;
    for (size_t i = 0; i < ol->len; i++) {
        size_t ix = (size_t)ol->items[i].as.q.v;
        thrum_list_push(rt, nl, xs->items[ix]);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal rand_sample(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    if (argv[0].kind != TV_OBJ || argv[0].as.obj->kind != OBJ_LIST) {
        thrum_error(rt, line, col, "sample needs a list", NULL);
    }
    TList* xs = (TList*)argv[0].as.obj;
    int64_t k = thrum_need_int(rt, argv[1], line, col);
    if (k < 0 || (size_t)k > xs->len) {
        char msg[96];
        snprintf(msg, sizeof(msg), "sample count %lld out of range (len %zu)", (long long)k, xs->len);
        thrum_error(rt, line, col, msg, NULL);
    }
    T_FRAME(rt);
    T_VAR(rt, xsv, argv[0]);
    TRandCall rc = {(TRandState*)ctx, 0, 0, xs->len, (int)k};
    char key[64];
    snprintf(key, sizeof(key), "%zu,%lld", xs->len, (long long)k);
    T_VAR(rt, ord, thrum_journal_get(rt, "rand.sample", key, prod_rand_sample, &rc));
    TList* ol = (TList*)ord.as.obj;
    T_VAR(rt, out, thrum_list(rt, (size_t)k));
    TList* nl = (TList*)out.as.obj;
    for (size_t i = 0; i < ol->len; i++) {
        size_t ix = (size_t)ol->items[i].as.q.v;
        thrum_list_push(rt, nl, xs->items[ix]);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal rand_gauss(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ mu = thrum_need_q(rt, argv[0], line, col);
    TQ sig = thrum_need_q(rt, argv[1], line, col);
    if (!thrum_dim_eq(mu.d, sig.d)) {
        char m1[64], m2[64], msg[192];
        thrum_dimstr(mu.d, m1, sizeof(m1));
        thrum_dimstr(sig.d, m2, sizeof(m2));
        snprintf(msg, sizeof(msg), "rand.gauss mean and stdev dimensions must match: %s vs %s",
                 m1[0] ? m1 : "dimensionless", m2[0] ? m2 : "dimensionless");
        thrum_error(rt, line, col, msg, NULL);
    }
    TRandGauss rg = {(TRandState*)ctx, mu.v, sig.v};
    TVal rv = thrum_journal_get(rt, "rand.gauss", "", prod_rand_gauss, &rg);
    return thrum_qty(rt, rv.as.q.v, mu.u, mu.d, mu.disp_sc, mu.disp_sym);
}

static TCap* mk_cap_rand(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    TRandState* s = (TRandState*)calloc(1, sizeof(TRandState));
    if (argc > 0) {
        int64_t sv = thrum_need_int(rt, argv[0], line, col);
        uint64_t av = sv < 0 ? (uint64_t)(-sv) : (uint64_t)sv;
        mt_seed_u64(s, av);
    } else {
        uint32_t key[4] = {0};
        FILE* ur = fopen("/dev/urandom", "rb");
        if (ur) {
            if (fread(key, sizeof(uint32_t), 4, ur) != 4) key[0] = (uint32_t)time(NULL);
            fclose(ur);
        } else {
            key[0] = (uint32_t)time(NULL) ^ (uint32_t)getpid();
            key[1] = (uint32_t)clock();
        }
        mt_init_by_array(s, key, 4);
    }
    TCap* c = mk_cap(rt, "rand", NULL, free, s);
    cap_add(rt, c, "float", rand_float, 0, 0);
    cap_add(rt, c, "int", rand_int, 2, 2);
    cap_add(rt, c, "pick", rand_pick, 1, 1);
    cap_add(rt, c, "shuffle", rand_shuffle, 1, 1);
    cap_add(rt, c, "sample", rand_sample, 2, 2);
    cap_add(rt, c, "gauss", rand_gauss, 2, 2);
    return c;
}

/* ============================== cap_fs (Sandboxed Root) ============================== */
typedef struct {
    char* root;
} TFsState;

static void free_fs_state(void* p) {
    TFsState* fs = (TFsState*)p;
    if (fs) { free(fs->root); free(fs); }
}

static char* norm_abs_path(const char* raw) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/");
    size_t rlen = strlen(raw);
    char* full;
    if (raw[0] == '/') {
        full = strdup(raw);
    } else {
        full = (char*)malloc(strlen(cwd) + rlen + 2);
        sprintf(full, "%s/%s", cwd, raw);
    }
    char** segs = (char**)malloc(512 * sizeof(char*));
    int n = 0, cap = 512;
    char* save = NULL;
    char* tok = strtok_r(full, "/", &save);
    while (tok) {
        if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
            /* skip */
        } else if (strcmp(tok, "..") == 0) {
            if (n > 0) n--;
        } else {
            if (n >= cap) { cap *= 2; segs = (char**)realloc(segs, (size_t)cap * sizeof(char*)); }
            segs[n++] = tok;
        }
        tok = strtok_r(NULL, "/", &save);
    }
    CSB sb = {0};
    if (n == 0) csb_ch(&sb, '/');
    for (int i = 0; i < n; i++) {
        csb_ch(&sb, '/');
        csb_app(&sb, segs[i]);
    }
    free(segs);
    free(full);
    return sb.data;
}

static char* fs_resolve(TRuntime* rt, TFsState* st, TVal pval, int line, int col) {
    if (pval.kind != TV_OBJ || pval.as.obj->kind != OBJ_STR) {
        thrum_error(rt, line, col, "fs path must be text", NULL);
    }
    const char* p = ((TStr*)pval.as.obj)->data;
    char* joined;
    if (p[0] == '/') {
        joined = norm_abs_path(p);
    } else {
        size_t rl = strlen(st->root);
        char* tmp = (char*)malloc(rl + strlen(p) + 2);
        sprintf(tmp, "%s/%s", st->root, p);
        joined = norm_abs_path(tmp);
        free(tmp);
    }
    size_t rl = strlen(st->root);
    bool ok = (strcmp(joined, st->root) == 0) ||
              (strncmp(joined, st->root, rl) == 0 && (rl == 1 || joined[rl] == '/'));
    if (!ok) {
        free(joined);
        char msg[512];
        snprintf(msg, sizeof(msg), "path escapes the granted root '%s'", st->root);
        thrum_error(rt, line, col, msg, NULL);
    }
    return joined;
}

static void ensure_parent_dirs(const char* path) {
    char* copy = strdup(path);
    char* slash = strrchr(copy, '/');
    if (slash && slash != copy) {
        *slash = '\0';
        for (char* p = copy + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(copy, 0755);
                *p = '/';
            }
        }
        mkdir(copy, 0755);
    }
    free(copy);
}

typedef struct {
    const char* path;
    const char* content;
    int line;
    int col;
} TFsOp;

static TVal prod_fs_read(TRuntime* rt, void* ctx) {
    TFsOp* op = (TFsOp*)ctx;
    FILE* fh = fopen(op->path, "rb");
    if (!fh) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot read '%s': %s", op->path, strerror(errno));
        thrum_error(rt, op->line, op->col, msg, NULL);
    }
    fseek(fh, 0, SEEK_END);
    long sz = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)(sz > 0 ? sz : 1) + 1);
    size_t nr = sz > 0 ? fread(buf, 1, (size_t)sz, fh) : 0;
    fclose(fh);
    TVal out = thrum_str(rt, buf, nr);
    free(buf);
    return out;
}

static TVal prod_fs_write(TRuntime* rt, void* ctx) {
    TFsOp* op = (TFsOp*)ctx;
    ensure_parent_dirs(op->path);
    FILE* fh = fopen(op->path, "wb");
    if (!fh) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot write '%s': %s", op->path, strerror(errno));
        thrum_error(rt, op->line, op->col, msg, NULL);
    }
    fputs(op->content, fh);
    fclose(fh);
    return thrum_cstr(rt, op->content);
}

static TVal prod_fs_append(TRuntime* rt, void* ctx) {
    TFsOp* op = (TFsOp*)ctx;
    ensure_parent_dirs(op->path);
    FILE* fh = fopen(op->path, "ab");
    if (!fh) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot append '%s': %s", op->path, strerror(errno));
        thrum_error(rt, op->line, op->col, msg, NULL);
    }
    fputs(op->content, fh);
    fclose(fh);
    return thrum_cstr(rt, op->content);
}

static TVal prod_fs_exists(TRuntime* rt, void* ctx) {
    (void)rt;
    struct stat st;
    return thrum_bool(stat(((TFsOp*)ctx)->path, &st) == 0);
}

static TVal prod_fs_isfile(TRuntime* rt, void* ctx) {
    (void)rt;
    struct stat st;
    return thrum_bool(stat(((TFsOp*)ctx)->path, &st) == 0 && S_ISREG(st.st_mode));
}

static TVal prod_fs_isdir(TRuntime* rt, void* ctx) {
    (void)rt;
    struct stat st;
    return thrum_bool(stat(((TFsOp*)ctx)->path, &st) == 0 && S_ISDIR(st.st_mode));
}

static int cmp_cstr_ptr(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static TVal prod_fs_list(TRuntime* rt, void* ctx) {
    TFsOp* op = (TFsOp*)ctx;
    DIR* d = opendir(op->path);
    if (!d) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot list '%s': %s", op->path, strerror(errno));
        thrum_error(rt, op->line, op->col, msg, NULL);
    }
    char** names = NULL;
    size_t n = 0, cap = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (n >= cap) { cap = cap ? cap * 2 : 16; names = (char**)realloc(names, cap * sizeof(char*)); }
        names[n++] = strdup(ent->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof(char*), cmp_cstr_ptr);
    T_FRAME(rt);
    T_VAR(rt, lst, thrum_list(rt, n));
    TList* l = (TList*)lst.as.obj;
    for (size_t i = 0; i < n; i++) {
        thrum_list_push(rt, l, thrum_cstr(rt, names[i]));
        free(names[i]);
    }
    free(names);
    T_END_FRAME(rt);
    return lst;
}

static TVal prod_fs_mkdir(TRuntime* rt, void* ctx) {
    TFsOp* op = (TFsOp*)ctx;
    char* copy = strdup(op->path);
    for (char* p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(copy, 0755);
            *p = '/';
        }
    }
    mkdir(copy, 0755);
    free(copy);
    (void)rt;
    return thrum_bool(true);
}

static TVal prod_fs_remove(TRuntime* rt, void* ctx) {
    TFsOp* op = (TFsOp*)ctx;
    struct stat st;
    if (lstat(op->path, &st) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot remove missing path '%s'", op->content ? op->content : op->path);
        thrum_error(rt, op->line, op->col, msg, NULL);
    }
    if (S_ISDIR(st.st_mode)) {
        if (rmdir(op->path) != 0) thrum_error(rt, op->line, op->col, strerror(errno), NULL);
    } else {
        if (unlink(op->path) != 0) thrum_error(rt, op->line, op->col, strerror(errno), NULL);
    }
    return thrum_bool(true);
}

static TVal fs_read(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    char* p = fs_resolve(rt, (TFsState*)ctx, argv[0], line, col);
    TFsOp op = {p, NULL, line, col};
    TVal out = thrum_journal_get(rt, "fs.read", p, prod_fs_read, &op);
    free(p);
    return out;
}

static TVal fs_read_lines(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    T_FRAME(rt);
    T_VAR(rt, txtv, fs_read(rt, ctx, argc, argv, line, col));
    TStr* s = (TStr*)txtv.as.obj;
    T_VAR(rt, out, thrum_list(rt, 8));
    TList* l = (TList*)out.as.obj;
    size_t i = 0;
    while (i < s->len) {
        size_t start = i;
        while (i < s->len && s->data[i] != '\n' && s->data[i] != '\r') i++;
        thrum_list_push(rt, l, thrum_str(rt, s->data + start, i - start));
        if (i < s->len && s->data[i] == '\r') i++;
        if (i < s->len && s->data[i] == '\n') i++;
    }
    T_END_FRAME(rt);
    return out;
}

static TVal fs_write(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    char* p = fs_resolve(rt, (TFsState*)ctx, argv[0], line, col);
    if (argv[1].kind != TV_OBJ || argv[1].as.obj->kind != OBJ_STR) {
        free(p);
        thrum_error(rt, line, col, "fs.write content must be text", NULL);
    }
    const char* txt = ((TStr*)argv[1].as.obj)->data;
    TFsOp op = {p, txt, line, col};
    TVal rec = thrum_journal_get(rt, "fs.write", p, prod_fs_write, &op);
    if (rt->journal_replay) {
        char* rs = thrum_vstr_alloc(rt, rec, false);
        if (rec.kind != TV_OBJ || rec.as.obj->kind != OBJ_STR || strcmp(rs, txt) != 0) rt->jdiv++;
        free(rs);
    }
    free(p);
    return thrum_nil();
}

static TVal fs_append(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    char* p = fs_resolve(rt, (TFsState*)ctx, argv[0], line, col);
    if (argv[1].kind != TV_OBJ || argv[1].as.obj->kind != OBJ_STR) {
        free(p);
        thrum_error(rt, line, col, "fs.append content must be text", NULL);
    }
    const char* txt = ((TStr*)argv[1].as.obj)->data;
    TFsOp op = {p, txt, line, col};
    TVal rec = thrum_journal_get(rt, "fs.append", p, prod_fs_append, &op);
    if (rt->journal_replay) {
        char* rs = thrum_vstr_alloc(rt, rec, false);
        if (rec.kind != TV_OBJ || rec.as.obj->kind != OBJ_STR || strcmp(rs, txt) != 0) rt->jdiv++;
        free(rs);
    }
    free(p);
    return thrum_nil();
}

static TVal fs_write_lines(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    if (argv[1].kind != TV_OBJ || argv[1].as.obj->kind != OBJ_LIST) {
        thrum_error(rt, line, col, "fs.write_lines needs a list", NULL);
    }
    TList* l = (TList*)argv[1].as.obj;
    CSB sb = {0};
    for (size_t i = 0; i < l->len; i++) {
        if (i > 0) csb_ch(&sb, '\n');
        char* s = thrum_vstr_alloc(rt, l->items[i], false);
        csb_app(&sb, s);
        free(s);
    }
    if (l->len > 0) csb_ch(&sb, '\n');
    T_FRAME(rt);
    T_VAR(rt, tv, thrum_str(rt, sb.data ? sb.data : "", sb.len));
    free(sb.data);
    TVal args[2] = {argv[0], tv};
    fs_write(rt, ctx, 2, args, line, col);
    T_END_FRAME(rt);
    return thrum_nil();
}

static TVal fs_exists(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    char* p = fs_resolve(rt, (TFsState*)ctx, argv[0], line, col);
    TFsOp op = {p, NULL, line, col};
    TVal out = thrum_journal_get(rt, "fs.exists", p, prod_fs_exists, &op);
    free(p);
    return out;
}

static TVal fs_is_file(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    char* p = fs_resolve(rt, (TFsState*)ctx, argv[0], line, col);
    TFsOp op = {p, NULL, line, col};
    TVal out = thrum_journal_get(rt, "fs.is_file", p, prod_fs_isfile, &op);
    free(p);
    return out;
}

static TVal fs_is_dir(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    char* p = fs_resolve(rt, (TFsState*)ctx, argv[0], line, col);
    TFsOp op = {p, NULL, line, col};
    TVal out = thrum_journal_get(rt, "fs.is_dir", p, prod_fs_isdir, &op);
    free(p);
    return out;
}

static TVal fs_list(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    TVal pv = argc > 0 ? argv[0] : thrum_cstr(rt, ".");
    char* p = fs_resolve(rt, (TFsState*)ctx, pv, line, col);
    TFsOp op = {p, NULL, line, col};
    TVal out = thrum_journal_get(rt, "fs.list", p, prod_fs_list, &op);
    free(p);
    return out;
}

static TVal fs_mkdir(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    char* p = fs_resolve(rt, (TFsState*)ctx, argv[0], line, col);
    TFsOp op = {p, NULL, line, col};
    thrum_journal_get(rt, "fs.mkdir", p, prod_fs_mkdir, &op);
    free(p);
    return thrum_nil();
}

static TVal fs_remove(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    char* p = fs_resolve(rt, (TFsState*)ctx, argv[0], line, col);
    const char* orig = ((TStr*)argv[0].as.obj)->data;
    TFsOp op = {p, orig, line, col};
    thrum_journal_get(rt, "fs.remove", p, prod_fs_remove, &op);
    free(p);
    return thrum_nil();
}

static TCap* mk_cap_fs(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    const char* raw_root = ".";
    if (argc > 0) {
        if (argv[0].kind != TV_OBJ || argv[0].as.obj->kind != OBJ_STR) {
            thrum_error(rt, line, col, "fs root path must be text", NULL);
        }
        raw_root = ((TStr*)argv[0].as.obj)->data;
    }
    TFsState* st = (TFsState*)calloc(1, sizeof(TFsState));
    st->root = norm_abs_path(raw_root);
    TCap* c = mk_cap(rt, "fs", NULL, free_fs_state, st);
    cap_add(rt, c, "read", fs_read, 1, 1);
    cap_add(rt, c, "read_lines", fs_read_lines, 1, 1);
    cap_add(rt, c, "write", fs_write, 2, 2);
    cap_add(rt, c, "write_lines", fs_write_lines, 2, 2);
    cap_add(rt, c, "append", fs_append, 2, 2);
    cap_add(rt, c, "exists", fs_exists, 1, 1);
    cap_add(rt, c, "is_file", fs_is_file, 1, 1);
    cap_add(rt, c, "is_dir", fs_is_dir, 1, 1);
    cap_add(rt, c, "list", fs_list, 0, 1);
    cap_add(rt, c, "mkdir", fs_mkdir, 1, 1);
    cap_add(rt, c, "remove", fs_remove, 1, 1);
    return c;
}

/* ============================== cap_env ============================== */
typedef struct {
    const char* name;
    const char* def;
} TEnvReq;

static TVal prod_env_get(TRuntime* rt, void* ctx) {
    TEnvReq* req = (TEnvReq*)ctx;
    const char* v = getenv(req->name);
    return thrum_cstr(rt, v ? v : (req->def ? req->def : ""));
}

static TVal prod_env_has(TRuntime* rt, void* ctx) {
    (void)rt;
    return thrum_bool(getenv(((TEnvReq*)ctx)->name) != NULL);
}

static TVal prod_env_keys(TRuntime* rt, void* ctx) {
    (void)ctx;
    char** names = NULL;
    size_t n = 0, cap = 0;
    for (char** ep = environ; ep && *ep; ep++) {
        const char* eq = strchr(*ep, '=');
        size_t kl = eq ? (size_t)(eq - *ep) : strlen(*ep);
        if (n >= cap) { cap = cap ? cap * 2 : 32; names = (char**)realloc(names, cap * sizeof(char*)); }
        char* k = (char*)malloc(kl + 1);
        memcpy(k, *ep, kl);
        k[kl] = '\0';
        names[n++] = k;
    }
    qsort(names, n, sizeof(char*), cmp_cstr_ptr);
    T_FRAME(rt);
    T_VAR(rt, lst, thrum_list(rt, n));
    TList* l = (TList*)lst.as.obj;
    for (size_t i = 0; i < n; i++) {
        thrum_list_push(rt, l, thrum_cstr(rt, names[i]));
        free(names[i]);
    }
    free(names);
    T_END_FRAME(rt);
    return lst;
}

static TVal env_get(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx;
    if (argv[0].kind != TV_OBJ || argv[0].as.obj->kind != OBJ_STR) {
        thrum_error(rt, line, col, "env.get name must be text", NULL);
    }
    const char* nm = ((TStr*)argv[0].as.obj)->data;
    char* def = (argc > 1) ? thrum_vstr_alloc(rt, argv[1], false) : strdup("");
    TEnvReq req = {nm, def};
    TVal out = thrum_journal_get(rt, "env.get", nm, prod_env_get, &req);
    free(def);
    return out;
}

static TVal env_has(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)argc;
    if (argv[0].kind != TV_OBJ || argv[0].as.obj->kind != OBJ_STR) {
        thrum_error(rt, line, col, "env.has name must be text", NULL);
    }
    const char* nm = ((TStr*)argv[0].as.obj)->data;
    TEnvReq req = {nm, NULL};
    return thrum_journal_get(rt, "env.has", nm, prod_env_has, &req);
}

static TVal env_keys(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)argc; (void)argv; (void)line; (void)col;
    return thrum_journal_get(rt, "env.keys", "", prod_env_keys, NULL);
}

static TCap* mk_cap_env(TRuntime* rt) {
    TCap* c = mk_cap(rt, "env", NULL, NULL, NULL);
    cap_add(rt, c, "get", env_get, 1, 2);
    cap_add(rt, c, "has", env_has, 1, 1);
    cap_add(rt, c, "keys", env_keys, 0, 0);
    return c;
}

/* ============================== cap_args ============================== */
static TVal prod_args_all(TRuntime* rt, void* ctx) {
    (void)ctx;
    T_FRAME(rt);
    T_VAR(rt, lst, thrum_list(rt, (size_t)rt->argc));
    TList* l = (TList*)lst.as.obj;
    for (int i = 0; i < rt->argc; i++) {
        thrum_list_push(rt, l, thrum_cstr(rt, rt->argv[i]));
    }
    T_END_FRAME(rt);
    return lst;
}

static TVal args_all(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)argc; (void)argv; (void)line; (void)col;
    return thrum_journal_get(rt, "args.all", "", prod_args_all, NULL);
}

static TVal args_get(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx;
    int64_t idx = thrum_need_int(rt, argv[0], line, col);
    TVal def = argc > 1 ? argv[1] : thrum_nil();
    T_FRAME(rt);
    T_VAR(rt, defv, def);
    T_VAR(rt, xs, thrum_journal_get(rt, "args.all", "", prod_args_all, NULL));
    TList* l = (TList*)xs.as.obj;
    TVal out = (idx >= 0 && (size_t)idx < l->len) ? l->items[idx] : defv;
    T_END_FRAME(rt);
    return out;
}

static TVal args_len(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)argc; (void)argv; (void)line; (void)col;
    T_FRAME(rt);
    T_VAR(rt, xs, thrum_journal_get(rt, "args.all", "", prod_args_all, NULL));
    double n = (double)((TList*)xs.as.obj)->len;
    T_END_FRAME(rt);
    return thrum_num_dimless(n);
}

static TCap* mk_cap_args(TRuntime* rt) {
    TCap* c = mk_cap(rt, "args", NULL, NULL, NULL);
    cap_add(rt, c, "all", args_all, 0, 0);
    cap_add(rt, c, "get", args_get, 1, 2);
    cap_add(rt, c, "len", args_len, 0, 0);
    return c;
}

/* ============================== cap_tty ============================== */
typedef struct {
    int fd;
    bool has_saved;
    struct termios saved;
    bool cursor_hidden;
} TTtyState;

static bool tty_is_live(TRuntime* rt) {
    FILE* out = rt->out ? rt->out : stdout;
    return isatty(STDIN_FILENO) && isatty(fileno(out));
}

static TVal prod_tty_is_tty(TRuntime* rt, void* ctx) {
    (void)ctx;
    return thrum_bool(tty_is_live(rt));
}

static TVal tty_is_tty(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)argv; (void)line; (void)col;
    return thrum_journal_get(rt, "tty.is_tty", "", prod_tty_is_tty, ctx);
}

static TVal tty_enter_raw(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)argv; (void)line; (void)col;
    TTtyState* st = (TTtyState*)ctx;
    if (rt->journal_replay || !tty_is_live(rt)) return thrum_bool(false);
    if (!st->has_saved) {
        if (tcgetattr(STDIN_FILENO, &st->saved) == 0) {
            struct termios raw = st->saved;
            raw.c_lflag &= (tcflag_t)~(ECHO | ICANON);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
            st->fd = STDIN_FILENO;
            st->has_saved = true;
        }
    }
    return thrum_bool(true);
}

static TVal tty_exit_raw(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)rt; (void)argc; (void)argv; (void)line; (void)col;
    TTtyState* st = (TTtyState*)ctx;
    if (st->has_saved) {
        tcsetattr(st->fd, TCSADRAIN, &st->saved);
        st->has_saved = false;
    }
    return thrum_nil();
}

static TVal tty_hide_cursor(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)argv; (void)line; (void)col;
    TTtyState* st = (TTtyState*)ctx;
    if (!rt->journal_replay && tty_is_live(rt)) {
        FILE* out = rt->out ? rt->out : stdout;
        fputs("\x1b[?25l", out);
        fflush(out);
        st->cursor_hidden = true;
    }
    return thrum_nil();
}

static TVal tty_show_cursor(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)argv; (void)line; (void)col;
    TTtyState* st = (TTtyState*)ctx;
    if (st->cursor_hidden) {
        FILE* out = rt->out ? rt->out : stdout;
        fputs("\x1b[?25h", out);
        fflush(out);
        st->cursor_hidden = false;
    }
    return thrum_nil();
}

static void tty_cleanup(TRuntime* rt, void* state) {
    if (!state) return;
    tty_exit_raw(rt, state, 0, NULL, 0, 0);
    tty_show_cursor(rt, state, 0, NULL, 0, 0);
}

static TVal prod_true(TRuntime* rt, void* ctx) {
    (void)rt; (void)ctx;
    return thrum_bool(true);
}

static TVal tty_clear(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)argc; (void)argv; (void)line; (void)col;
    thrum_journal_get(rt, "tty.clear", "", prod_true, NULL);
    FILE* out = rt->out ? rt->out : stdout;
    fputs("\x1b[2J\x1b[H", out);
    fflush(out);
    return thrum_nil();
}

static TVal tty_write(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)line; (void)col;
    CSB sb = {0};
    for (int i = 0; i < argc; i++) {
        char* s = thrum_vstr_alloc(rt, argv[i], false);
        csb_app(&sb, s);
        free(s);
    }
    const char* txt = sb.data ? sb.data : "";
    record_out_divergence(rt, "tty.write", txt);
    FILE* out = rt->out ? rt->out : stdout;
    fputs(txt, out);
    fflush(out);
    free(sb.data);
    return thrum_nil();
}

static TVal prod_tty_size(TRuntime* rt, void* ctx) {
    (void)ctx;
    int cols = 80, rows = 24;
    struct winsize ws;
    FILE* out = rt->out ? rt->out : stdout;
    if (ioctl(fileno(out), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    T_FRAME(rt);
    T_VAR(rt, mp, thrum_map(rt));
    TMap* m = (TMap*)mp.as.obj;
    thrum_map_set_cs(rt, m, "cols", thrum_num_dimless((double)cols));
    thrum_map_set_cs(rt, m, "rows", thrum_num_dimless((double)rows));
    T_END_FRAME(rt);
    return mp;
}

static TVal tty_size(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)argv; (void)line; (void)col;
    return thrum_journal_get(rt, "tty.size", "", prod_tty_size, ctx);
}

static bool fd_ready(int fd, double timeout_sec) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    if (timeout_sec < 0.0) timeout_sec = 0.0;
    struct timeval tv;
    tv.tv_sec = (time_t)floor(timeout_sec);
    tv.tv_usec = (suseconds_t)((timeout_sec - (double)tv.tv_sec) * 1e6);
    int r = select(fd + 1, &rfds, NULL, NULL, &tv);
    return r > 0 && FD_ISSET(fd, &rfds);
}

static TVal prod_tty_poll(TRuntime* rt, void* ctx) {
    double timeout_sec = *(double*)ctx;
    if (!tty_is_live(rt)) return thrum_nil();
    int fd = STDIN_FILENO;
    if (!fd_ready(fd, timeout_sec)) return thrum_nil();
    unsigned char ch = 0;
    if (read(fd, &ch, 1) <= 0) return thrum_nil();
    if (ch == 0x1b) {
        if (!fd_ready(fd, 0.015)) return thrum_cstr(rt, "esc");
        unsigned char b2 = 0;
        if (read(fd, &b2, 1) <= 0) return thrum_cstr(rt, "esc");
        if (b2 == '[') {
            if (!fd_ready(fd, 0.015)) return thrum_cstr(rt, "esc");
            unsigned char b3 = 0;
            if (read(fd, &b3, 1) <= 0) return thrum_cstr(rt, "esc");
            switch (b3) {
                case 'A': return thrum_cstr(rt, "up");
                case 'B': return thrum_cstr(rt, "down");
                case 'C': return thrum_cstr(rt, "right");
                case 'D': return thrum_cstr(rt, "left");
                default: return thrum_cstr(rt, "esc");
            }
        }
        return thrum_cstr(rt, "esc");
    }
    if (ch == ' ') return thrum_cstr(rt, "space");
    if (ch == '\r' || ch == '\n') return thrum_cstr(rt, "enter");
    if (ch == 0x7f || ch == 0x08) return thrum_cstr(rt, "backspace");
    if (ch == '\t') return thrum_cstr(rt, "tab");
    char buf[5];
    size_t len = 1;
    buf[0] = (char)ch;
    /* Read remaining UTF-8 continuation bytes if multi-byte char */
    if ((ch & 0xE0) == 0xC0) len = 2;
    else if ((ch & 0xF0) == 0xE0) len = 3;
    else if ((ch & 0xF8) == 0xF0) len = 4;
    for (size_t k = 1; k < len; k++) {
        if (!fd_ready(fd, 0.01) || read(fd, &buf[k], 1) <= 0) { len = k; break; }
    }
    return thrum_str(rt, buf, len);
}

static TVal tty_poll_key(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx;
    double timeout_sec = 0.05;
    if (argc > 0) {
        TQ sec = thrum_need_q(rt, argv[0], line, col);
        if (!thrum_dim_eq(sec.d, thrum_dim(0, 0, 1, 0, 0, 0, 0))) {
            thrum_error(rt, line, col, "tty.poll_key timeout expects a duration (s)", NULL);
        }
        timeout_sec = sec.v;
    }
    return thrum_journal_get(rt, "tty.poll_key", "", prod_tty_poll, &timeout_sec);
}

static TCap* mk_cap_tty(TRuntime* rt) {
    TTtyState* st = (TTtyState*)calloc(1, sizeof(TTtyState));
    TCap* c = mk_cap(rt, "tty", tty_cleanup, free, st);
    cap_add(rt, c, "is_tty", tty_is_tty, 0, 0);
    cap_add(rt, c, "enter_raw", tty_enter_raw, 0, 0);
    cap_add(rt, c, "exit_raw", tty_exit_raw, 0, 0);
    cap_add(rt, c, "hide_cursor", tty_hide_cursor, 0, 0);
    cap_add(rt, c, "show_cursor", tty_show_cursor, 0, 0);
    cap_add(rt, c, "clear", tty_clear, 0, 0);
    cap_add(rt, c, "write", tty_write, 0, -1);
    cap_add(rt, c, "size", tty_size, 0, 0);
    cap_add(rt, c, "poll_key", tty_poll_key, 0, 1);
    return c;
}

/* ============================== cap_proc ============================== */
typedef struct {
    char** argv;
    int argc;
    const char* cwd;
    int line;
    int col;
} TProcReq;

static void read_fd_all(int fd, CSB* sb) {
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        csb_app_n(sb, buf, (size_t)n);
    }
}

static TVal prod_proc_run(TRuntime* rt, void* ctx) {
    TProcReq* pr = (TProcReq*)ctx;
    int out_p[2], err_p[2];
    if (pipe(out_p) != 0 || pipe(err_p) != 0) {
        thrum_error(rt, pr->line, pr->col, "pipe failed", NULL);
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(out_p[0]); close(out_p[1]);
        close(err_p[0]); close(err_p[1]);
        thrum_error(rt, pr->line, pr->col, "fork failed", NULL);
    }
    if (pid == 0) {
        dup2(out_p[1], STDOUT_FILENO);
        dup2(err_p[1], STDERR_FILENO);
        close(out_p[0]); close(out_p[1]);
        close(err_p[0]); close(err_p[1]);
        if (pr->cwd && pr->cwd[0]) {
            if (chdir(pr->cwd) != 0) _exit(127);
        }
        execvp(pr->argv[0], pr->argv);
        fprintf(stderr, "execvp('%s'): %s\n", pr->argv[0], strerror(errno));
        _exit(127);
    }
    close(out_p[1]);
    close(err_p[1]);
    CSB out_b = {0}, err_b = {0};
    /* Read both pipes using select to prevent deadlock on large output */
    int open_out = out_p[0], open_err = err_p[0];
    while (open_out >= 0 || open_err >= 0) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (open_out >= 0) { FD_SET(open_out, &rfds); if (open_out > maxfd) maxfd = open_out; }
        if (open_err >= 0) { FD_SET(open_err, &rfds); if (open_err > maxfd) maxfd = open_err; }
        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char buf[4096];
        if (open_out >= 0 && FD_ISSET(open_out, &rfds)) {
            ssize_t n = read(open_out, buf, sizeof(buf));
            if (n > 0) csb_app_n(&out_b, buf, (size_t)n);
            else { close(open_out); open_out = -1; }
        }
        if (open_err >= 0 && FD_ISSET(open_err, &rfds)) {
            ssize_t n = read(open_err, buf, sizeof(buf));
            if (n > 0) csb_app_n(&err_b, buf, (size_t)n);
            else { close(open_err); open_err = -1; }
        }
    }
    if (open_out >= 0) { read_fd_all(open_out, &out_b); close(open_out); }
    if (open_err >= 0) { read_fd_all(open_err, &err_b); close(open_err); }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);

    T_FRAME(rt);
    T_VAR(rt, mp, thrum_map(rt));
    TMap* m = (TMap*)mp.as.obj;
    thrum_map_set_cs(rt, m, "code", thrum_num_dimless((double)code));
    thrum_map_set_cs(rt, m, "stdout", thrum_str(rt, out_b.data ? out_b.data : "", out_b.len));
    thrum_map_set_cs(rt, m, "stderr", thrum_str(rt, err_b.data ? err_b.data : "", err_b.len));
    free(out_b.data);
    free(err_b.data);
    T_END_FRAME(rt);
    return mp;
}

static TVal proc_run(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx;
    if (argv[0].kind != TV_OBJ || argv[0].as.obj->kind != OBJ_LIST || ((TList*)argv[0].as.obj)->len == 0) {
        thrum_error(rt, line, col, "proc.run expects a non-empty list of text arguments", NULL);
    }
    TList* l = (TList*)argv[0].as.obj;
    for (size_t i = 0; i < l->len; i++) {
        if (l->items[i].kind != TV_OBJ || l->items[i].as.obj->kind != OBJ_STR) {
            thrum_error(rt, line, col, "proc.run expects a non-empty list of text arguments", NULL);
        }
    }
    const char* cwd = NULL;
    if (argc > 1) {
        if (argv[1].kind != TV_OBJ || argv[1].as.obj->kind != OBJ_STR) {
            thrum_error(rt, line, col, "proc.run cwd must be text", NULL);
        }
        cwd = ((TStr*)argv[1].as.obj)->data;
    }
    char** cargs = (char**)malloc((l->len + 1) * sizeof(char*));
    CSB keyb = {0};
    for (size_t i = 0; i < l->len; i++) {
        cargs[i] = ((TStr*)l->items[i].as.obj)->data;
        if (i > 0) csb_ch(&keyb, ' ');
        csb_app(&keyb, cargs[i]);
    }
    cargs[l->len] = NULL;
    TProcReq pr = {cargs, (int)l->len, cwd, line, col};
    TVal out = thrum_journal_get(rt, "proc.run", keyb.data ? keyb.data : "", prod_proc_run, &pr);
    free(keyb.data);
    free(cargs);
    return out;
}

static TVal proc_exe(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)argc; (void)argv; (void)line; (void)col;
    char buf[4096];
#ifdef __APPLE__
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0) {
        char* real = realpath(buf, NULL);
        if (real) {
            TVal out = thrum_cstr(rt, real);
            free(real);
            return out;
        }
        return thrum_cstr(rt, buf);
    }
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return thrum_cstr(rt, buf);
    }
#endif
    char* fallback = norm_abs_path(rt->exe_path && rt->exe_path[0] ? rt->exe_path : "thrumc");
    TVal out = thrum_cstr(rt, fallback);
    free(fallback);
    return out;
}

static TVal proc_cwd(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)argc; (void)argv; (void)line; (void)col;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) strcpy(buf, "/");
    return thrum_cstr(rt, buf);
}

static TVal proc_exit(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx;
    int code = argc > 0 ? (int)thrum_need_int(rt, argv[0], line, col) : 0;
    thrum_finish(rt);
    exit(code);
}

static TCap* mk_cap_proc(TRuntime* rt) {
    TCap* c = mk_cap(rt, "proc", NULL, NULL, NULL);
    cap_add(rt, c, "run", proc_run, 1, 2);
    cap_add(rt, c, "exe", proc_exe, 0, 0);
    cap_add(rt, c, "cwd", proc_cwd, 0, 0);
    cap_add(rt, c, "exit", proc_exit, 0, 1);
    return c;
}

/* ============================== Dispatch ============================== */
TVal thrum_get_cap(TRuntime* rt, const char* name, int argc, TVal* argv, int line, int col) {
    if (strcmp(name, "io") == 0) return thrum_obj((TObj*)mk_cap_io(rt));
    if (strcmp(name, "clock") == 0) return thrum_obj((TObj*)mk_cap_clock(rt));
    if (strcmp(name, "rand") == 0) return thrum_obj((TObj*)mk_cap_rand(rt, argc, argv, line, col));
    if (strcmp(name, "fs") == 0) return thrum_obj((TObj*)mk_cap_fs(rt, argc, argv, line, col));
    if (strcmp(name, "env") == 0) return thrum_obj((TObj*)mk_cap_env(rt));
    if (strcmp(name, "args") == 0) return thrum_obj((TObj*)mk_cap_args(rt));
    if (strcmp(name, "tty") == 0) return thrum_obj((TObj*)mk_cap_tty(rt));
    if (strcmp(name, "proc") == 0) return thrum_obj((TObj*)mk_cap_proc(rt));
    if (strcmp(name, "gui") == 0) return thrum_cap_gui_create(rt, line, col);
    if (strcmp(name, "net") == 0) return thrum_cap_net_create(rt, line, col);
    char msg[160];
    snprintf(msg, sizeof(msg), "unknown capability '%s'", name);
    thrum_error(rt, line, col, msg, "available: args, clock, env, fs, gui, io, net, proc, rand, tty");
}
