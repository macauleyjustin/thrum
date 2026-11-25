#ifndef __APPLE__
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include "thrum_rt.h"
#include <ctype.h>
#include <regex.h>

/* ============================== Helpers ============================== */
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} BSB;

static void bsb_app_n(BSB* sb, const char* s, size_t n) {
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

static void bsb_app(BSB* sb, const char* s) {
    if (s) bsb_app_n(sb, s, strlen(s));
}

static void bsb_ch(BSB* sb, char c) {
    bsb_app_n(sb, &c, 1);
}

static TList* need_list(TRuntime* rt, TVal v, const char* what, int line, int col) {
    if (v.kind != TV_OBJ || v.as.obj->kind != OBJ_LIST) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s needs a list, got %s", what, thrum_tname(v));
        thrum_error(rt, line, col, msg, NULL);
    }
    return (TList*)v.as.obj;
}

static TMap* need_map(TRuntime* rt, TVal v, const char* what, int line, int col) {
    if (v.kind != TV_OBJ || v.as.obj->kind != OBJ_MAP) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s needs a map, got %s", what, thrum_tname(v));
        thrum_error(rt, line, col, msg, NULL);
    }
    return (TMap*)v.as.obj;
}

static TStr* need_str(TRuntime* rt, TVal v, const char* what, int line, int col) {
    if (v.kind != TV_OBJ || v.as.obj->kind != OBJ_STR) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s needs text, got %s", what, thrum_tname(v));
        thrum_error(rt, line, col, msg, NULL);
    }
    return (TStr*)v.as.obj;
}

static void reg_builtin(TRuntime* rt, const char* name, TBuiltinPtr fn, int min_a, int max_a) {
    T_FRAME(rt);
    T_VAR(rt, bv, thrum_make_builtin(rt, name, fn, min_a, max_a));
    thrum_def_pin(rt, rt->builtins, name, bv);
    T_END_FRAME(rt);
}

static void mod_add(TRuntime* rt, TEnv* sc, const char* mod, const char* name, TBuiltinPtr fn, int min_a, int max_a) {
    T_FRAME(rt);
    char full[96];
    snprintf(full, sizeof(full), "%s.%s", mod, name);
    T_VAR(rt, bv, thrum_make_builtin(rt, full, fn, min_a, max_a));
    thrum_def_pin(rt, sc, name, bv);
    T_END_FRAME(rt);
}

static void mod_add_val(TRuntime* rt, TEnv* sc, const char* name, TVal v) {
    thrum_def_pin(rt, sc, name, v);
}

static void mod_alias_builtin(TRuntime* rt, TEnv* sc, const char* name, const char* bname, int line, int col) {
    T_FRAME(rt);
    T_VAR(rt, bv, thrum_read(rt, rt->builtins, bname, line, col));
    thrum_def_pin(rt, sc, name, bv);
    T_END_FRAME(rt);
}

/* ============================== Core Builtins ============================== */
static TVal bi_len(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    if (argv[0].kind == TV_OBJ) {
        TObj* o = argv[0].as.obj;
        if (o->kind == OBJ_LIST) return thrum_num_dimless((double)((TList*)o)->len);
        if (o->kind == OBJ_STR) return thrum_num_dimless((double)((TStr*)o)->ulen);
        if (o->kind == OBJ_MAP) return thrum_num_dimless((double)((TMap*)o)->count);
    }
    thrum_error(rt, line, col, "len needs a list, text or map", NULL);
}

static TVal bi_type(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)line; (void)col;
    return thrum_cstr(rt, thrum_tname(argv[0]));
}

static TVal bi_str(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)line; (void)col;
    return thrum_vstr_val(rt, argv[0], false);
}

static TVal bi_repr(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)line; (void)col;
    return thrum_vstr_val(rt, argv[0], true);
}

static TVal bi_num(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    if (argc == 2) {
        TStr* us = need_str(rt, argv[1], "convert", line, col);
        return thrum_convert(rt, argv[0], us->data, line, col);
    }
    if (argv[0].kind == TV_NUM) return argv[0];
    if (argv[0].kind == TV_OBJ && argv[0].as.obj->kind == OBJ_STR) {
        TStr* s = (TStr*)argv[0].as.obj;
        const char* p = s->data;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p) {
            char* end = NULL;
            double v = strtod(p, &end);
            while (end && *end && isspace((unsigned char)*end)) end++;
            if (end && end != p && *end == '\0') return thrum_num_dimless(v);
        }
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "cannot convert %s to num", thrum_tname(argv[0]));
    thrum_error(rt, line, col, msg, NULL);
}

static TVal bi_bool(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)rt; (void)argc; (void)line; (void)col;
    return thrum_bool(thrum_truthy(argv[0]));
}

static TVal bi_isnil(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)rt; (void)argc; (void)line; (void)col;
    return thrum_bool(argv[0].kind == TV_NIL);
}

static TVal bi_range(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    int64_t a0 = thrum_need_int(rt, argv[0], line, col);
    int64_t start = 0, stop = a0, step = 1;
    if (argc >= 2) {
        start = a0;
        stop = thrum_need_int(rt, argv[1], line, col);
    }
    if (argc >= 3) step = thrum_need_int(rt, argv[2], line, col);
    if (step == 0) thrum_error(rt, line, col, "range step cannot be 0", NULL);
    T_FRAME(rt);
    T_VAR(rt, out, thrum_list(rt, 8));
    TList* l = (TList*)out.as.obj;
    if (step > 0) {
        for (int64_t x = start; x < stop; x += step) thrum_list_push(rt, l, thrum_num_dimless((double)x));
    } else {
        for (int64_t x = start; x > stop; x += step) thrum_list_push(rt, l, thrum_num_dimless((double)x));
    }
    T_END_FRAME(rt);
    return out;
}

static TVal bi_list(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)line; (void)col;
    T_FRAME(rt);
    T_VAR(rt, a0, argv[0]);
    if (a0.kind == TV_OBJ && a0.as.obj->kind == OBJ_LIST) {
        TList* src = (TList*)a0.as.obj;
        T_VAR(rt, out, thrum_list(rt, src->len));
        TList* nl = (TList*)out.as.obj;
        for (size_t i = 0; i < src->len; i++) thrum_list_push(rt, nl, src->items[i]);
        T_END_FRAME(rt);
        return out;
    }
    if (a0.kind == TV_OBJ && a0.as.obj->kind == OBJ_STR) {
        TStr* s = (TStr*)a0.as.obj;
        T_VAR(rt, out, thrum_list(rt, s->ulen));
        TList* nl = (TList*)out.as.obj;
        for (size_t off = 0; off < s->len; ) {
            size_t cp = thrum_utf8_cp_len((unsigned char)s->data[off]);
            if (off + cp > s->len) cp = s->len - off;
            thrum_list_push(rt, nl, thrum_str(rt, s->data + off, cp));
            off += cp;
        }
        T_END_FRAME(rt);
        return out;
    }
    if (a0.kind == TV_OBJ && a0.as.obj->kind == OBJ_MAP) {
        TMap* m = (TMap*)a0.as.obj;
        T_VAR(rt, out, thrum_list(rt, m->count));
        TList* nl = (TList*)out.as.obj;
        for (size_t i = 0; i < m->order_len; i++) {
            TVal k = m->order_keys[i], v;
            if (!thrum_map_get(m, k, &v)) continue;
            T_VAR(rt, pair, thrum_list(rt, 2));
            TList* pl = (TList*)pair.as.obj;
            thrum_list_push(rt, pl, k);
            thrum_list_push(rt, pl, v);
            thrum_list_push(rt, nl, pair);
        }
        T_END_FRAME(rt);
        return out;
    }
    T_VAR(rt, out, thrum_list(rt, 1));
    thrum_list_push(rt, (TList*)out.as.obj, a0);
    T_END_FRAME(rt);
    return out;
}

static TVal bi_keys(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, mv, argv[0]);
    TMap* m = need_map(rt, mv, "keys", line, col);
    T_VAR(rt, out, thrum_list(rt, m->count));
    TList* l = (TList*)out.as.obj;
    for (size_t i = 0; i < m->order_len; i++) {
        TVal k = m->order_keys[i];
        if (thrum_map_has(m, k)) thrum_list_push(rt, l, k);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal bi_vals(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, mv, argv[0]);
    TMap* m = need_map(rt, mv, "vals", line, col);
    T_VAR(rt, out, thrum_list(rt, m->count));
    TList* l = (TList*)out.as.obj;
    for (size_t i = 0; i < m->order_len; i++) {
        TVal k = m->order_keys[i], v;
        if (thrum_map_get(m, k, &v)) thrum_list_push(rt, l, v);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal bi_items(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, mv, argv[0]);
    TMap* m = need_map(rt, mv, "items", line, col);
    T_VAR(rt, out, thrum_list(rt, m->count));
    TList* l = (TList*)out.as.obj;
    for (size_t i = 0; i < m->order_len; i++) {
        TVal k = m->order_keys[i], v;
        if (!thrum_map_get(m, k, &v)) continue;
        T_VAR(rt, pair, thrum_list(rt, 2));
        TList* pl = (TList*)pair.as.obj;
        thrum_list_push(rt, pl, k);
        thrum_list_push(rt, pl, v);
        thrum_list_push(rt, l, pair);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal bi_push(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    T_VAR(rt, xv, argv[1]);
    TList* l = need_list(rt, lv, "push", line, col);
    T_VAR(rt, out, thrum_list(rt, l->len + 1));
    TList* nl = (TList*)out.as.obj;
    for (size_t i = 0; i < l->len; i++) thrum_list_push(rt, nl, l->items[i]);
    thrum_list_push(rt, nl, xv);
    T_END_FRAME(rt);
    return out;
}

static TVal bi_pop(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    TList* l = need_list(rt, lv, "pop", line, col);
    if (l->len == 0) thrum_error(rt, line, col, "pop from empty list", NULL);
    T_VAR(rt, out, thrum_list(rt, l->len - 1));
    TList* nl = (TList*)out.as.obj;
    for (size_t i = 0; i + 1 < l->len; i++) thrum_list_push(rt, nl, l->items[i]);
    T_END_FRAME(rt);
    return out;
}

static TVal bi_concat(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    if (argv[0].kind == TV_OBJ && argv[1].kind == TV_OBJ && argv[0].as.obj->kind == argv[1].as.obj->kind) {
        if (argv[0].as.obj->kind == OBJ_LIST) {
            T_FRAME(rt);
            T_VAR(rt, av, argv[0]);
            T_VAR(rt, bv, argv[1]);
            TList* a = (TList*)av.as.obj;
            TList* b = (TList*)bv.as.obj;
            T_VAR(rt, out, thrum_list(rt, a->len + b->len));
            TList* nl = (TList*)out.as.obj;
            for (size_t i = 0; i < a->len; i++) thrum_list_push(rt, nl, a->items[i]);
            for (size_t i = 0; i < b->len; i++) thrum_list_push(rt, nl, b->items[i]);
            T_END_FRAME(rt);
            return out;
        }
        if (argv[0].as.obj->kind == OBJ_STR) {
            TStr* a = (TStr*)argv[0].as.obj;
            TStr* b = (TStr*)argv[1].as.obj;
            char* buf = (char*)malloc(a->len + b->len + 1);
            memcpy(buf, a->data, a->len);
            memcpy(buf + a->len, b->data, b->len);
            TVal out = thrum_str(rt, buf, a->len + b->len);
            free(buf);
            return out;
        }
    }
    thrum_error(rt, line, col, "concat needs two lists or two texts", NULL);
}

static TVal bi_slice(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    if (argv[0].kind != TV_OBJ || (argv[0].as.obj->kind != OBJ_LIST && argv[0].as.obj->kind != OBJ_STR)) {
        thrum_error(rt, line, col, "slice needs list or text", NULL);
    }
    TVal stop = (argc > 2) ? argv[2] : thrum_nil();
    return thrum_get_slice(rt, argv[0], true, argv[1], argc > 2, stop, false, thrum_nil(), line, col);
}

static TVal bi_reverse(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    if (argv[0].kind == TV_OBJ && argv[0].as.obj->kind == OBJ_LIST) {
        T_FRAME(rt);
        T_VAR(rt, lv, argv[0]);
        TList* l = (TList*)lv.as.obj;
        T_VAR(rt, out, thrum_list(rt, l->len));
        TList* nl = (TList*)out.as.obj;
        for (size_t i = l->len; i > 0; i--) thrum_list_push(rt, nl, l->items[i - 1]);
        T_END_FRAME(rt);
        return out;
    }
    if (argv[0].kind == TV_OBJ && argv[0].as.obj->kind == OBJ_STR) {
        TStr* s = (TStr*)argv[0].as.obj;
        char* buf = (char*)malloc(s->len + 1);
        size_t out_pos = s->len;
        for (size_t off = 0; off < s->len; ) {
            size_t cp = thrum_utf8_cp_len((unsigned char)s->data[off]);
            if (off + cp > s->len) cp = s->len - off;
            out_pos -= cp;
            memcpy(buf + out_pos, s->data + off, cp);
            off += cp;
        }
        buf[s->len] = '\0';
        TVal out = thrum_str(rt, buf, s->len);
        free(buf);
        return out;
    }
    thrum_error(rt, line, col, "reverse needs a list or text", NULL);
}

typedef struct {
    TVal key;
    size_t idx;
    TVal item;
    bool is_num;
} SortEntry;

static int cmp_sort_entry(const void* a, const void* b) {
    const SortEntry* ea = (const SortEntry*)a;
    const SortEntry* eb = (const SortEntry*)b;
    if (ea->is_num) {
        if (ea->key.as.q.v < eb->key.as.q.v) return -1;
        if (ea->key.as.q.v > eb->key.as.q.v) return 1;
    } else {
        TStr* sa = (TStr*)ea->key.as.obj;
        TStr* sb = (TStr*)eb->key.as.obj;
        size_t ml = sa->len < sb->len ? sa->len : sb->len;
        int c = memcmp(sa->data, sb->data, ml);
        if (c != 0) return c;
        if (sa->len < sb->len) return -1;
        if (sa->len > sb->len) return 1;
    }
    return (ea->idx < eb->idx) ? -1 : (ea->idx > eb->idx ? 1 : 0);
}

static TVal bi_sort(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    T_VAR(rt, fnv, argc > 1 ? argv[1] : thrum_nil());
    TList* xs = need_list(rt, lv, "sort", line, col);
    if (xs->len == 0 || (xs->len == 1 && argc == 1)) {
        T_VAR(rt, out, thrum_list(rt, xs->len));
        TList* nl = (TList*)out.as.obj;
        for (size_t i = 0; i < xs->len; i++) thrum_list_push(rt, nl, xs->items[i]);
        T_END_FRAME(rt);
        return out;
    }
    size_t n = xs->len;
    SortEntry* arr = (SortEntry*)calloc(n, sizeof(SortEntry));
    for (size_t i = 0; i < n; i++) {
        thrum_push_root(rt, &arr[i].key);
        thrum_push_root(rt, &arr[i].item);
        arr[i].item = xs->items[i];
        arr[i].idx = i;
    }
    for (size_t i = 0; i < n; i++) {
        if (argc > 1) {
            TVal arg = arr[i].item;
            arr[i].key = thrum_call(rt, fnv, 1, &arg, line, col);
        } else {
            arr[i].key = arr[i].item;
        }
    }
    TVal k0 = arr[0].key;
    if (k0.kind == TV_NUM) {
        TDim d = k0.as.q.d;
        for (size_t i = 0; i < n; i++) {
            if (arr[i].key.kind != TV_NUM || !thrum_dim_eq(arr[i].key.as.q.d, d)) {
                free(arr);
                thrum_error(rt, line, col, argc > 1 ? "cannot sort mixed key types or dimensions" : "cannot sort mixed dimensions", NULL);
            }
            arr[i].is_num = true;
        }
    } else if (k0.kind == TV_OBJ && k0.as.obj->kind == OBJ_STR) {
        for (size_t i = 0; i < n; i++) {
            if (arr[i].key.kind != TV_OBJ || arr[i].key.as.obj->kind != OBJ_STR) {
                free(arr);
                thrum_error(rt, line, col, argc > 1 ? "cannot sort mixed key types" : "cannot sort mixed types", NULL);
            }
            arr[i].is_num = false;
        }
    } else {
        free(arr);
        char msg[96];
        snprintf(msg, sizeof(msg), argc > 1 ? "cannot sort by %s" : "cannot sort mixed types", thrum_tname(k0));
        thrum_error(rt, line, col, msg, NULL);
    }
    qsort(arr, n, sizeof(SortEntry), cmp_sort_entry);
    T_VAR(rt, out, thrum_list(rt, n));
    TList* nl = (TList*)out.as.obj;
    for (size_t i = 0; i < n; i++) thrum_list_push(rt, nl, arr[i].item);
    free(arr);
    T_END_FRAME(rt);
    return out;
}

static TVal bi_uniq(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    TList* l = need_list(rt, lv, "uniq", line, col);
    T_VAR(rt, out, thrum_list(rt, l->len));
    TList* nl = (TList*)out.as.obj;
    for (size_t i = 0; i < l->len; i++) {
        bool dup = false;
        for (size_t j = 0; j < nl->len; j++) {
            if (thrum_veq(l->items[i], nl->items[j])) { dup = true; break; }
        }
        if (!dup) thrum_list_push(rt, nl, l->items[i]);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal bi_flatten(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    TList* l = need_list(rt, lv, "flatten", line, col);
    T_VAR(rt, out, thrum_list(rt, l->len));
    TList* nl = (TList*)out.as.obj;
    for (size_t i = 0; i < l->len; i++) {
        if (l->items[i].kind == TV_OBJ && l->items[i].as.obj->kind == OBJ_LIST) {
            TList* sub = (TList*)l->items[i].as.obj;
            for (size_t j = 0; j < sub->len; j++) thrum_list_push(rt, nl, sub->items[j]);
        } else {
            thrum_list_push(rt, nl, l->items[i]);
        }
    }
    T_END_FRAME(rt);
    return out;
}

static TVal bi_zip(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, av, argv[0]);
    T_VAR(rt, bv, argv[1]);
    TList* a = need_list(rt, av, "zip", line, col);
    TList* b = need_list(rt, bv, "zip", line, col);
    size_t n = a->len < b->len ? a->len : b->len;
    T_VAR(rt, out, thrum_list(rt, n));
    TList* nl = (TList*)out.as.obj;
    for (size_t i = 0; i < n; i++) {
        T_VAR(rt, pair, thrum_list(rt, 2));
        TList* pl = (TList*)pair.as.obj;
        thrum_list_push(rt, pl, a->items[i]);
        thrum_list_push(rt, pl, b->items[i]);
        thrum_list_push(rt, nl, pair);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal bi_enumerate(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, srcv, argv[0]);
    if (srcv.kind == TV_OBJ && srcv.as.obj->kind == OBJ_LIST) {
        TList* l = (TList*)srcv.as.obj;
        T_VAR(rt, out, thrum_list(rt, l->len));
        TList* nl = (TList*)out.as.obj;
        for (size_t i = 0; i < l->len; i++) {
            T_VAR(rt, pair, thrum_list(rt, 2));
            TList* pl = (TList*)pair.as.obj;
            thrum_list_push(rt, pl, thrum_num_dimless((double)i));
            thrum_list_push(rt, pl, l->items[i]);
            thrum_list_push(rt, nl, pair);
        }
        T_END_FRAME(rt);
        return out;
    }
    if (srcv.kind == TV_OBJ && srcv.as.obj->kind == OBJ_STR) {
        TStr* s = (TStr*)srcv.as.obj;
        T_VAR(rt, out, thrum_list(rt, s->ulen));
        TList* nl = (TList*)out.as.obj;
        size_t ci = 0;
        for (size_t off = 0; off < s->len; ) {
            size_t cp = thrum_utf8_cp_len((unsigned char)s->data[off]);
            if (off + cp > s->len) cp = s->len - off;
            T_VAR(rt, pair, thrum_list(rt, 2));
            TList* pl = (TList*)pair.as.obj;
            thrum_list_push(rt, pl, thrum_num_dimless((double)ci));
            thrum_list_push(rt, pl, thrum_str(rt, s->data + off, cp));
            thrum_list_push(rt, nl, pair);
            off += cp;
            ci++;
        }
        T_END_FRAME(rt);
        return out;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "enumerate needs a list or text, got %s", thrum_tname(srcv));
    thrum_error(rt, line, col, msg, NULL);
}

static TVal bi_map(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    T_VAR(rt, fv, argv[1]);
    TList* l = need_list(rt, lv, "map", line, col);
    T_VAR(rt, out, thrum_list(rt, l->len));
    TList* nl = (TList*)out.as.obj;
    for (size_t i = 0; i < l->len; i++) {
        TVal arg = l->items[i];
        TVal r = thrum_call(rt, fv, 1, &arg, line, col);
        thrum_list_push(rt, nl, r);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal bi_filter(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    T_VAR(rt, fv, argv[1]);
    TList* l = need_list(rt, lv, "filter", line, col);
    T_VAR(rt, out, thrum_list(rt, l->len));
    TList* nl = (TList*)out.as.obj;
    for (size_t i = 0; i < l->len; i++) {
        TVal arg = l->items[i];
        TVal r = thrum_call(rt, fv, 1, &arg, line, col);
        if (thrum_truthy(r)) thrum_list_push(rt, nl, l->items[i]);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal bi_find(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    T_VAR(rt, fv, argv[1]);
    TList* l = need_list(rt, lv, "find", line, col);
    for (size_t i = 0; i < l->len; i++) {
        TVal arg = l->items[i];
        if (thrum_truthy(thrum_call(rt, fv, 1, &arg, line, col))) {
            TVal out = l->items[i];
            T_END_FRAME(rt);
            return out;
        }
    }
    T_END_FRAME(rt);
    return thrum_nil();
}

static TVal bi_count(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    T_VAR(rt, fv, argv[1]);
    TList* l = need_list(rt, lv, "count", line, col);
    size_t c = 0;
    for (size_t i = 0; i < l->len; i++) {
        TVal arg = l->items[i];
        if (thrum_truthy(thrum_call(rt, fv, 1, &arg, line, col))) c++;
    }
    T_END_FRAME(rt);
    return thrum_num_dimless((double)c);
}

static TVal bi_reduce(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    T_VAR(rt, fv, argv[1]);
    T_VAR(rt, acc, argv[2]);
    TList* l = need_list(rt, lv, "reduce", line, col);
    for (size_t i = 0; i < l->len; i++) {
        TVal args[2] = {acc, l->items[i]};
        acc = thrum_call(rt, fv, 2, args, line, col);
    }
    T_END_FRAME(rt);
    return acc;
}

static TVal bi_any(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    T_VAR(rt, fv, argc > 1 ? argv[1] : thrum_nil());
    TList* l = need_list(rt, lv, "any", line, col);
    for (size_t i = 0; i < l->len; i++) {
        TVal r = (argc == 1) ? l->items[i] : thrum_call(rt, fv, 1, &l->items[i], line, col);
        if (thrum_truthy(r)) { T_END_FRAME(rt); return thrum_bool(true); }
    }
    T_END_FRAME(rt);
    return thrum_bool(false);
}

static TVal bi_all(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    T_VAR(rt, fv, argc > 1 ? argv[1] : thrum_nil());
    TList* l = need_list(rt, lv, "all", line, col);
    for (size_t i = 0; i < l->len; i++) {
        TVal r = (argc == 1) ? l->items[i] : thrum_call(rt, fv, 1, &l->items[i], line, col);
        if (!thrum_truthy(r)) { T_END_FRAME(rt); return thrum_bool(false); }
    }
    T_END_FRAME(rt);
    return thrum_bool(true);
}

static TVal bi_sum(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TList* l = need_list(rt, argv[0], "sum", line, col);
    if (l->len == 0) return thrum_num_dimless(0.0);
    TVal acc = thrum_obj((TObj*)l) ; /* unused temp replaced cleanly below */
    acc = thrum_qty(rt, thrum_need_q(rt, l->items[0], line, col).v,
                    thrum_need_q(rt, l->items[0], line, col).u,
                    thrum_need_q(rt, l->items[0], line, col).d,
                    thrum_need_q(rt, l->items[0], line, col).disp_sc,
                    thrum_need_q(rt, l->items[0], line, col).disp_sym);
    for (size_t i = 1; i < l->len; i++) {
        thrum_need_q(rt, l->items[i], line, col);
        acc = thrum_add(rt, acc, l->items[i], line, col);
    }
    return acc;
}

static TVal bi_product(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TList* l = need_list(rt, argv[0], "product", line, col);
    if (l->len == 0) return thrum_num_dimless(1.0);
    thrum_need_q(rt, l->items[0], line, col);
    TVal acc = l->items[0];
    for (size_t i = 1; i < l->len; i++) {
        thrum_need_q(rt, l->items[i], line, col);
        acc = thrum_mul(rt, acc, l->items[i], line, col);
    }
    return acc;
}

static TVal list_extreme(TRuntime* rt, TList* xs, bool want_min, int line, int col) {
    if (xs->len == 0) thrum_error(rt, line, col, "empty list", NULL);
    TVal best = xs->items[0];
    if (best.kind == TV_NUM) {
        for (size_t i = 1; i < xs->len; i++) {
            if (thrum_cmp(rt, xs->items[i], best, want_min ? 0 : 1, line, col)) best = xs->items[i];
        }
        return best;
    }
    if (best.kind == TV_OBJ && best.as.obj->kind == OBJ_STR) {
        for (size_t i = 1; i < xs->len; i++) {
            if (xs->items[i].kind != TV_OBJ || xs->items[i].as.obj->kind != OBJ_STR) {
                char msg[96];
                snprintf(msg, sizeof(msg), "cannot compare %s with text", thrum_tname(xs->items[i]));
                thrum_error(rt, line, col, msg, NULL);
            }
            TStr* sx = (TStr*)xs->items[i].as.obj;
            TStr* sb = (TStr*)best.as.obj;
            int cmp = strcmp(sx->data, sb->data);
            if ((want_min && cmp < 0) || (!want_min && cmp > 0)) best = xs->items[i];
        }
        return best;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "min/max need numbers or text, got %s", thrum_tname(best));
    thrum_error(rt, line, col, msg, NULL);
}

static TVal bi_min(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    return list_extreme(rt, need_list(rt, argv[0], "min", line, col), true, line, col);
}

static TVal bi_max(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    return list_extreme(rt, need_list(rt, argv[0], "max", line, col), false, line, col);
}

static TVal bi_mean(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TList* l = need_list(rt, argv[0], "mean", line, col);
    if (l->len == 0) thrum_error(rt, line, col, "mean of empty list", NULL);
    TVal s = bi_sum(rt, 1, argv, line, col);
    return thrum_div(rt, s, thrum_num_dimless((double)l->len), line, col);
}

static TVal bi_median(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, sorted, bi_sort(rt, 1, argv, line, col));
    TList* xs = (TList*)sorted.as.obj;
    if (xs->len == 0) thrum_error(rt, line, col, "median of empty list", NULL);
    size_t n = xs->len;
    TVal out;
    if (n % 2 == 1) {
        out = xs->items[n / 2];
    } else {
        TVal sum2 = thrum_add(rt, xs->items[n / 2 - 1], xs->items[n / 2], line, col);
        out = thrum_div(rt, sum2, thrum_num_dimless(2.0), line, col);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal bi_stdev(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TList* xs = need_list(rt, argv[0], "stdev", line, col);
    if (xs->len < 2) thrum_error(rt, line, col, "stdev needs list of >= 2 values", NULL);
    TVal m = bi_mean(rt, 1, argv, line, col);
    TVal acc = thrum_num_dimless(0.0);
    for (size_t i = 0; i < xs->len; i++) {
        TVal diff = thrum_sub(rt, xs->items[i], m, line, col);
        TVal sq = thrum_pow(rt, diff, thrum_num_dimless(2.0), line, col);
        acc = (i == 0) ? sq : thrum_add(rt, acc, sq, line, col);
    }
    TVal var = thrum_div(rt, acc, thrum_num_dimless((double)(xs->len - 1)), line, col);
    return thrum_pow(rt, var, thrum_num_dimless(0.5), line, col);
}

static TVal bi_abs(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    return thrum_qty(rt, fabs(x.v), x.u, x.d, x.disp_sc, x.disp_sym);
}

static TVal bi_sqrt(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    thrum_need_q(rt, argv[0], line, col);
    return thrum_pow(rt, argv[0], thrum_num_dimless(0.5), line, col);
}

static TVal bi_sign(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    return thrum_num_dimless((x.v > 0.0) ? 1.0 : (x.v < 0.0 ? -1.0 : 0.0));
}

static TVal bi_floor(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    double sc = x.disp_sc != 0.0 ? x.disp_sc : 1.0;
    return thrum_qty(rt, floor(x.v / sc) * sc, x.u, x.d, x.disp_sc, x.disp_sym);
}

static TVal bi_ceil(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    double sc = x.disp_sc != 0.0 ? x.disp_sc : 1.0;
    return thrum_qty(rt, ceil(x.v / sc) * sc, x.u, x.d, x.disp_sc, x.disp_sym);
}

static double py_round(double val, int64_t nd) {
    if (!isfinite(val)) return val;
    double factor = pow(10.0, (double)nd);
    double scaled = val * factor;
    if (!isfinite(scaled)) return val;
    double r = nearbyint(scaled); /* round-half-to-even in standard C round mode */
    return r / factor;
}

static TVal bi_round(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    TQ x = thrum_need_q(rt, argv[0], line, col);
    int64_t nd = argc > 1 ? thrum_need_int(rt, argv[1], line, col) : 0;
    double sc = x.disp_sc != 0.0 ? x.disp_sc : 1.0;
    return thrum_qty(rt, py_round(x.v / sc, nd) * sc, x.u, x.d, x.disp_sc, x.disp_sym);
}

static TVal bi_clamp(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TVal hi_pair[2] = {argv[0], argv[2]};
    T_FRAME(rt);
    T_VAR(rt, l1, thrum_list(rt, 2));
    thrum_list_push(rt, (TList*)l1.as.obj, hi_pair[0]);
    thrum_list_push(rt, (TList*)l1.as.obj, hi_pair[1]);
    T_VAR(rt, capped, list_extreme(rt, (TList*)l1.as.obj, true, line, col));
    T_VAR(rt, l2, thrum_list(rt, 2));
    thrum_list_push(rt, (TList*)l2.as.obj, argv[1]);
    thrum_list_push(rt, (TList*)l2.as.obj, capped);
    TVal out = list_extreme(rt, (TList*)l2.as.obj, false, line, col);
    T_END_FRAME(rt);
    return out;
}

static TVal bi_lerp(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TVal diff = thrum_sub(rt, argv[1], argv[0], line, col);
    TVal prod = thrum_mul(rt, diff, argv[2], line, col);
    return thrum_add(rt, argv[0], prod, line, col);
}

static TVal bi_approx(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    thrum_need_q(rt, argv[0], line, col);
    thrum_need_q(rt, argv[1], line, col);
    double k = argc > 2 ? thrum_need_q(rt, argv[2], line, col).v : 3.0;
    return thrum_bool(thrum_approx(argv[0], argv[1], k));
}

static TVal bi_unit(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    if (x.disp_sym && x.disp_sym[0]) return thrum_cstr(rt, x.disp_sym);
    char dbuf[64];
    thrum_dimstr(x.d, dbuf, sizeof(dbuf));
    return thrum_cstr(rt, dbuf[0] ? dbuf : "1");
}

static TVal bi_dim(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    char dbuf[64];
    thrum_dimstr(x.d, dbuf, sizeof(dbuf));
    return thrum_cstr(rt, dbuf[0] ? dbuf : "1");
}

static TVal bi_unc(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    return thrum_qty(rt, x.u, 0.0, x.d, x.disp_sc, x.disp_sym);
}

static TVal bi_bare(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    return thrum_num_dimless(x.v);
}

static TVal bi_si(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    return thrum_qty(rt, x.v, x.u, x.d, 1.0, NULL);
}

static TVal bi_exact(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    return thrum_qty(rt, x.v, 0.0, x.d, x.disp_sc, x.disp_sym);
}

static TVal bi_with_unc(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    TQ u = thrum_need_q(rt, argv[1], line, col);
    if (!thrum_dimless(u.d) && !thrum_dim_eq(u.d, x.d)) {
        char d1[64], d2[64], msg[192];
        thrum_dimstr(u.d, d1, sizeof(d1));
        thrum_dimstr(x.d, d2, sizeof(d2));
        snprintf(msg, sizeof(msg), "uncertainty dimension %s does not match %s",
                 d1[0] ? d1 : "dimensionless", d2[0] ? d2 : "dimensionless");
        thrum_error(rt, line, col, msg, NULL);
    }
    double uv = thrum_dim_eq(u.d, x.d) ? u.v : u.v * (x.disp_sc != 0.0 ? x.disp_sc : 1.0);
    return thrum_qty(rt, x.v, fabs(uv), x.d, x.disp_sc, x.disp_sym);
}

static TVal bi_qty(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    if (argv[1].kind != TV_OBJ || argv[1].as.obj->kind != OBJ_STR) {
        thrum_error(rt, line, col, "qty unit must be text", NULL);
    }
    const char* us = ((TStr*)argv[1].as.obj)->data;
    double sc = 1.0;
    TDim d = thrum_d0();
    thrum_parse_unit(rt, us, line, col, &sc, &d);
    double v = thrum_need_q(rt, argv[0], line, col).v;
    double u = (argc > 2) ? thrum_need_q(rt, argv[2], line, col).v : 0.0;
    return thrum_qty(rt, v * sc, u * sc, d, sc, us);
}

static TVal bi_convert(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[1], "convert", line, col);
    return thrum_convert(rt, argv[0], s->data, line, col);
}

static TVal bi_c2k(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    return thrum_qty(rt, x.v + 273.15, x.u, thrum_dim(0, 0, 0, 0, 1, 0, 0), 1.0, "K");
}

static TVal bi_k2c(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    return thrum_qty(rt, x.v - 273.15, x.u, thrum_dim(0, 0, 0, 0, 1, 0, 0), 1.0, "K");
}

static TVal bi_split(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    T_FRAME(rt);
    T_VAR(rt, sv, argv[0]);
    TStr* s = need_str(rt, sv, "split", line, col);
    T_VAR(rt, out, thrum_list(rt, 8));
    TList* l = (TList*)out.as.obj;
    if (argc == 1) {
        size_t i = 0;
        while (i < s->len) {
            while (i < s->len && isspace((unsigned char)s->data[i])) i++;
            if (i >= s->len) break;
            size_t st = i;
            while (i < s->len && !isspace((unsigned char)s->data[i])) i++;
            thrum_list_push(rt, l, thrum_str(rt, s->data + st, i - st));
        }
    } else {
        T_VAR(rt, sepv, argv[1]);
        TStr* sep = need_str(rt, sepv, "split", line, col);
        if (sep->len == 0) thrum_error(rt, line, col, "empty separator", NULL);
        size_t i = 0;
        for (;;) {
            const char* p = (i <= s->len) ? strstr(s->data + i, sep->data) : NULL;
            if (!p) {
                thrum_list_push(rt, l, thrum_str(rt, s->data + i, s->len - i));
                break;
            }
            size_t pos = (size_t)(p - s->data);
            thrum_list_push(rt, l, thrum_str(rt, s->data + i, pos - i));
            i = pos + sep->len;
        }
    }
    T_END_FRAME(rt);
    return out;
}

static TVal bi_join(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* sep = need_str(rt, argv[0], "join", line, col);
    TList* xs = need_list(rt, argv[1], "join", line, col);
    BSB sb = {0};
    for (size_t i = 0; i < xs->len; i++) {
        if (i > 0) bsb_app_n(&sb, sep->data, sep->len);
        char* s = thrum_vstr_alloc(rt, xs->items[i], false);
        bsb_app(&sb, s);
        free(s);
    }
    TVal out = thrum_str(rt, sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return out;
}

static TVal bi_upper(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "upper", line, col);
    char* buf = (char*)malloc(s->len + 1);
    for (size_t i = 0; i < s->len; i++) buf[i] = (char)toupper((unsigned char)s->data[i]);
    TVal out = thrum_str(rt, buf, s->len);
    free(buf);
    return out;
}

static TVal bi_lower(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "lower", line, col);
    char* buf = (char*)malloc(s->len + 1);
    for (size_t i = 0; i < s->len; i++) buf[i] = (char)tolower((unsigned char)s->data[i]);
    TVal out = thrum_str(rt, buf, s->len);
    free(buf);
    return out;
}

static TVal bi_trim(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "trim", line, col);
    size_t i = 0, j = s->len;
    while (i < j && isspace((unsigned char)s->data[i])) i++;
    while (j > i && isspace((unsigned char)s->data[j - 1])) j--;
    return thrum_str(rt, s->data + i, j - i);
}

static TVal bi_replace(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "replace", line, col);
    TStr* old = need_str(rt, argv[1], "replace", line, col);
    TStr* rep = need_str(rt, argv[2], "replace", line, col);
    BSB sb = {0};
    if (old->len == 0) {
        for (size_t i = 0; i <= s->len; i++) {
            bsb_app_n(&sb, rep->data, rep->len);
            if (i < s->len) bsb_ch(&sb, s->data[i]);
        }
    } else {
        size_t i = 0;
        while (i <= s->len) {
            const char* p = strstr(s->data + i, old->data);
            if (!p) {
                bsb_app_n(&sb, s->data + i, s->len - i);
                break;
            }
            size_t pos = (size_t)(p - s->data);
            bsb_app_n(&sb, s->data + i, pos - i);
            bsb_app_n(&sb, rep->data, rep->len);
            i = pos + old->len;
        }
    }
    TVal out = thrum_str(rt, sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return out;
}

static TVal bi_starts(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "starts", line, col);
    TStr* pre = need_str(rt, argv[1], "starts", line, col);
    return thrum_bool(s->len >= pre->len && memcmp(s->data, pre->data, pre->len) == 0);
}

static TVal bi_ends(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "ends", line, col);
    TStr* suf = need_str(rt, argv[1], "ends", line, col);
    return thrum_bool(s->len >= suf->len && memcmp(s->data + s->len - suf->len, suf->data, suf->len) == 0);
}

static TVal bi_fix(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    int64_t nd = thrum_need_int(rt, argv[1], line, col);
    if (nd < 0) thrum_error(rt, line, col, "fix precision must be >= 0", NULL);
    double sc = x.disp_sc != 0.0 ? x.disp_sc : 1.0;
    const char* sym = x.disp_sym;
    char dbuf[64] = "";
    if (!sym) { thrum_dimstr(x.d, dbuf, sizeof(dbuf)); sym = dbuf[0] ? dbuf : NULL; }
    double v = x.v / sc;
    char buf[192];
    if (!isfinite(v)) {
        const char* nf = isnan(v) ? "nan" : (v > 0 ? "inf" : "-inf");
        if (sym && sym[0]) snprintf(buf, sizeof(buf), "%s %s", nf, sym);
        else snprintf(buf, sizeof(buf), "%s", nf);
    } else {
        if (sym && sym[0]) snprintf(buf, sizeof(buf), "%.*f %s", (int)nd, v, sym);
        else snprintf(buf, sizeof(buf), "%.*f", (int)nd, v);
    }
    return thrum_cstr(rt, buf);
}

static TVal bi_fmt(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)line; (void)col;
    return thrum_vstr_val(rt, argv[0], false);
}

static TVal bi_fail(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    char* s = thrum_vstr_alloc(rt, argv[0], false);
    const char* im = thrum_intern(rt, s);
    free(s);
    thrum_error(rt, line, col, im, NULL);
}

void thrum_install_builtins(TRuntime* rt) {
    reg_builtin(rt, "len", bi_len, 1, 1);
    reg_builtin(rt, "type", bi_type, 1, 1);
    reg_builtin(rt, "str", bi_str, 1, 1);
    reg_builtin(rt, "repr", bi_repr, 1, 1);
    reg_builtin(rt, "num", bi_num, 1, 2);
    reg_builtin(rt, "bool", bi_bool, 1, 1);
    reg_builtin(rt, "isnil", bi_isnil, 1, 1);
    reg_builtin(rt, "range", bi_range, 1, 3);
    reg_builtin(rt, "list", bi_list, 1, 1);
    reg_builtin(rt, "keys", bi_keys, 1, 1);
    reg_builtin(rt, "vals", bi_vals, 1, 1);
    reg_builtin(rt, "items", bi_items, 1, 1);
    reg_builtin(rt, "push", bi_push, 2, 2);
    reg_builtin(rt, "pop", bi_pop, 1, 1);
    reg_builtin(rt, "concat", bi_concat, 2, 2);
    reg_builtin(rt, "slice", bi_slice, 2, 3);
    reg_builtin(rt, "reverse", bi_reverse, 1, 1);
    reg_builtin(rt, "sort", bi_sort, 1, 2);
    reg_builtin(rt, "uniq", bi_uniq, 1, 1);
    reg_builtin(rt, "flatten", bi_flatten, 1, 1);
    reg_builtin(rt, "zip", bi_zip, 2, 2);
    reg_builtin(rt, "enumerate", bi_enumerate, 1, 1);
    reg_builtin(rt, "map", bi_map, 2, 2);
    reg_builtin(rt, "filter", bi_filter, 2, 2);
    reg_builtin(rt, "find", bi_find, 2, 2);
    reg_builtin(rt, "count", bi_count, 2, 2);
    reg_builtin(rt, "reduce", bi_reduce, 3, 3);
    reg_builtin(rt, "any", bi_any, 1, 2);
    reg_builtin(rt, "all", bi_all, 1, 2);
    reg_builtin(rt, "sum", bi_sum, 1, 1);
    reg_builtin(rt, "product", bi_product, 1, 1);
    reg_builtin(rt, "min", bi_min, 1, 1);
    reg_builtin(rt, "max", bi_max, 1, 1);
    reg_builtin(rt, "mean", bi_mean, 1, 1);
    reg_builtin(rt, "median", bi_median, 1, 1);
    reg_builtin(rt, "stdev", bi_stdev, 1, 1);
    reg_builtin(rt, "abs", bi_abs, 1, 1);
    reg_builtin(rt, "sqrt", bi_sqrt, 1, 1);
    reg_builtin(rt, "sign", bi_sign, 1, 1);
    reg_builtin(rt, "floor", bi_floor, 1, 1);
    reg_builtin(rt, "ceil", bi_ceil, 1, 1);
    reg_builtin(rt, "round", bi_round, 1, 2);
    reg_builtin(rt, "clamp", bi_clamp, 3, 3);
    reg_builtin(rt, "lerp", bi_lerp, 3, 3);
    reg_builtin(rt, "approx", bi_approx, 2, 3);
    reg_builtin(rt, "unit", bi_unit, 1, 1);
    reg_builtin(rt, "dim", bi_dim, 1, 1);
    reg_builtin(rt, "unc", bi_unc, 1, 1);
    reg_builtin(rt, "bare", bi_bare, 1, 1);
    reg_builtin(rt, "si", bi_si, 1, 1);
    reg_builtin(rt, "exact", bi_exact, 1, 1);
    reg_builtin(rt, "with_unc", bi_with_unc, 2, 2);
    reg_builtin(rt, "qty", bi_qty, 2, 3);
    reg_builtin(rt, "convert", bi_convert, 2, 2);
    reg_builtin(rt, "c2k", bi_c2k, 1, 1);
    reg_builtin(rt, "k2c", bi_k2c, 1, 1);
    reg_builtin(rt, "split", bi_split, 1, 2);
    reg_builtin(rt, "join", bi_join, 2, 2);
    reg_builtin(rt, "upper", bi_upper, 1, 1);
    reg_builtin(rt, "lower", bi_lower, 1, 1);
    reg_builtin(rt, "trim", bi_trim, 1, 1);
    reg_builtin(rt, "replace", bi_replace, 3, 3);
    reg_builtin(rt, "starts", bi_starts, 2, 2);
    reg_builtin(rt, "ends", bi_ends, 2, 2);
    reg_builtin(rt, "fix", bi_fix, 2, 2);
    reg_builtin(rt, "fmt", bi_fmt, 1, 2);
    reg_builtin(rt, "fail", bi_fail, 1, 1);
}

/* ============================== std.math ============================== */
typedef double (*M1Fn)(double);

static TVal math_lift(TRuntime* rt, M1Fn f, TVal a, int line, int col, bool need_pos, double dom_lo, double dom_hi) {
    TQ x = thrum_need_q(rt, a, line, col);
    if (!thrum_dimless(x.d)) thrum_error(rt, line, col, "math functions need dimensionless arguments", NULL);
    if (need_pos && x.v <= 0.0) thrum_error(rt, line, col, "math domain error", NULL);
    if (dom_lo <= dom_hi && (x.v < dom_lo || x.v > dom_hi)) thrum_error(rt, line, col, "math domain error", NULL);
    double val = f(x.v);
    if (isnan(val) && !isnan(x.v)) thrum_error(rt, line, col, "math domain error", NULL);
    double h = 1e-7 * fmax(1.0, fabs(x.v));
    double f1 = f(x.v + h), f0 = f(x.v - h);
    double dv = (!isnan(f1) && !isnan(f0) && isfinite(f1) && isfinite(f0)) ? (f1 - f0) / (2.0 * h) : 0.0;
    return thrum_qty(rt, val, fabs(dv) * x.u, thrum_d0(), 1.0, NULL);
}

static TVal m_sin(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, sin, argv[0], line, col, false, 1.0, -1.0); }
static TVal m_cos(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, cos, argv[0], line, col, false, 1.0, -1.0); }
static TVal m_tan(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, tan, argv[0], line, col, false, 1.0, -1.0); }
static TVal m_asin(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, asin, argv[0], line, col, false, -1.0, 1.0); }
static TVal m_acos(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, acos, argv[0], line, col, false, -1.0, 1.0); }
static TVal m_atan(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, atan, argv[0], line, col, false, 1.0, -1.0); }
static TVal m_exp(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, exp, argv[0], line, col, false, 1.0, -1.0); }
static TVal m_expm1(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, expm1, argv[0], line, col, false, 1.0, -1.0); }
static TVal m_ln(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, log, argv[0], line, col, true, 1.0, -1.0); }
static TVal m_log1p(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, log1p, argv[0], line, col, false, -1.0 + 1e-300, 1e308); }
static TVal m_log10(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, log10, argv[0], line, col, true, 1.0, -1.0); }
static TVal m_log2(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, log2, argv[0], line, col, true, 1.0, -1.0); }
static TVal m_sinh(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, sinh, argv[0], line, col, false, 1.0, -1.0); }
static TVal m_cosh(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, cosh, argv[0], line, col, false, 1.0, -1.0); }
static TVal m_tanh(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, tanh, argv[0], line, col, false, 1.0, -1.0); }
static TVal m_asinh(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, asinh, argv[0], line, col, false, 1.0, -1.0); }
static TVal m_acosh(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, acosh, argv[0], line, col, false, 1.0, 1e308); }
static TVal m_atanh(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return math_lift(rt, atanh, argv[0], line, col, false, -1.0, 1.0); }

static TVal m_deg2rad(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    if (!thrum_dimless(x.d)) thrum_error(rt, line, col, "deg2rad needs dimensionless angle", NULL);
    if (x.disp_sym && (strcmp(x.disp_sym, "deg") == 0 || strcmp(x.disp_sym, "°") == 0 ||
                       strcmp(x.disp_sym, "degree") == 0 || strcmp(x.disp_sym, "degrees") == 0)) {
        return thrum_qty(rt, x.v, x.u, thrum_d0(), 1.0, "rad");
    }
    double k = 3.14159265358979323846 / 180.0;
    return thrum_qty(rt, x.v * k, x.u * k, thrum_d0(), 1.0, "rad");
}

static TVal m_rad2deg(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    if (!thrum_dimless(x.d)) thrum_error(rt, line, col, "rad2deg needs dimensionless angle", NULL);
    double k = 3.14159265358979323846 / 180.0;
    return thrum_qty(rt, x.v, x.u, thrum_d0(), k, "deg");
}

static TVal m_isfinite(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return thrum_bool(isfinite(thrum_need_q(rt, argv[0], line, col).v) != 0); }
static TVal m_isnan(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return thrum_bool(isnan(thrum_need_q(rt, argv[0], line, col).v) != 0); }
static TVal m_isinf(TRuntime* rt, int argc, TVal* argv, int line, int col) { (void)argc; return thrum_bool(isinf(thrum_need_q(rt, argv[0], line, col).v) != 0); }

static TVal m_atan2(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ y = thrum_need_q(rt, argv[0], line, col);
    TQ x = thrum_need_q(rt, argv[1], line, col);
    if (!thrum_dim_eq(y.d, x.d)) thrum_error(rt, line, col, "atan2 arguments must have identical dimensions", NULL);
    double r2 = x.v * x.v + y.v * y.v;
    double u = r2 != 0.0 ? hypot(x.v * y.u, y.v * x.u) / r2 : 0.0;
    return thrum_qty(rt, atan2(y.v, x.v), u, thrum_d0(), 1.0, NULL);
}

static TVal m_log(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    TQ b = thrum_need_q(rt, argv[1], line, col);
    if (!thrum_dimless(x.d) || !thrum_dimless(b.d)) thrum_error(rt, line, col, "log needs dimensionless arguments", NULL);
    if (x.v <= 0.0 || b.v <= 0.0 || b.v == 1.0) thrum_error(rt, line, col, "math domain error", NULL);
    double u = fabs(x.u / (x.v * log(b.v)));
    return thrum_qty(rt, log(x.v) / log(b.v), u, thrum_d0(), 1.0, NULL);
}

static TVal m_hypot(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TVal a2 = thrum_pow(rt, argv[0], thrum_num_dimless(2.0), line, col);
    TVal b2 = thrum_pow(rt, argv[1], thrum_num_dimless(2.0), line, col);
    TVal sum = thrum_add(rt, a2, b2, line, col);
    return thrum_pow(rt, sum, thrum_num_dimless(0.5), line, col);
}

static int64_t igcd(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a;
}

static TVal m_gcd(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    return thrum_num_dimless((double)igcd(thrum_need_int(rt, argv[0], line, col), thrum_need_int(rt, argv[1], line, col)));
}

static TVal m_lcm(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    int64_t a = thrum_need_int(rt, argv[0], line, col);
    int64_t b = thrum_need_int(rt, argv[1], line, col);
    if (!a || !b) return thrum_num_dimless(0.0);
    int64_t g = igcd(a, b);
    double r = fabs((double)(a / g) * (double)b);
    return thrum_num_dimless(r);
}

static TVal m_factorial(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    int64_t n = thrum_need_int(rt, argv[0], line, col);
    if (n < 0) thrum_error(rt, line, col, "factorial needs n >= 0", NULL);
    double r = 1.0;
    for (int64_t i = 2; i <= n; i++) r *= (double)i;
    return thrum_num_dimless(r);
}

static TVal m_comb(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    int64_t n = thrum_need_int(rt, argv[0], line, col);
    int64_t k = thrum_need_int(rt, argv[1], line, col);
    if (n < 0 || k < 0) thrum_error(rt, line, col, "comb needs non-negative integers", NULL);
    if (k > n) return thrum_num_dimless(0.0);
    if (k > n - k) k = n - k;
    double r = 1.0;
    for (int64_t i = 1; i <= k; i++) r = (r * (double)(n - k + i)) / (double)i;
    return thrum_num_dimless(floor(r + 0.5));
}

static TVal m_perm(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    int64_t n = thrum_need_int(rt, argv[0], line, col);
    int64_t k = thrum_need_int(rt, argv[1], line, col);
    if (n < 0 || k < 0) thrum_error(rt, line, col, "perm needs non-negative integers", NULL);
    if (k > n) return thrum_num_dimless(0.0);
    double r = 1.0;
    for (int64_t i = 0; i < k; i++) r *= (double)(n - i);
    return thrum_num_dimless(r);
}

static TVal m_is_prime(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    int64_t n = thrum_need_int(rt, argv[0], line, col);
    if (n < 2) return thrum_bool(false);
    if (n % 2 == 0) return thrum_bool(n == 2);
    for (int64_t f = 3; f <= n / f; f += 2) if (n % f == 0) return thrum_bool(false);
    return thrum_bool(true);
}

static TVal m_factors(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    int64_t n = thrum_need_int(rt, argv[0], line, col);
    if (n < 1) thrum_error(rt, line, col, "factors needs n >= 1", NULL);
    T_FRAME(rt);
    T_VAR(rt, out, thrum_list(rt, 8));
    TList* l = (TList*)out.as.obj;
    for (int64_t d = 2; d <= n / d; d++) {
        while (n % d == 0) {
            thrum_list_push(rt, l, thrum_num_dimless((double)d));
            n /= d;
        }
    }
    if (n > 1) thrum_list_push(rt, l, thrum_num_dimless((double)n));
    T_END_FRAME(rt);
    return out;
}

static TVal m_linspace(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ start = thrum_need_q(rt, argv[0], line, col);
    TQ stop = thrum_need_q(rt, argv[1], line, col);
    int64_t n = thrum_need_int(rt, argv[2], line, col);
    if (!thrum_dim_eq(start.d, stop.d)) thrum_error(rt, line, col, "linspace endpoints must have identical dimensions", NULL);
    if (n < 0) thrum_error(rt, line, col, "linspace count must be >= 0", NULL);
    T_FRAME(rt);
    T_VAR(rt, out, thrum_list(rt, (size_t)n));
    TList* l = (TList*)out.as.obj;
    if (n == 1) thrum_list_push(rt, l, argv[0]);
    else if (n > 1) {
        TVal span = thrum_sub(rt, argv[1], argv[0], line, col);
        TVal step = thrum_div(rt, span, thrum_num_dimless((double)(n - 1)), line, col);
        for (int64_t i = 0; i < n; i++) {
            TVal v = thrum_add(rt, argv[0], thrum_mul(rt, step, thrum_num_dimless((double)i), line, col), line, col);
            thrum_list_push(rt, l, v);
        }
    }
    T_END_FRAME(rt);
    return out;
}

static TVal m_arange(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    TQ start = thrum_need_q(rt, argv[0], line, col);
    TQ stop = thrum_need_q(rt, argv[1], line, col);
    TVal stepv = (argc > 2) ? argv[2] : thrum_qty(rt, 1.0 * (start.disp_sc != 0.0 ? start.disp_sc : 1.0), 0.0, start.d, start.disp_sc, start.disp_sym);
    if (argc <= 2 && start.disp_sc == 1.0 && !start.disp_sym) stepv = thrum_qty(rt, 1.0, 0.0, start.d, 1.0, NULL);
    else if (argc <= 2) stepv = thrum_qty(rt, 1.0, 0.0, start.d, start.disp_sc, start.disp_sym);
    TQ step = thrum_need_q(rt, stepv, line, col);
    if (!thrum_dim_eq(start.d, stop.d) || !thrum_dim_eq(start.d, step.d)) {
        thrum_error(rt, line, col, "arange arguments must have identical dimensions", NULL);
    }
    if (step.v == 0.0) thrum_error(rt, line, col, "arange step cannot be 0", NULL);
    T_FRAME(rt);
    T_VAR(rt, out, thrum_list(rt, 16));
    TList* l = (TList*)out.as.obj;
    TVal cur = argv[0];
    double eps = 1e-12 * fmax(1.0, fabs(stop.v));
    while (step.v > 0.0 ? (cur.as.q.v < stop.v - eps) : (cur.as.q.v > stop.v + eps)) {
        thrum_list_push(rt, l, cur);
        cur = thrum_add(rt, cur, stepv, line, col);
        if (l->len > 100000) thrum_error(rt, line, col, "arange too large", NULL);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal m_dot(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TList* xs = need_list(rt, argv[0], "dot", line, col);
    TList* ys = need_list(rt, argv[1], "dot", line, col);
    if (xs->len != ys->len) thrum_error(rt, line, col, "dot needs equal-length lists", NULL);
    if (xs->len == 0) return thrum_num_dimless(0.0);
    TVal acc = thrum_mul(rt, xs->items[0], ys->items[0], line, col);
    for (size_t i = 1; i < xs->len; i++) {
        TVal p = thrum_mul(rt, xs->items[i], ys->items[i], line, col);
        acc = thrum_add(rt, acc, p, line, col);
    }
    return acc;
}

static TVal m_norm(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TList* xs = need_list(rt, argv[0], "norm", line, col);
    if (xs->len == 0) return thrum_num_dimless(0.0);
    TVal acc = thrum_pow(rt, xs->items[0], thrum_num_dimless(2.0), line, col);
    for (size_t i = 1; i < xs->len; i++) {
        TVal sq = thrum_pow(rt, xs->items[i], thrum_num_dimless(2.0), line, col);
        acc = thrum_add(rt, acc, sq, line, col);
    }
    return thrum_pow(rt, acc, thrum_num_dimless(0.5), line, col);
}

static TVal m_cumsum(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, xv, argv[0]);
    TList* xs = need_list(rt, xv, "cumsum", line, col);
    T_VAR(rt, out, thrum_list(rt, xs->len));
    TList* l = (TList*)out.as.obj;
    if (xs->len > 0) {
        thrum_need_q(rt, xs->items[0], line, col);
        TVal acc = xs->items[0];
        thrum_list_push(rt, l, acc);
        for (size_t i = 1; i < xs->len; i++) {
            thrum_need_q(rt, xs->items[i], line, col);
            acc = thrum_add(rt, acc, xs->items[i], line, col);
            thrum_list_push(rt, l, acc);
        }
    }
    T_END_FRAME(rt);
    return out;
}

static TVal m_diff(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, xv, argv[0]);
    TList* xs = need_list(rt, xv, "diff", line, col);
    T_VAR(rt, out, thrum_list(rt, xs->len > 1 ? xs->len - 1 : 0));
    TList* l = (TList*)out.as.obj;
    for (size_t i = 0; i + 1 < xs->len; i++) {
        thrum_list_push(rt, l, thrum_sub(rt, xs->items[i + 1], xs->items[i], line, col));
    }
    T_END_FRAME(rt);
    return out;
}

static TVal m_band(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    return thrum_num_dimless((double)(thrum_need_int(rt, argv[0], line, col) & thrum_need_int(rt, argv[1], line, col)));
}
static TVal m_bor(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    return thrum_num_dimless((double)(thrum_need_int(rt, argv[0], line, col) | thrum_need_int(rt, argv[1], line, col)));
}
static TVal m_bxor(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    return thrum_num_dimless((double)(thrum_need_int(rt, argv[0], line, col) ^ thrum_need_int(rt, argv[1], line, col)));
}
static TVal m_bnot(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    return thrum_num_dimless((double)(~thrum_need_int(rt, argv[0], line, col)));
}
static TVal m_shl(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    int64_t a = thrum_need_int(rt, argv[0], line, col);
    int64_t k = thrum_need_int(rt, argv[1], line, col);
    if (k < 0 || k >= 63) thrum_error(rt, line, col, "shift count out of range 0..62", NULL);
    return thrum_num_dimless((double)(int64_t)((uint64_t)a << k));
}
static TVal m_shr(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    int64_t a = thrum_need_int(rt, argv[0], line, col);
    int64_t k = thrum_need_int(rt, argv[1], line, col);
    if (k < 0 || k >= 63) thrum_error(rt, line, col, "shift count out of range 0..62", NULL);
    int64_t res = (a < 0) ? ~((~a) >> k) : (a >> k);
    return thrum_num_dimless((double)res);
}

static TEnv* build_mod_math(TRuntime* rt) {
    TEnv* sc = thrum_env_new(rt, rt->builtins, false);
    mod_add_val(rt, sc, "pi", thrum_num_dimless(3.14159265358979323846));
    mod_add_val(rt, sc, "tau", thrum_num_dimless(6.28318530717958647692));
    mod_add_val(rt, sc, "e", thrum_num_dimless(2.71828182845904523536));
    mod_add_val(rt, sc, "inf", thrum_num_dimless(INFINITY));
    mod_add_val(rt, sc, "nan", thrum_num_dimless(NAN));
    mod_add_val(rt, sc, "phi", thrum_num_dimless((1.0 + sqrt(5.0)) / 2.0));
    mod_add(rt, sc, "math", "sin", m_sin, 1, 1);
    mod_add(rt, sc, "math", "cos", m_cos, 1, 1);
    mod_add(rt, sc, "math", "tan", m_tan, 1, 1);
    mod_add(rt, sc, "math", "asin", m_asin, 1, 1);
    mod_add(rt, sc, "math", "acos", m_acos, 1, 1);
    mod_add(rt, sc, "math", "atan", m_atan, 1, 1);
    mod_add(rt, sc, "math", "exp", m_exp, 1, 1);
    mod_add(rt, sc, "math", "expm1", m_expm1, 1, 1);
    mod_add(rt, sc, "math", "ln", m_ln, 1, 1);
    mod_add(rt, sc, "math", "log1p", m_log1p, 1, 1);
    mod_add(rt, sc, "math", "log10", m_log10, 1, 1);
    mod_add(rt, sc, "math", "log2", m_log2, 1, 1);
    mod_add(rt, sc, "math", "sinh", m_sinh, 1, 1);
    mod_add(rt, sc, "math", "cosh", m_cosh, 1, 1);
    mod_add(rt, sc, "math", "tanh", m_tanh, 1, 1);
    mod_add(rt, sc, "math", "asinh", m_asinh, 1, 1);
    mod_add(rt, sc, "math", "acosh", m_acosh, 1, 1);
    mod_add(rt, sc, "math", "atanh", m_atanh, 1, 1);
    mod_add(rt, sc, "math", "deg2rad", m_deg2rad, 1, 1);
    mod_add(rt, sc, "math", "rad2deg", m_rad2deg, 1, 1);
    mod_add(rt, sc, "math", "isfinite", m_isfinite, 1, 1);
    mod_add(rt, sc, "math", "isnan", m_isnan, 1, 1);
    mod_add(rt, sc, "math", "isinf", m_isinf, 1, 1);
    mod_add(rt, sc, "math", "atan2", m_atan2, 2, 2);
    mod_add(rt, sc, "math", "log", m_log, 2, 2);
    mod_add(rt, sc, "math", "hypot", m_hypot, 2, 2);
    mod_add(rt, sc, "math", "gcd", m_gcd, 2, 2);
    mod_add(rt, sc, "math", "lcm", m_lcm, 2, 2);
    mod_add(rt, sc, "math", "factorial", m_factorial, 1, 1);
    mod_add(rt, sc, "math", "comb", m_comb, 2, 2);
    mod_add(rt, sc, "math", "perm", m_perm, 2, 2);
    mod_add(rt, sc, "math", "is_prime", m_is_prime, 1, 1);
    mod_add(rt, sc, "math", "factors", m_factors, 1, 1);
    mod_add(rt, sc, "math", "linspace", m_linspace, 3, 3);
    mod_add(rt, sc, "math", "arange", m_arange, 2, 3);
    mod_add(rt, sc, "math", "dot", m_dot, 2, 2);
    mod_add(rt, sc, "math", "norm", m_norm, 1, 1);
    mod_add(rt, sc, "math", "cumsum", m_cumsum, 1, 1);
    mod_add(rt, sc, "math", "diff", m_diff, 1, 1);
    mod_add(rt, sc, "math", "band", m_band, 2, 2);
    mod_add(rt, sc, "math", "bor", m_bor, 2, 2);
    mod_add(rt, sc, "math", "bxor", m_bxor, 2, 2);
    mod_add(rt, sc, "math", "bnot", m_bnot, 1, 1);
    mod_add(rt, sc, "math", "shl", m_shl, 2, 2);
    mod_add(rt, sc, "math", "shr", m_shr, 2, 2);
    mod_alias_builtin(rt, sc, "clamp", "clamp", 0, 0);
    return sc;
}

/* ============================== std.const ============================== */
static TEnv* build_mod_const(TRuntime* rt) {
    TEnv* sc = thrum_env_new(rt, rt->builtins, false);
    mod_add_val(rt, sc, "c", thrum_qty(rt, 299792458.0, 0.0, thrum_dim(1, 0, -1, 0, 0, 0, 0), 1.0, "m/s"));
    mod_add_val(rt, sc, "h", thrum_qty(rt, 6.62607015e-34, 0.0, thrum_dim(2, 1, -1, 0, 0, 0, 0), 1.0, "J*s"));
    mod_add_val(rt, sc, "hbar", thrum_qty(rt, 1.054571817e-34, 0.0, thrum_dim(2, 1, -1, 0, 0, 0, 0), 1.0, "J*s"));
    mod_add_val(rt, sc, "kB", thrum_qty(rt, 1.380649e-23, 0.0, thrum_dim(2, 1, -2, 0, -1, 0, 0), 1.0, "J/K"));
    mod_add_val(rt, sc, "NA", thrum_qty(rt, 6.02214076e23, 0.0, thrum_dim(0, 0, 0, 0, 0, -1, 0), 1.0, "1/mol"));
    mod_add_val(rt, sc, "e", thrum_qty(rt, 1.602176634e-19, 0.0, thrum_dim(0, 0, 1, 1, 0, 0, 0), 1.0, "C"));
    mod_add_val(rt, sc, "G", thrum_qty(rt, 6.67430e-11, 1.5e-15, thrum_dim(3, -1, -2, 0, 0, 0, 0), 1.0, "m^3/(kg*s^2)"));
    mod_add_val(rt, sc, "eps0", thrum_qty(rt, 8.8541878128e-12, 1.3e-21, thrum_dim(-3, -1, 4, 2, 0, 0, 0), 1.0, "F/m"));
    mod_add_val(rt, sc, "mu0", thrum_qty(rt, 1.25663706212e-6, 1.9e-16, thrum_dim(1, 1, -2, -2, 0, 0, 0), 1.0, "N/A^2"));
    mod_add_val(rt, sc, "me", thrum_qty(rt, 9.1093837015e-31, 2.8e-40, thrum_dim(0, 1, 0, 0, 0, 0, 0), 1.0, "kg"));
    mod_add_val(rt, sc, "mp", thrum_qty(rt, 1.67262192369e-27, 5.1e-37, thrum_dim(0, 1, 0, 0, 0, 0, 0), 1.0, "kg"));
    mod_add_val(rt, sc, "mn", thrum_qty(rt, 1.67492749804e-27, 9.5e-37, thrum_dim(0, 1, 0, 0, 0, 0, 0), 1.0, "kg"));
    mod_add_val(rt, sc, "u", thrum_qty(rt, 1.66053906660e-27, 5.0e-37, thrum_dim(0, 1, 0, 0, 0, 0, 0), 1.0, "kg"));
    mod_add_val(rt, sc, "alpha", thrum_qty(rt, 7.2973525693e-3, 1.1e-12, thrum_d0(), 1.0, NULL));
    mod_add_val(rt, sc, "Ry", thrum_qty(rt, 10973731.568160, 2.1e-5, thrum_dim(-1, 0, 0, 0, 0, 0, 0), 1.0, "1/m"));
    mod_add_val(rt, sc, "F", thrum_qty(rt, 96485.3321233100184, 0.0, thrum_dim(0, 0, 1, 1, 0, -1, 0), 1.0, "C/mol"));
    mod_add_val(rt, sc, "muB", thrum_qty(rt, 9.2740100783e-24, 2.8e-33, thrum_dim(2, 0, 0, 1, 0, 0, 0), 1.0, "J/T"));
    mod_add_val(rt, sc, "R", thrum_qty(rt, 8.314462618, 0.0, thrum_dim(2, 1, -2, 0, -1, -1, 0), 1.0, "J/(mol*K)"));
    mod_add_val(rt, sc, "g0", thrum_qty(rt, 9.80665, 0.0, thrum_dim(1, 0, -2, 0, 0, 0, 0), 1.0, "m/s^2"));
    mod_add_val(rt, sc, "atm", thrum_qty(rt, 101325.0, 0.0, thrum_dim(-1, 1, -2, 0, 0, 0, 0), 1.0, "Pa"));
    mod_add_val(rt, sc, "sigma", thrum_qty(rt, 5.670374419e-8, 0.0, thrum_dim(0, 1, -3, 0, -4, 0, 0), 1.0, "W/(m^2*K^4)"));
    mod_add_val(rt, sc, "au", thrum_qty(rt, 1.495978707e11, 0.0, thrum_dim(1, 0, 0, 0, 0, 0, 0), 1.0, "m"));
    mod_add_val(rt, sc, "ly", thrum_qty(rt, 9.4607304725808e15, 0.0, thrum_dim(1, 0, 0, 0, 0, 0, 0), 1.0, "m"));
    mod_add_val(rt, sc, "pc", thrum_qty(rt, 3.085677581491367e16, 0.0, thrum_dim(1, 0, 0, 0, 0, 0, 0), 1.0, "m"));
    mod_add_val(rt, sc, "M_sun", thrum_qty(rt, 1.98847e30, 7e25, thrum_dim(0, 1, 0, 0, 0, 0, 0), 1.0, "kg"));
    mod_add_val(rt, sc, "M_earth", thrum_qty(rt, 5.97217e24, 6e19, thrum_dim(0, 1, 0, 0, 0, 0, 0), 1.0, "kg"));
    mod_add_val(rt, sc, "R_earth", thrum_qty(rt, 6.371e6, 0.0, thrum_dim(1, 0, 0, 0, 0, 0, 0), 1.0, "m"));
    mod_add_val(rt, sc, "L_sun", thrum_qty(rt, 3.828e26, 0.0, thrum_dim(2, 1, -3, 0, 0, 0, 0), 1.0, "W"));
    mod_add_val(rt, sc, "T_cmb", thrum_qty(rt, 2.72548, 0.00057, thrum_dim(0, 0, 0, 0, 1, 0, 0), 1.0, "K"));
    return sc;
}

/* ============================== std.stat ============================== */
static TVal st_linfit(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TList* xs = need_list(rt, argv[0], "linfit", line, col);
    TList* ys = need_list(rt, argv[1], "linfit", line, col);
    size_t n = xs->len;
    if (n < 2 || n != ys->len) thrum_error(rt, line, col, "linfit needs equal lists of length >= 2", NULL);
    TVal mx = bi_mean(rt, 1, &argv[0], line, col);
    TVal my = bi_mean(rt, 1, &argv[1], line, col);
    TVal sxy = thrum_num_dimless(0.0), sxx = thrum_num_dimless(0.0);
    for (size_t i = 0; i < n; i++) {
        TVal dx = thrum_sub(rt, xs->items[i], mx, line, col);
        TVal dy = thrum_sub(rt, ys->items[i], my, line, col);
        TVal pxy = thrum_mul(rt, dx, dy, line, col);
        TVal pxx = thrum_mul(rt, dx, dx, line, col);
        sxy = (i == 0) ? pxy : thrum_add(rt, sxy, pxy, line, col);
        sxx = (i == 0) ? pxx : thrum_add(rt, sxx, pxx, line, col);
    }
    TVal slope = thrum_div(rt, sxy, sxx, line, col);
    TVal inter = thrum_sub(rt, my, thrum_mul(rt, slope, mx, line, col), line, col);
    TVal ssr = thrum_num_dimless(0.0), sst = thrum_num_dimless(0.0);
    for (size_t i = 0; i < n; i++) {
        TVal yhat = thrum_add(rt, inter, thrum_mul(rt, slope, xs->items[i], line, col), line, col);
        TVal r = thrum_pow(rt, thrum_sub(rt, ys->items[i], yhat, line, col), thrum_num_dimless(2.0), line, col);
        TVal t = thrum_pow(rt, thrum_sub(rt, ys->items[i], my, line, col), thrum_num_dimless(2.0), line, col);
        ssr = (i == 0) ? r : thrum_add(rt, ssr, r, line, col);
        sst = (i == 0) ? t : thrum_add(rt, sst, t, line, col);
    }
    double r2 = (sst.as.q.v != 0.0) ? (1.0 - ssr.as.q.v / sst.as.q.v) : 1.0;
    T_FRAME(rt);
    T_VAR(rt, out, thrum_map(rt));
    TMap* m = (TMap*)out.as.obj;
    thrum_map_set_cs(rt, m, "slope", slope);
    thrum_map_set_cs(rt, m, "intercept", inter);
    thrum_map_set_cs(rt, m, "r2", thrum_num_dimless(r2));
    thrum_map_set_cs(rt, m, "n", thrum_num_dimless((double)n));
    T_END_FRAME(rt);
    return out;
}

static TVal st_variance(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TList* xs = need_list(rt, argv[0], "variance", line, col);
    if (xs->len < 2) thrum_error(rt, line, col, "variance needs >= 2 values", NULL);
    TVal m = bi_mean(rt, 1, argv, line, col);
    TVal acc = thrum_num_dimless(0.0);
    for (size_t i = 0; i < xs->len; i++) {
        TVal sq = thrum_pow(rt, thrum_sub(rt, xs->items[i], m, line, col), thrum_num_dimless(2.0), line, col);
        acc = (i == 0) ? sq : thrum_add(rt, acc, sq, line, col);
    }
    return thrum_div(rt, acc, thrum_num_dimless((double)(xs->len - 1)), line, col);
}

static TVal st_quantile(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, sorted, bi_sort(rt, 1, &argv[0], line, col));
    TList* xs = (TList*)sorted.as.obj;
    if (xs->len == 0) thrum_error(rt, line, col, "quantile of empty list", NULL);
    TQ q = thrum_need_q(rt, argv[1], line, col);
    if (!thrum_dimless(q.d) || q.v < 0.0 || q.v > 1.0) thrum_error(rt, line, col, "quantile probability must be in [0, 1]", NULL);
    TVal out;
    if (xs->len == 1) {
        out = xs->items[0];
    } else {
        double pos = q.v * (double)(xs->len - 1);
        size_t lo = (size_t)floor(pos), hi = (size_t)ceil(pos);
        if (lo == hi) out = xs->items[lo];
        else {
            TVal diff = thrum_sub(rt, xs->items[hi], xs->items[lo], line, col);
            out = thrum_add(rt, xs->items[lo], thrum_mul(rt, diff, thrum_num_dimless(pos - (double)lo), line, col), line, col);
        }
    }
    T_END_FRAME(rt);
    return out;
}

static TVal st_iqr(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TVal a75[2] = {argv[0], thrum_num_dimless(0.75)};
    TVal a25[2] = {argv[0], thrum_num_dimless(0.25)};
    return thrum_sub(rt, st_quantile(rt, 2, a75, line, col), st_quantile(rt, 2, a25, line, col), line, col);
}

static TVal st_cov(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TList* xs = need_list(rt, argv[0], "cov", line, col);
    TList* ys = need_list(rt, argv[1], "cov", line, col);
    if (xs->len < 2 || xs->len != ys->len) thrum_error(rt, line, col, "cov needs equal lists of length >= 2", NULL);
    TVal mx = bi_mean(rt, 1, &argv[0], line, col);
    TVal my = bi_mean(rt, 1, &argv[1], line, col);
    TVal acc = thrum_num_dimless(0.0);
    for (size_t i = 0; i < xs->len; i++) {
        TVal p = thrum_mul(rt, thrum_sub(rt, xs->items[i], mx, line, col), thrum_sub(rt, ys->items[i], my, line, col), line, col);
        acc = (i == 0) ? p : thrum_add(rt, acc, p, line, col);
    }
    return thrum_div(rt, acc, thrum_num_dimless((double)(xs->len - 1)), line, col);
}

static TVal st_corr(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TVal sx = bi_stdev(rt, 1, &argv[0], line, col);
    TVal sy = bi_stdev(rt, 1, &argv[1], line, col);
    TVal c = st_cov(rt, 2, argv, line, col);
    return thrum_div(rt, c, thrum_mul(rt, sx, sy, line, col), line, col);
}

static TVal st_wmean(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TList* xs = need_list(rt, argv[0], "weighted_mean", line, col);
    if (xs->len == 0) thrum_error(rt, line, col, "weighted_mean of empty list", NULL);
    TQ q0 = thrum_need_q(rt, xs->items[0], line, col);
    bool all_u = q0.u > 0.0;
    for (size_t i = 1; i < xs->len; i++) {
        TQ q = thrum_need_q(rt, xs->items[i], line, col);
        if (!thrum_dim_eq(q.d, q0.d)) thrum_error(rt, line, col, "dimension mismatch in weighted_mean", NULL);
        if (q.u <= 0.0) all_u = false;
    }
    if (all_u) {
        double wsum = 0.0, num = 0.0;
        for (size_t i = 0; i < xs->len; i++) {
            TQ q = xs->items[i].as.q;
            double w = 1.0 / (q.u * q.u);
            wsum += w;
            num += w * q.v;
        }
        return thrum_qty(rt, num / wsum, sqrt(1.0 / wsum), q0.d, q0.disp_sc, q0.disp_sym);
    }
    TVal m = bi_mean(rt, 1, argv, line, col);
    if (xs->len >= 2) {
        TVal sd = bi_stdev(rt, 1, argv, line, col);
        double se = sd.as.q.v / sqrt((double)xs->len);
        double u = m.as.q.u > se ? m.as.q.u : se;
        return thrum_qty(rt, m.as.q.v, u, q0.d, q0.disp_sc, q0.disp_sym);
    }
    return m;
}

static TVal st_zscore(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, xv, argv[0]);
    TList* xs = need_list(rt, xv, "zscore", line, col);
    T_VAR(rt, m, bi_mean(rt, 1, &xv, line, col));
    T_VAR(rt, sd, bi_stdev(rt, 1, &xv, line, col));
    T_VAR(rt, out, thrum_list(rt, xs->len));
    TList* l = (TList*)out.as.obj;
    for (size_t i = 0; i < xs->len; i++) {
        thrum_list_push(rt, l, thrum_div(rt, thrum_sub(rt, xs->items[i], m, line, col), sd, line, col));
    }
    T_END_FRAME(rt);
    return out;
}

static TVal st_normalize(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, xv, argv[0]);
    TList* xs = need_list(rt, xv, "normalize", line, col);
    T_VAR(rt, lo, list_extreme(rt, xs, true, line, col));
    T_VAR(rt, hi, list_extreme(rt, xs, false, line, col));
    T_VAR(rt, span, thrum_sub(rt, hi, lo, line, col));
    T_VAR(rt, out, thrum_list(rt, xs->len));
    TList* l = (TList*)out.as.obj;
    for (size_t i = 0; i < xs->len; i++) {
        if (span.as.q.v == 0.0) thrum_list_push(rt, l, thrum_num_dimless(0.0));
        else thrum_list_push(rt, l, thrum_div(rt, thrum_sub(rt, xs->items[i], lo, line, col), span, line, col));
    }
    T_END_FRAME(rt);
    return out;
}

static TVal st_histogram(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    T_FRAME(rt);
    T_VAR(rt, xv, argv[0]);
    TList* xs = need_list(rt, xv, "histogram", line, col);
    int64_t bins = (argc > 1) ? thrum_need_int(rt, argv[1], line, col) : 10;
    if (xs->len == 0 || bins < 1) thrum_error(rt, line, col, "histogram needs non-empty list and bins >= 1", NULL);
    for (size_t i = 0; i < xs->len; i++) thrum_need_q(rt, xs->items[i], line, col);
    T_VAR(rt, lo, list_extreme(rt, xs, true, line, col));
    T_VAR(rt, hi, list_extreme(rt, xs, false, line, col));
    T_VAR(rt, width, thrum_div(rt, thrum_sub(rt, hi, lo, line, col), thrum_num_dimless((double)bins), line, col));
    int* counts = (int*)calloc((size_t)bins, sizeof(int));
    if (width.as.q.v == 0.0) {
        counts[0] = (int)xs->len;
    } else {
        for (size_t i = 0; i < xs->len; i++) {
            int64_t idx = (int64_t)((xs->items[i].as.q.v - lo.as.q.v) / width.as.q.v);
            if (idx >= bins) idx = bins - 1;
            if (idx < 0) idx = 0;
            counts[idx]++;
        }
    }
    T_VAR(rt, out, thrum_list(rt, (size_t)bins));
    TList* ol = (TList*)out.as.obj;
    for (int64_t i = 0; i < bins; i++) {
        TVal b0 = thrum_add(rt, lo, thrum_mul(rt, width, thrum_num_dimless((double)i), line, col), line, col);
        TVal b1 = thrum_add(rt, lo, thrum_mul(rt, width, thrum_num_dimless((double)(i + 1)), line, col), line, col);
        T_VAR(rt, mp, thrum_map(rt));
        TMap* m = (TMap*)mp.as.obj;
        thrum_map_set_cs(rt, m, "low", b0);
        thrum_map_set_cs(rt, m, "high", b1);
        thrum_map_set_cs(rt, m, "count", thrum_num_dimless((double)counts[i]));
        thrum_list_push(rt, ol, mp);
    }
    free(counts);
    T_END_FRAME(rt);
    return out;
}

static TVal st_mode(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TList* xs = need_list(rt, argv[0], "mode", line, col);
    if (xs->len == 0) thrum_error(rt, line, col, "mode of empty list", NULL);
    int* cnt = (int*)calloc(xs->len, sizeof(int));
    size_t best_i = 0;
    for (size_t i = 0; i < xs->len; i++) {
        for (size_t j = 0; j <= i; j++) {
            if (thrum_veq(xs->items[j], xs->items[i])) {
                cnt[j]++;
                if (cnt[j] > cnt[best_i]) best_i = j;
                break;
            }
        }
    }
    TVal out = xs->items[best_i];
    free(cnt);
    return out;
}

static TEnv* build_mod_stat(TRuntime* rt) {
    TEnv* sc = thrum_env_new(rt, rt->builtins, false);
    mod_add(rt, sc, "stat", "linfit", st_linfit, 2, 2);
    mod_alias_builtin(rt, sc, "mean", "mean", 0, 0);
    mod_alias_builtin(rt, sc, "stdev", "stdev", 0, 0);
    mod_alias_builtin(rt, sc, "median", "median", 0, 0);
    mod_add(rt, sc, "stat", "variance", st_variance, 1, 1);
    mod_add(rt, sc, "stat", "quantile", st_quantile, 2, 2);
    mod_add(rt, sc, "stat", "iqr", st_iqr, 1, 1);
    mod_add(rt, sc, "stat", "cov", st_cov, 2, 2);
    mod_add(rt, sc, "stat", "corr", st_corr, 2, 2);
    mod_add(rt, sc, "stat", "weighted_mean", st_wmean, 1, 1);
    mod_add(rt, sc, "stat", "zscore", st_zscore, 1, 1);
    mod_add(rt, sc, "stat", "normalize", st_normalize, 1, 1);
    mod_add(rt, sc, "stat", "histogram", st_histogram, 1, 2);
    mod_add(rt, sc, "stat", "mode", st_mode, 1, 1);
    return sc;
}

/* ============================== std.units ============================== */
static TVal u_define(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    if (argv[0].kind != TV_OBJ || argv[0].as.obj->kind != OBJ_STR ||
        argv[2].kind != TV_OBJ || argv[2].as.obj->kind != OBJ_STR) {
        thrum_error(rt, line, col, "units.define expects (name: text, value: num, base_unit: text)", NULL);
    }
    const char* nm = ((TStr*)argv[0].as.obj)->data;
    const char* base = ((TStr*)argv[2].as.obj)->data;
    double sc = 1.0;
    TDim d = thrum_d0();
    thrum_parse_unit(rt, base, line, col, &sc, &d);
    TQ val = thrum_need_q(rt, argv[1], line, col);
    if (!thrum_dimless(val.d)) thrum_error(rt, line, col, "units.define scale factor must be dimensionless", NULL);
    thrum_unit_define(rt, nm, val.v * sc, d);
    return thrum_nil();
}

static TVal u_list(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)argv; (void)line; (void)col;
    return thrum_units_list(rt);
}

static TVal u_dim_of(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    return bi_dim(rt, argc, argv, line, col);
}

static TVal u_compatible(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    TQ y = thrum_need_q(rt, argv[1], line, col);
    return thrum_bool(thrum_dim_eq(x.d, y.d));
}

static TVal u_f2k(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    return thrum_qty(rt, (x.v - 32.0) * 5.0 / 9.0 + 273.15, x.u * 5.0 / 9.0, thrum_dim(0, 0, 0, 0, 1, 0, 0), 1.0, "K");
}

static TVal u_k2f(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TQ x = thrum_need_q(rt, argv[0], line, col);
    return thrum_qty(rt, (x.v - 273.15) * 9.0 / 5.0 + 32.0, x.u * 9.0 / 5.0, thrum_dim(0, 0, 0, 0, 1, 0, 0), 1.0, "K");
}

static TEnv* build_mod_units(TRuntime* rt) {
    TEnv* sc = thrum_env_new(rt, rt->builtins, false);
    mod_alias_builtin(rt, sc, "convert", "convert", 0, 0);
    mod_alias_builtin(rt, sc, "qty", "qty", 0, 0);
    mod_add(rt, sc, "units", "define", u_define, 3, 3);
    mod_add(rt, sc, "units", "list", u_list, 0, 0);
    mod_add(rt, sc, "units", "dim_of", u_dim_of, 1, 1);
    mod_add(rt, sc, "units", "compatible", u_compatible, 2, 2);
    mod_alias_builtin(rt, sc, "c2k", "c2k", 0, 0);
    mod_alias_builtin(rt, sc, "k2c", "k2c", 0, 0);
    mod_add(rt, sc, "units", "f2k", u_f2k, 1, 1);
    mod_add(rt, sc, "units", "k2f", u_k2f, 1, 1);
    return sc;
}

/* ============================== std.text ============================== */
static TVal tx_title(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "text.title", line, col);
    char* buf = (char*)malloc(s->len + 1);
    bool new_w = true;
    for (size_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        if (isalpha(c)) {
            buf[i] = (char)(new_w ? toupper(c) : tolower(c));
            new_w = false;
        } else {
            buf[i] = (char)c;
            new_w = true;
        }
    }
    TVal out = thrum_str(rt, buf, s->len);
    free(buf);
    return out;
}

static TVal tx_ltrim(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "text.ltrim", line, col);
    size_t i = 0;
    while (i < s->len && isspace((unsigned char)s->data[i])) i++;
    return thrum_str(rt, s->data + i, s->len - i);
}

static TVal tx_rtrim(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "text.rtrim", line, col);
    size_t j = s->len;
    while (j > 0 && isspace((unsigned char)s->data[j - 1])) j--;
    return thrum_str(rt, s->data, j);
}

static TVal tx_contains(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "text.contains", line, col);
    TStr* sub = need_str(rt, argv[1], "text.contains", line, col);
    return thrum_bool(sub->len == 0 || strstr(s->data, sub->data) != NULL);
}

static TVal tx_index_of(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "text.index_of", line, col);
    TStr* sub = need_str(rt, argv[1], "text.index_of", line, col);
    if (sub->len == 0) return thrum_num_dimless(0.0);
    const char* p = strstr(s->data, sub->data);
    return thrum_num_dimless(p ? (double)thrum_utf8_count(s->data, (size_t)(p - s->data)) : -1.0);
}

static TVal tx_count(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "text.count", line, col);
    TStr* sub = need_str(rt, argv[1], "text.count", line, col);
    if (sub->len == 0) return thrum_num_dimless((double)(s->ulen + 1));
    size_t c = 0, i = 0;
    while (i + sub->len <= s->len) {
        const char* p = strstr(s->data + i, sub->data);
        if (!p) break;
        c++;
        i = (size_t)(p - s->data) + sub->len;
    }
    return thrum_num_dimless((double)c);
}

static TVal tx_pad(TRuntime* rt, int argc, TVal* argv, int line, int col, bool left) {
    TStr* s = need_str(rt, argv[0], "pad", line, col);
    int64_t w = thrum_need_int(rt, argv[1], line, col);
    const char* fill = " ";
    size_t flen = 1;
    if (argc > 2) {
        TStr* ch = need_str(rt, argv[2], "pad", line, col);
        if (ch->ulen != 1) thrum_error(rt, line, col, "pad fill must be a single character", NULL);
        fill = ch->data;
        flen = ch->len;
    }
    if (w <= (int64_t)s->ulen) return argv[0];
    BSB sb = {0};
    size_t pad = (size_t)(w - (int64_t)s->ulen);
    if (left) {
        for (size_t k = 0; k < pad; k++) bsb_app_n(&sb, fill, flen);
        bsb_app_n(&sb, s->data, s->len);
    } else {
        bsb_app_n(&sb, s->data, s->len);
        for (size_t k = 0; k < pad; k++) bsb_app_n(&sb, fill, flen);
    }
    TVal out = thrum_str(rt, sb.data, sb.len);
    free(sb.data);
    return out;
}

static TVal tx_pad_left(TRuntime* rt, int argc, TVal* argv, int line, int col) { return tx_pad(rt, argc, argv, line, col, true); }
static TVal tx_pad_right(TRuntime* rt, int argc, TVal* argv, int line, int col) { return tx_pad(rt, argc, argv, line, col, false); }

static TVal tx_repeat(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "text.repeat", line, col);
    int64_t n = thrum_need_int(rt, argv[1], line, col);
    if (n <= 0) return thrum_cstr(rt, "");
    BSB sb = {0};
    for (int64_t i = 0; i < n; i++) bsb_app_n(&sb, s->data, s->len);
    TVal out = thrum_str(rt, sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return out;
}

static TVal tx_chars(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, sv, argv[0]);
    TStr* s = need_str(rt, sv, "text.chars", line, col);
    T_VAR(rt, out, thrum_list(rt, s->ulen));
    TList* l = (TList*)out.as.obj;
    for (size_t off = 0; off < s->len; ) {
        size_t cp = thrum_utf8_cp_len((unsigned char)s->data[off]);
        if (off + cp > s->len) cp = s->len - off;
        thrum_list_push(rt, l, thrum_str(rt, s->data + off, cp));
        off += cp;
    }
    T_END_FRAME(rt);
    return out;
}

static TVal tx_lines(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, sv, argv[0]);
    TStr* s = need_str(rt, sv, "text.lines", line, col);
    T_VAR(rt, out, thrum_list(rt, 8));
    TList* l = (TList*)out.as.obj;
    size_t i = 0;
    while (i < s->len) {
        size_t st = i;
        while (i < s->len && s->data[i] != '\n' && s->data[i] != '\r') i++;
        thrum_list_push(rt, l, thrum_str(rt, s->data + st, i - st));
        if (i < s->len && s->data[i] == '\r') i++;
        if (i < s->len && s->data[i] == '\n') i++;
    }
    T_END_FRAME(rt);
    return out;
}

static TVal tx_words(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    return bi_split(rt, 1, argv, line, col);
}

static TVal tx_is_digit(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "text.is_digit", line, col);
    if (s->len == 0) return thrum_bool(false);
    for (size_t i = 0; i < s->len; i++) if (!isdigit((unsigned char)s->data[i])) return thrum_bool(false);
    return thrum_bool(true);
}

static TVal tx_is_alpha(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "text.is_alpha", line, col);
    if (s->len == 0) return thrum_bool(false);
    for (size_t i = 0; i < s->len; i++) if (!isalpha((unsigned char)s->data[i])) return thrum_bool(false);
    return thrum_bool(true);
}

static TVal tx_format(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    if (argc < 1) thrum_error(rt, line, col, "text.format expects template", NULL);
    TStr* tmpl = need_str(rt, argv[0], "text.format", line, col);
    int nargs = argc - 1;
    char** sargs = (char**)calloc((size_t)(nargs > 0 ? nargs : 1), sizeof(char*));
    for (int i = 0; i < nargs; i++) sargs[i] = thrum_vstr_alloc(rt, argv[i + 1], false);
    BSB sb = {0};
    int next_auto = 0;
    for (size_t i = 0; i < tmpl->len; i++) {
        char c = tmpl->data[i];
        if (c == '{') {
            if (i + 1 < tmpl->len && tmpl->data[i + 1] == '{') { bsb_ch(&sb, '{'); i++; continue; }
            size_t j = i + 1;
            while (j < tmpl->len && tmpl->data[j] != '}') j++;
            if (j >= tmpl->len) {
                for (int k = 0; k < nargs; k++) free(sargs[k]);
                free(sargs); free(sb.data);
                thrum_error(rt, line, col, "text.format error: single '{' encountered in format string", NULL);
            }
            size_t flen = j - (i + 1);
            int idx = -1;
            if (flen == 0) {
                idx = next_auto++;
            } else {
                bool digits = true;
                for (size_t k = 0; k < flen; k++) if (!isdigit((unsigned char)tmpl->data[i + 1 + k])) { digits = false; break; }
                if (digits) idx = atoi(tmpl->data + i + 1);
            }
            if (idx < 0 || idx >= nargs) {
                for (int k = 0; k < nargs; k++) free(sargs[k]);
                free(sargs); free(sb.data);
                thrum_error(rt, line, col, "text.format error: replacement index out of range", NULL);
            }
            bsb_app(&sb, sargs[idx]);
            i = j;
        } else if (c == '}' && i + 1 < tmpl->len && tmpl->data[i + 1] == '}') {
            bsb_ch(&sb, '}');
            i++;
        } else {
            bsb_ch(&sb, c);
        }
    }
    for (int k = 0; k < nargs; k++) free(sargs[k]);
    free(sargs);
    TVal out = thrum_str(rt, sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return out;
}

static TVal tx_ord(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "text.ord", line, col);
    if (s->ulen != 1) thrum_error(rt, line, col, "text.ord expects a single character", NULL);
    const unsigned char* p = (const unsigned char*)s->data;
    uint32_t cp = 0;
    if (p[0] < 0x80) cp = p[0];
    else if ((p[0] & 0xE0) == 0xC0 && s->len >= 2) cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    else if ((p[0] & 0xF0) == 0xE0 && s->len >= 3) cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    else if ((p[0] & 0xF8) == 0xF0 && s->len >= 4) cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    else cp = p[0];
    return thrum_num_dimless((double)cp);
}

static TVal tx_chr(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    int64_t cp = thrum_need_int(rt, argv[0], line, col);
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        thrum_error(rt, line, col, "text.chr codepoint out of valid UTF-8 range", NULL);
    }
    char buf[5];
    size_t n = 0;
    if (cp <= 0x7F) {
        buf[0] = (char)cp; n = 1;
    } else if (cp <= 0x7FF) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp <= 0xFFFF) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    buf[n] = '\0';
    return thrum_str(rt, buf, n);
}

static TVal tx_to_radix(TRuntime* rt, int64_t v, int base) {
    if (v == 0) return thrum_cstr(rt, "0");
    bool neg = v < 0;
    uint64_t uv = neg ? (uint64_t)(-(v + 1)) + 1ULL : (uint64_t)v;
    char tmp[72];
    int pos = 71;
    tmp[pos] = '\0';
    const char* digs = "0123456789abcdef";
    while (uv > 0) {
        tmp[--pos] = digs[uv % (uint64_t)base];
        uv /= (uint64_t)base;
    }
    if (neg) tmp[--pos] = '-';
    return thrum_cstr(rt, tmp + pos);
}

static TVal tx_to_hex(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    return tx_to_radix(rt, thrum_need_int(rt, argv[0], line, col), 16);
}

static TVal tx_to_bin(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    return tx_to_radix(rt, thrum_need_int(rt, argv[0], line, col), 2);
}

static TVal tx_from_hex(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "text.from_hex", line, col);
    const char* p = s->data;
    while (*p && isspace((unsigned char)*p)) p++;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    else if (*p == '+') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    const char* start = p;
    uint64_t acc = 0;
    while (*p && isxdigit((unsigned char)*p)) {
        unsigned char c = (unsigned char)*p++;
        uint64_t d = (c >= '0' && c <= '9') ? (uint64_t)(c - '0') :
                     (c >= 'a' && c <= 'f') ? (uint64_t)(c - 'a' + 10) : (uint64_t)(c - 'A' + 10);
        acc = (acc << 4) | d;
    }
    while (*p && isspace((unsigned char)*p)) p++;
    if (p == start || *p != '\0') thrum_error(rt, line, col, "invalid hex integer", NULL);
    return thrum_num_dimless(neg ? -(double)acc : (double)acc);
}

static TEnv* build_mod_text(TRuntime* rt) {
    TEnv* sc = thrum_env_new(rt, rt->builtins, false);
    mod_alias_builtin(rt, sc, "split", "split", 0, 0);
    mod_alias_builtin(rt, sc, "join", "join", 0, 0);
    mod_alias_builtin(rt, sc, "upper", "upper", 0, 0);
    mod_alias_builtin(rt, sc, "lower", "lower", 0, 0);
    mod_add(rt, sc, "text", "title", tx_title, 1, 1);
    mod_alias_builtin(rt, sc, "trim", "trim", 0, 0);
    mod_add(rt, sc, "text", "ltrim", tx_ltrim, 1, 1);
    mod_add(rt, sc, "text", "rtrim", tx_rtrim, 1, 1);
    mod_alias_builtin(rt, sc, "replace", "replace", 0, 0);
    mod_alias_builtin(rt, sc, "starts", "starts", 0, 0);
    mod_alias_builtin(rt, sc, "ends", "ends", 0, 0);
    mod_add(rt, sc, "text", "contains", tx_contains, 2, 2);
    mod_add(rt, sc, "text", "index_of", tx_index_of, 2, 2);
    mod_add(rt, sc, "text", "count", tx_count, 2, 2);
    mod_add(rt, sc, "text", "pad_left", tx_pad_left, 2, 3);
    mod_add(rt, sc, "text", "pad_right", tx_pad_right, 2, 3);
    mod_add(rt, sc, "text", "repeat", tx_repeat, 2, 2);
    mod_add(rt, sc, "text", "chars", tx_chars, 1, 1);
    mod_add(rt, sc, "text", "lines", tx_lines, 1, 1);
    mod_add(rt, sc, "text", "words", tx_words, 1, 1);
    mod_add(rt, sc, "text", "is_digit", tx_is_digit, 1, 1);
    mod_add(rt, sc, "text", "is_alpha", tx_is_alpha, 1, 1);
    mod_add(rt, sc, "text", "format", tx_format, 1, -1);
    mod_add(rt, sc, "text", "ord", tx_ord, 1, 1);
    mod_add(rt, sc, "text", "chr", tx_chr, 1, 1);
    mod_add(rt, sc, "text", "to_hex", tx_to_hex, 1, 1);
    mod_add(rt, sc, "text", "to_bin", tx_to_bin, 1, 1);
    mod_add(rt, sc, "text", "from_hex", tx_from_hex, 1, 1);
    return sc;
}

/* ============================== std.regex ============================== */
static char* expand_ere_pattern(const char* pat) {
    BSB sb = {0};
    bool in_cls = false;
    for (size_t i = 0; pat[i]; i++) {
        char c = pat[i];
        if (!in_cls && c == '(' && pat[i + 1] == '?' && pat[i + 2] == ':') {
            bsb_ch(&sb, '(');
            i += 2;
            continue;
        }
        if (c == '\\' && pat[i + 1]) {
            char n = pat[i + 1];
            const char* exp_out = NULL;
            const char* exp_in = NULL;
            if (n == 'd') { exp_out = "[0-9]"; exp_in = "0-9"; }
            else if (n == 'D') { exp_out = "[^0-9]"; }
            else if (n == 's') { exp_out = "[ \t\n\r\f\v]"; exp_in = " \t\n\r\f\v"; }
            else if (n == 'S') { exp_out = "[^ \t\n\r\f\v]"; }
            else if (n == 'w') { exp_out = "[A-Za-z0-9_]"; exp_in = "A-Za-z0-9_"; }
            else if (n == 'W') { exp_out = "[^A-Za-z0-9_]"; }
            if (in_cls && exp_in) { bsb_app(&sb, exp_in); i++; continue; }
            if (!in_cls && exp_out) { bsb_app(&sb, exp_out); i++; continue; }
            if (n == 'n') { bsb_ch(&sb, '\n'); i++; continue; }
            if (n == 't') { bsb_ch(&sb, '\t'); i++; continue; }
            if (n == 'r') { bsb_ch(&sb, '\r'); i++; continue; }
            bsb_ch(&sb, '\\');
            bsb_ch(&sb, n);
            i++;
            continue;
        }
        if (c == '[' && !in_cls) in_cls = true;
        else if (c == ']' && in_cls) in_cls = false;
        bsb_ch(&sb, c);
    }
    return sb.data ? sb.data : strdup("");
}

static void compile_rx(TRuntime* rt, TVal patv, regex_t* rx, int line, int col) {
    TStr* ps = need_str(rt, patv, "regex", line, col);
    char* ep = expand_ere_pattern(ps->data);
    int rc = regcomp(rx, ep, REG_EXTENDED);
    free(ep);
    if (rc != 0) {
        char err[128], msg[256];
        regerror(rc, rx, err, sizeof(err));
        snprintf(msg, sizeof(msg), "invalid regex '%s': %s", ps->data, err);
        regfree(rx);
        thrum_error(rt, line, col, msg, NULL);
    }
}

static TVal make_rx_match(TRuntime* rt, const char* s, size_t nsub, regmatch_t* pm) {
    T_FRAME(rt);
    T_VAR(rt, mp, thrum_map(rt));
    TMap* m = (TMap*)mp.as.obj;
    size_t st = (size_t)pm[0].rm_so, en = (size_t)pm[0].rm_eo;
    thrum_map_set_cs(rt, m, "text", thrum_str(rt, s + st, en - st));
    thrum_map_set_cs(rt, m, "start", thrum_num_dimless((double)thrum_utf8_count(s, st)));
    thrum_map_set_cs(rt, m, "end", thrum_num_dimless((double)thrum_utf8_count(s, en)));
    T_VAR(rt, grps, thrum_list(rt, nsub));
    TList* gl = (TList*)grps.as.obj;
    for (size_t k = 1; k <= nsub && k < 32; k++) {
        if (pm[k].rm_so >= 0) {
            thrum_list_push(rt, gl, thrum_str(rt, s + pm[k].rm_so, (size_t)(pm[k].rm_eo - pm[k].rm_so)));
        } else {
            thrum_list_push(rt, gl, thrum_cstr(rt, ""));
        }
    }
    thrum_map_set_cs(rt, m, "groups", grps);
    T_END_FRAME(rt);
    return mp;
}

static TVal rx_test(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    regex_t rx;
    compile_rx(rt, argv[0], &rx, line, col);
    TStr* s = need_str(rt, argv[1], "regex", line, col);
    int rc = regexec(&rx, s->data, 0, NULL, 0);
    regfree(&rx);
    return thrum_bool(rc == 0);
}

static TVal rx_search(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    regex_t rx;
    compile_rx(rt, argv[0], &rx, line, col);
    TStr* s = need_str(rt, argv[1], "regex", line, col);
    regmatch_t pm[32];
    int rc = regexec(&rx, s->data, 32, pm, 0);
    size_t nsub = rx.re_nsub;
    regfree(&rx);
    if (rc != 0) return thrum_nil();
    return make_rx_match(rt, s->data, nsub, pm);
}

static TVal rx_match(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    regex_t rx;
    compile_rx(rt, argv[0], &rx, line, col);
    TStr* s = need_str(rt, argv[1], "regex", line, col);
    regmatch_t pm[32];
    int rc = regexec(&rx, s->data, 32, pm, 0);
    size_t nsub = rx.re_nsub;
    regfree(&rx);
    if (rc != 0 || pm[0].rm_so != 0) return thrum_nil();
    return make_rx_match(rt, s->data, nsub, pm);
}

static TVal rx_find_all(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, sv, argv[1]);
    TStr* s = need_str(rt, sv, "regex", line, col);
    regex_t rx;
    compile_rx(rt, argv[0], &rx, line, col);
    T_VAR(rt, out, thrum_list(rt, 8));
    TList* l = (TList*)out.as.obj;
    size_t pos = 0;
    regmatch_t pm[32];
    while (pos <= s->len && regexec(&rx, s->data + pos, 32, pm, pos > 0 ? REG_NOTBOL : 0) == 0) {
        size_t g = (rx.re_nsub >= 1) ? 1 : 0;
        if (pm[g].rm_so >= 0) {
            thrum_list_push(rt, l, thrum_str(rt, s->data + pos + pm[g].rm_so, (size_t)(pm[g].rm_eo - pm[g].rm_so)));
        } else {
            thrum_list_push(rt, l, thrum_cstr(rt, ""));
        }
        size_t next = pos + (size_t)pm[0].rm_eo;
        if (pm[0].rm_so == pm[0].rm_eo) next++;
        pos = next;
    }
    regfree(&rx);
    T_END_FRAME(rt);
    return out;
}

static TVal rx_replace(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    regex_t rx;
    compile_rx(rt, argv[0], &rx, line, col);
    TStr* rep = need_str(rt, argv[1], "regex", line, col);
    TStr* s = need_str(rt, argv[2], "regex", line, col);
    BSB sb = {0};
    size_t pos = 0;
    regmatch_t pm[32];
    while (pos <= s->len && regexec(&rx, s->data + pos, 32, pm, pos > 0 ? REG_NOTBOL : 0) == 0) {
        bsb_app_n(&sb, s->data + pos, (size_t)pm[0].rm_so);
        bsb_app_n(&sb, rep->data, rep->len);
        if (pm[0].rm_so == pm[0].rm_eo) {
            if (pos + (size_t)pm[0].rm_eo >= s->len) break;
            bsb_ch(&sb, s->data[pos + (size_t)pm[0].rm_eo]);
            pos += (size_t)pm[0].rm_eo + 1;
        } else {
            pos += (size_t)pm[0].rm_eo;
        }
    }
    if (pos < s->len) bsb_app_n(&sb, s->data + pos, s->len - pos);
    regfree(&rx);
    TVal out = thrum_str(rt, sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return out;
}

static TVal rx_split(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, sv, argv[1]);
    TStr* s = need_str(rt, sv, "regex", line, col);
    regex_t rx;
    compile_rx(rt, argv[0], &rx, line, col);
    T_VAR(rt, out, thrum_list(rt, 8));
    TList* l = (TList*)out.as.obj;
    size_t pos = 0;
    regmatch_t pm[4];
    while (pos <= s->len && regexec(&rx, s->data + pos, 4, pm, pos > 0 ? REG_NOTBOL : 0) == 0) {
        if (pm[0].rm_so == pm[0].rm_eo) {
            if (pos >= s->len) break;
            pos++;
            continue;
        }
        thrum_list_push(rt, l, thrum_str(rt, s->data + pos, (size_t)pm[0].rm_so));
        pos += (size_t)pm[0].rm_eo;
    }
    thrum_list_push(rt, l, thrum_str(rt, s->data + pos, pos <= s->len ? (s->len - pos) : 0));
    regfree(&rx);
    T_END_FRAME(rt);
    return out;
}

static TEnv* build_mod_regex(TRuntime* rt) {
    TEnv* sc = thrum_env_new(rt, rt->builtins, false);
    mod_add(rt, sc, "regex", "test", rx_test, 2, 2);
    mod_add(rt, sc, "regex", "match", rx_match, 2, 2);
    mod_add(rt, sc, "regex", "search", rx_search, 2, 2);
    mod_add(rt, sc, "regex", "find_all", rx_find_all, 2, 2);
    mod_add(rt, sc, "regex", "replace", rx_replace, 3, 3);
    mod_add(rt, sc, "regex", "split", rx_split, 2, 2);
    return sc;
}

/* ============================== std.json ============================== */
static TVal js_parse(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "json.parse", line, col);
    return thrum_json_parse_str(rt, s->data, s->len, false, line, col);
}

static TVal js_stringify(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    int ind = (argc > 1) ? (int)thrum_need_int(rt, argv[1], line, col) : -1;
    char* res = thrum_json_dump_alloc(rt, argv[0], false, ind, line, col);
    TVal out = thrum_cstr(rt, res);
    free(res);
    return out;
}

static TVal js_parse_qty(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "json.parse_qty", line, col);
    return thrum_json_parse_str(rt, s->data, s->len, true, line, col);
}

static TVal js_stringify_qty(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    int ind = (argc > 1) ? (int)thrum_need_int(rt, argv[1], line, col) : -1;
    char* res = thrum_json_dump_alloc(rt, argv[0], true, ind, line, col);
    TVal out = thrum_cstr(rt, res);
    free(res);
    return out;
}

static TEnv* build_mod_json(TRuntime* rt) {
    TEnv* sc = thrum_env_new(rt, rt->builtins, false);
    mod_add(rt, sc, "json", "parse", js_parse, 1, 1);
    mod_add(rt, sc, "json", "stringify", js_stringify, 1, 2);
    mod_add(rt, sc, "json", "parse_qty", js_parse_qty, 1, 1);
    mod_add(rt, sc, "json", "stringify_qty", js_stringify_qty, 1, 2);
    return sc;
}

/* ============================== std.csv ============================== */
static TVal try_parse_csv_qty(TRuntime* rt, const char* cell, size_t len) {
    size_t i = 0, j = len;
    while (i < j && isspace((unsigned char)cell[i])) i++;
    while (j > i && isspace((unsigned char)cell[j - 1])) j--;
    if (i >= j) return thrum_str(rt, cell, len);
    const char* s = cell + i;
    size_t n = j - i;
    char* tmp = (char*)malloc(n + 1);
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    double sign = 1.0;
    size_t p = 0;
    if (tmp[p] == '-') { sign = -1.0; p++; }
    if (!isdigit((unsigned char)tmp[p]) && !(tmp[p] == '.' && isdigit((unsigned char)tmp[p + 1]))) {
        free(tmp);
        return thrum_str(rt, cell, len);
    }
    char* end = NULL;
    double val = strtod(tmp + p, &end);
    if (end == tmp + p) { free(tmp); return thrum_str(rt, cell, len); }
    val *= sign;
    p = (size_t)(end - tmp);
    double u = 0.0;
    while (tmp[p] == ' ' || tmp[p] == '\t') p++;
    if ((tmp[p] == '+' && tmp[p + 1] == '-') ||
        ((unsigned char)tmp[p] == 0xC2 && (unsigned char)tmp[p + 1] == 0xB1)) {
        p += 2;
        while (tmp[p] == ' ' || tmp[p] == '\t') p++;
        char* uend = NULL;
        u = strtod(tmp + p, &uend);
        if (uend == tmp + p) { free(tmp); return thrum_str(rt, cell, len); }
        p = (size_t)(uend - tmp);
        while (tmp[p] == ' ' || tmp[p] == '\t') p++;
    }
    if (tmp[p] == '\0') {
        free(tmp);
        return thrum_qty(rt, val, u, thrum_d0(), 1.0, NULL);
    }
    double sc = 1.0;
    TDim d = thrum_d0();
    int uend = 0;
    if (thrum_scan_unit(tmp, (int)p, &sc, &d, &uend) && tmp[uend] == '\0') {
        char* sym = strdup(tmp + p);
        TVal out = thrum_qty(rt, val * sc, u * sc, d, sc, sym);
        free(sym);
        free(tmp);
        return out;
    }
    free(tmp);
    return thrum_str(rt, cell, len);
}

static TVal csv_parse_impl(TRuntime* rt, int argc, TVal* argv, int line, int col, bool parse_nums) {
    TStr* raw = need_str(rt, argv[0], "csv.parse", line, col);
    bool header = argc > 1 ? thrum_truthy(argv[1]) : true;
    T_FRAME(rt);
    T_VAR(rt, rows, thrum_list(rt, 8));
    TList* rl = (TList*)rows.as.obj;
    size_t i = 0, n = raw->len;
    while (i < n) {
        T_VAR(rt, row, thrum_list(rt, 8));
        TList* rowl = (TList*)row.as.obj;
        for (;;) {
            BSB field = {0};
            if (i < n && raw->data[i] == '"') {
                i++;
                while (i < n) {
                    if (raw->data[i] == '"') {
                        if (i + 1 < n && raw->data[i + 1] == '"') { bsb_ch(&field, '"'); i += 2; }
                        else { i++; break; }
                    } else {
                        bsb_ch(&field, raw->data[i++]);
                    }
                }
            } else {
                while (i < n && raw->data[i] != ',' && raw->data[i] != '\n' && raw->data[i] != '\r') {
                    bsb_ch(&field, raw->data[i++]);
                }
            }
            const char* fd = field.data ? field.data : "";
            TVal cell = parse_nums ? try_parse_csv_qty(rt, fd, field.len) : thrum_str(rt, fd, field.len);
            free(field.data);
            thrum_list_push(rt, rowl, cell);
            if (i < n && raw->data[i] == ',') { i++; continue; }
            if (i < n && raw->data[i] == '\r') i++;
            if (i < n && raw->data[i] == '\n') i++;
            break;
        }
        thrum_list_push(rt, rl, row);
    }
    if (!header || rl->len == 0) {
        T_END_FRAME(rt);
        return rows;
    }
    TList* hdr = (TList*)rl->items[0].as.obj;
    T_VAR(rt, cols, thrum_list(rt, hdr->len));
    TList* cl = (TList*)cols.as.obj;
    for (size_t c = 0; c < hdr->len; c++) {
        TVal hv = hdr->items[c];
        char* hs = thrum_vstr_alloc(rt, hv, false);
        size_t st = 0, en = strlen(hs);
        while (st < en && isspace((unsigned char)hs[st])) st++;
        while (en > st && isspace((unsigned char)hs[en - 1])) en--;
        thrum_list_push(rt, cl, thrum_str(rt, hs + st, en - st));
        free(hs);
    }
    T_VAR(rt, out, thrum_list(rt, rl->len > 1 ? rl->len - 1 : 0));
    TList* ol = (TList*)out.as.obj;
    for (size_t r = 1; r < rl->len; r++) {
        TList* rr = (TList*)rl->items[r].as.obj;
        T_VAR(rt, mp, thrum_map(rt));
        TMap* m = (TMap*)mp.as.obj;
        for (size_t c = 0; c < cl->len; c++) {
            TVal v = (c < rr->len) ? rr->items[c] : thrum_cstr(rt, "");
            thrum_map_set(rt, m, cl->items[c], v);
        }
        thrum_list_push(rt, ol, mp);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal csv_parse(TRuntime* rt, int argc, TVal* argv, int line, int col) { return csv_parse_impl(rt, argc, argv, line, col, false); }
static TVal csv_parse_nums(TRuntime* rt, int argc, TVal* argv, int line, int col) { return csv_parse_impl(rt, argc, argv, line, col, true); }

static void csv_write_cell(BSB* sb, const char* s) {
    bool q = strchr(s, ',') || strchr(s, '"') || strchr(s, '\n') || strchr(s, '\r');
    if (!q) { bsb_app(sb, s); return; }
    bsb_ch(sb, '"');
    for (; *s; s++) {
        if (*s == '"') bsb_app(sb, "\"\"");
        else bsb_ch(sb, *s);
    }
    bsb_ch(sb, '"');
}

static TVal csv_stringify(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    TList* rows = need_list(rt, argv[0], "csv.stringify", line, col);
    TList* hdrs = argc > 1 ? need_list(rt, argv[1], "csv.stringify", line, col) : NULL;
    BSB sb = {0};
    if (rows->len == 0) {
        if (hdrs) {
            for (size_t c = 0; c < hdrs->len; c++) {
                if (c > 0) bsb_ch(&sb, ',');
                char* s = thrum_vstr_alloc(rt, hdrs->items[c], false);
                csv_write_cell(&sb, s);
                free(s);
            }
            bsb_ch(&sb, '\n');
        }
        TVal out = thrum_str(rt, sb.data ? sb.data : "", sb.len);
        free(sb.data);
        return out;
    }
    TVal r0 = rows->items[0];
    if (r0.kind == TV_OBJ && r0.as.obj->kind == OBJ_MAP) {
        T_FRAME(rt);
        T_VAR(rt, cols, thrum_list(rt, 8));
        TList* cl = (TList*)cols.as.obj;
        if (hdrs) {
            for (size_t c = 0; c < hdrs->len; c++) {
                char* s = thrum_vstr_alloc(rt, hdrs->items[c], false);
                thrum_list_push(rt, cl, thrum_cstr(rt, s));
                free(s);
            }
        } else {
            TMap* m0 = (TMap*)r0.as.obj;
            for (size_t k = 0; k < m0->order_len; k++) {
                if (thrum_map_has(m0, m0->order_keys[k])) thrum_list_push(rt, cl, m0->order_keys[k]);
            }
        }
        for (size_t c = 0; c < cl->len; c++) {
            if (c > 0) bsb_ch(&sb, ',');
            char* s = thrum_vstr_alloc(rt, cl->items[c], false);
            csv_write_cell(&sb, s);
            free(s);
        }
        bsb_ch(&sb, '\n');
        for (size_t r = 0; r < rows->len; r++) {
            TMap* rm = (rows->items[r].kind == TV_OBJ && rows->items[r].as.obj->kind == OBJ_MAP)
                           ? (TMap*)rows->items[r].as.obj
                           : NULL;
            for (size_t c = 0; c < cl->len; c++) {
                if (c > 0) bsb_ch(&sb, ',');
                TVal v = thrum_nil();
                if (rm) thrum_map_get(rm, cl->items[c], &v);
                char* s = thrum_vstr_alloc(rt, v, false);
                csv_write_cell(&sb, s);
                free(s);
            }
            bsb_ch(&sb, '\n');
        }
        T_END_FRAME(rt);
    } else if (r0.kind == TV_OBJ && r0.as.obj->kind == OBJ_RECVAL) {
        TRecType* rt_t = ((TRecVal*)r0.as.obj)->rtype;
        int ncols = hdrs ? (int)hdrs->len : rt_t->nfields;
        for (int c = 0; c < ncols; c++) {
            if (c > 0) bsb_ch(&sb, ',');
            if (hdrs) {
                char* s = thrum_vstr_alloc(rt, hdrs->items[c], false);
                csv_write_cell(&sb, s);
                free(s);
            } else {
                csv_write_cell(&sb, rt_t->fields[c].name);
            }
        }
        bsb_ch(&sb, '\n');
        for (size_t r = 0; r < rows->len; r++) {
            TRecVal* rv = (TRecVal*)rows->items[r].as.obj;
            for (int c = 0; c < ncols; c++) {
                if (c > 0) bsb_ch(&sb, ',');
                TVal v = thrum_nil();
                if (hdrs) {
                    char* fn = thrum_vstr_alloc(rt, hdrs->items[c], false);
                    for (int f = 0; f < rv->rtype->nfields; f++) {
                        if (strcmp(rv->rtype->fields[f].name, fn) == 0) { v = rv->fields[f]; break; }
                    }
                    free(fn);
                } else {
                    v = rv->fields[c];
                }
                char* s = thrum_vstr_alloc(rt, v, false);
                csv_write_cell(&sb, s);
                free(s);
            }
            bsb_ch(&sb, '\n');
        }
    } else {
        if (hdrs) {
            for (size_t c = 0; c < hdrs->len; c++) {
                if (c > 0) bsb_ch(&sb, ',');
                char* s = thrum_vstr_alloc(rt, hdrs->items[c], false);
                csv_write_cell(&sb, s);
                free(s);
            }
            bsb_ch(&sb, '\n');
        }
        for (size_t r = 0; r < rows->len; r++) {
            if (rows->items[r].kind == TV_OBJ && rows->items[r].as.obj->kind == OBJ_LIST) {
                TList* rl = (TList*)rows->items[r].as.obj;
                for (size_t c = 0; c < rl->len; c++) {
                    if (c > 0) bsb_ch(&sb, ',');
                    char* s = thrum_vstr_alloc(rt, rl->items[c], false);
                    csv_write_cell(&sb, s);
                    free(s);
                }
            } else {
                char* s = thrum_vstr_alloc(rt, rows->items[r], false);
                csv_write_cell(&sb, s);
                free(s);
            }
            bsb_ch(&sb, '\n');
        }
    }
    TVal out = thrum_str(rt, sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return out;
}

static TEnv* build_mod_csv(TRuntime* rt) {
    TEnv* sc = thrum_env_new(rt, rt->builtins, false);
    mod_add(rt, sc, "csv", "parse", csv_parse, 1, 2);
    mod_add(rt, sc, "csv", "parse_nums", csv_parse_nums, 1, 2);
    mod_add(rt, sc, "csv", "stringify", csv_stringify, 1, 2);
    return sc;
}

/* ============================== std.time (Howard Hinnant Gregorian UTC) ============================== */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t* y, int* m, int* d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yr = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int day = (int)(doy - (153 * mp + 2) / 5 + 1);
    int mon = (int)(mp + (mp < 10 ? 3 : -9));
    yr += (mon <= 2);
    *y = yr;
    *m = mon;
    *d = day;
}

static double need_sec(TRuntime* rt, TVal v, int line, int col) {
    TQ q = thrum_need_q(rt, v, line, col);
    if (!thrum_dim_eq(q.d, thrum_dim(0, 0, 1, 0, 0, 0, 0))) {
        thrum_error(rt, line, col, "time function expects duration/timestamp in seconds (s)", NULL);
    }
    return q.v;
}

static TVal tm_parse_iso(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* s = need_str(rt, argv[0], "time.parse_iso", line, col);
    int yr = 0, mo = 1, da = 1, hr = 0, mi = 0;
    double sc = 0.0;
    const char* p = s->data;
    int off_sign = 0, off_h = 0, off_m = 0;
    if (sscanf(p, "%d-%d-%d", &yr, &mo, &da) != 3) {
        thrum_error(rt, line, col, "invalid ISO timestamp", NULL);
    }
    const char* tsep = strchr(p, 'T');
    if (!tsep) tsep = strchr(p, ' ');
    if (tsep) {
        char* after = NULL;
        if (sscanf(tsep + 1, "%d:%d", &hr, &mi) >= 2) {
            const char* c1 = strchr(tsep + 1, ':');
            const char* c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            if (c2) {
                sc = strtod(c2 + 1, &after);
                if (after && (*after == '+' || *after == '-')) {
                    off_sign = (*after == '+') ? 1 : -1;
                    sscanf(after + 1, "%d:%d", &off_h, &off_m);
                }
            }
        }
    }
    int64_t days = days_from_civil(yr, mo, da);
    double ts = (double)days * 86400.0 + (double)(hr * 3600 + mi * 60) + sc -
                (double)off_sign * (off_h * 3600.0 + off_m * 60.0);
    return thrum_qty(rt, ts, 0.0, thrum_dim(0, 0, 1, 0, 0, 0, 0), 1.0, "s");
}

static TVal tm_format_iso(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    double ts = need_sec(rt, argv[0], line, col);
    int64_t sec_i = (int64_t)floor(ts);
    int64_t days = sec_i >= 0 ? sec_i / 86400 : (sec_i - 86399) / 86400;
    int64_t rem = sec_i - days * 86400;
    int64_t y = 0; int m = 0, d = 0;
    civil_from_days(days, &y, &m, &d);
    int hr = (int)(rem / 3600);
    int mi = (int)((rem % 3600) / 60);
    int sc = (int)(rem % 60);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04lld-%02d-%02dT%02d:%02d:%02dZ", (long long)y, m, d, hr, mi, sc);
    return thrum_cstr(rt, buf);
}

static TVal tm_format_dur(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    double sec = need_sec(rt, argv[0], line, col);
    bool neg = sec < 0.0;
    double rem = fabs(sec);
    int64_t days = (int64_t)floor(rem / 86400.0); rem -= (double)days * 86400.0;
    int64_t hrs = (int64_t)floor(rem / 3600.0); rem -= (double)hrs * 3600.0;
    int64_t mins = (int64_t)floor(rem / 60.0); rem -= (double)mins * 60.0;
    BSB sb = {0};
    if (neg) bsb_ch(&sb, '-');
    bool started = false;
    char buf[64];
    if (days >= 1) { snprintf(buf, sizeof(buf), "%lldd", (long long)days); bsb_app(&sb, buf); started = true; }
    if (hrs >= 1 || started) { if (started) bsb_ch(&sb, ' '); snprintf(buf, sizeof(buf), "%lldh", (long long)hrs); bsb_app(&sb, buf); started = true; }
    if (mins >= 1 || started) { if (started) bsb_ch(&sb, ' '); snprintf(buf, sizeof(buf), "%lldm", (long long)mins); bsb_app(&sb, buf); started = true; }
    if (started) bsb_ch(&sb, ' ');
    if (rem == (double)(int64_t)rem) snprintf(buf, sizeof(buf), "%llds", (long long)(int64_t)rem);
    else snprintf(buf, sizeof(buf), "%.3gs", rem);
    bsb_app(&sb, buf);
    TVal out = thrum_cstr(rt, sb.data);
    free(sb.data);
    return out;
}

static TVal tm_components(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    double ts = need_sec(rt, argv[0], line, col);
    int64_t sec_i = (int64_t)floor(ts);
    double frac = ts - (double)sec_i;
    int64_t days = sec_i >= 0 ? sec_i / 86400 : (sec_i - 86399) / 86400;
    int64_t rem = sec_i - days * 86400;
    int64_t y = 0; int m = 0, d = 0;
    civil_from_days(days, &y, &m, &d);
    /* 1970-01-01 was Thursday (weekday 3 in Python Monday=0) */
    int wday = (int)((days + 3) % 7);
    if (wday < 0) wday += 7;
    T_FRAME(rt);
    T_VAR(rt, mp, thrum_map(rt));
    TMap* map = (TMap*)mp.as.obj;
    thrum_map_set(rt, map, thrum_cstr(rt, "year"), thrum_num_dimless((double)y));
    thrum_map_set(rt, map, thrum_cstr(rt, "month"), thrum_num_dimless((double)m));
    thrum_map_set(rt, map, thrum_cstr(rt, "day"), thrum_num_dimless((double)d));
    thrum_map_set(rt, map, thrum_cstr(rt, "hour"), thrum_num_dimless((double)(rem / 3600)));
    thrum_map_set(rt, map, thrum_cstr(rt, "min"), thrum_num_dimless((double)((rem % 3600) / 60)));
    thrum_map_set(rt, map, thrum_cstr(rt, "sec"), thrum_num_dimless((double)(rem % 60) + frac));
    thrum_map_set(rt, map, thrum_cstr(rt, "weekday"), thrum_num_dimless((double)wday));
    T_END_FRAME(rt);
    return mp;
}

static TVal tm_from_components(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    int64_t y = thrum_need_int(rt, argv[0], line, col);
    int64_t mo = thrum_need_int(rt, argv[1], line, col);
    int64_t da = thrum_need_int(rt, argv[2], line, col);
    int64_t hr = argc > 3 ? thrum_need_int(rt, argv[3], line, col) : 0;
    int64_t mi = argc > 4 ? thrum_need_int(rt, argv[4], line, col) : 0;
    int64_t sc = argc > 5 ? thrum_need_int(rt, argv[5], line, col) : 0;
    if (mo < 1 || mo > 12 || da < 1 || da > 31 || hr < 0 || hr > 23 || mi < 0 || mi > 59 || sc < 0 || sc > 59) {
        thrum_error(rt, line, col, "invalid date/time", NULL);
    }
    double ts = (double)days_from_civil(y, mo, da) * 86400.0 + (double)(hr * 3600 + mi * 60 + sc);
    return thrum_qty(rt, ts, 0.0, thrum_dim(0, 0, 1, 0, 0, 0, 0), 1.0, "s");
}

static TEnv* build_mod_time(TRuntime* rt) {
    TEnv* sc = thrum_env_new(rt, rt->builtins, false);
    mod_add(rt, sc, "time", "parse_iso", tm_parse_iso, 1, 1);
    mod_add(rt, sc, "time", "format_iso", tm_format_iso, 1, 1);
    mod_add(rt, sc, "time", "format_dur", tm_format_dur, 1, 1);
    mod_add(rt, sc, "time", "components", tm_components, 1, 1);
    mod_add(rt, sc, "time", "from_components", tm_from_components, 3, 6);
    return sc;
}

/* ============================== std.path ============================== */
static TVal pa_join(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    if (argc == 0) return thrum_cstr(rt, "");
    BSB sb = {0};
    for (int i = 0; i < argc; i++) {
        TStr* p = need_str(rt, argv[i], "path.join", line, col);
        if (p->len == 0) continue;
        if (p->data[0] == '/') {
            sb.len = 0;
            bsb_app_n(&sb, p->data, p->len);
        } else {
            if (sb.len > 0 && sb.data[sb.len - 1] != '/') bsb_ch(&sb, '/');
            bsb_app_n(&sb, p->data, p->len);
        }
    }
    TVal out = thrum_str(rt, sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return out;
}

static void split_path(const char* p, size_t len, const char** dir_out, size_t* dir_len, const char** base_out, size_t* base_len) {
    const char* sl = NULL;
    for (size_t i = len; i > 0; i--) {
        if (p[i - 1] == '/') { sl = p + i - 1; break; }
    }
    if (!sl) {
        *dir_out = p; *dir_len = 0;
        *base_out = p; *base_len = len;
    } else if (sl == p) {
        *dir_out = "/"; *dir_len = 1;
        *base_out = sl + 1; *base_len = len - 1;
    } else {
        size_t dl = (size_t)(sl - p);
        while (dl > 1 && p[dl - 1] == '/') dl--;
        *dir_out = p; *dir_len = dl;
        *base_out = sl + 1; *base_len = len - (size_t)(sl + 1 - p);
    }
}

static TVal pa_dirname(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* p = need_str(rt, argv[0], "path.dirname", line, col);
    const char* d; const char* b; size_t dl, bl;
    split_path(p->data, p->len, &d, &dl, &b, &bl);
    return thrum_str(rt, d, dl);
}

static TVal pa_basename(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* p = need_str(rt, argv[0], "path.basename", line, col);
    const char* d; const char* b; size_t dl, bl;
    split_path(p->data, p->len, &d, &dl, &b, &bl);
    return thrum_str(rt, b, bl);
}

static void split_ext(const char* s, size_t len, size_t* dot_pos) {
    *dot_pos = len;
    for (size_t i = len; i > 0; i--) {
        if (s[i - 1] == '/') break;
        if (s[i - 1] == '.') {
            if (i > 1 && s[i - 2] != '/') *dot_pos = i - 1;
            break;
        }
    }
}

static TVal pa_stem(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* p = need_str(rt, argv[0], "path.stem", line, col);
    const char* d; const char* b; size_t dl, bl, dp;
    split_path(p->data, p->len, &d, &dl, &b, &bl);
    split_ext(b, bl, &dp);
    return thrum_str(rt, b, dp);
}

static TVal pa_ext(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* p = need_str(rt, argv[0], "path.ext", line, col);
    size_t dp;
    split_ext(p->data, p->len, &dp);
    return thrum_str(rt, p->data + dp, p->len - dp);
}

static TVal pa_normalize(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TStr* p = need_str(rt, argv[0], "path.normalize", line, col);
    if (p->len == 0) return thrum_cstr(rt, ".");
    bool abs_p = p->data[0] == '/';
    char* copy = strdup(p->data);
    char** segs = (char**)malloc(256 * sizeof(char*));
    int n = 0, cap = 256;
    char* save = NULL;
    char* tok = strtok_r(copy, "/", &save);
    while (tok) {
        if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
            /* skip */
        } else if (strcmp(tok, "..") == 0) {
            if (n > 0 && strcmp(segs[n - 1], "..") != 0) n--;
            else if (!abs_p) {
                if (n >= cap) { cap *= 2; segs = (char**)realloc(segs, (size_t)cap * sizeof(char*)); }
                segs[n++] = tok;
            }
        } else {
            if (n >= cap) { cap *= 2; segs = (char**)realloc(segs, (size_t)cap * sizeof(char*)); }
            segs[n++] = tok;
        }
        tok = strtok_r(NULL, "/", &save);
    }
    BSB sb = {0};
    if (abs_p) bsb_ch(&sb, '/');
    for (int i = 0; i < n; i++) {
        if (i > 0) bsb_ch(&sb, '/');
        bsb_app(&sb, segs[i]);
    }
    if (sb.len == 0) bsb_app(&sb, ".");
    free(segs);
    free(copy);
    TVal out = thrum_str(rt, sb.data, sb.len);
    free(sb.data);
    return out;
}

static TVal pa_split(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, pv, argv[0]);
    TStr* p = need_str(rt, pv, "path.split", line, col);
    const char* d; const char* b; size_t dl, bl;
    split_path(p->data, p->len, &d, &dl, &b, &bl);
    T_VAR(rt, out, thrum_list(rt, 2));
    thrum_list_push(rt, (TList*)out.as.obj, thrum_str(rt, d, dl));
    thrum_list_push(rt, (TList*)out.as.obj, thrum_str(rt, b, bl));
    T_END_FRAME(rt);
    return out;
}

static TEnv* build_mod_path(TRuntime* rt) {
    TEnv* sc = thrum_env_new(rt, rt->builtins, false);
    mod_add(rt, sc, "path", "join", pa_join, 0, -1);
    mod_add(rt, sc, "path", "dirname", pa_dirname, 1, 1);
    mod_add(rt, sc, "path", "basename", pa_basename, 1, 1);
    mod_add(rt, sc, "path", "stem", pa_stem, 1, 1);
    mod_add(rt, sc, "path", "ext", pa_ext, 1, 1);
    mod_add(rt, sc, "path", "normalize", pa_normalize, 1, 1);
    mod_add(rt, sc, "path", "split", pa_split, 1, 1);
    return sc;
}

/* ============================== std.list ============================== */
static TVal ls_chunk(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    TList* xs = need_list(rt, lv, "list.chunk", line, col);
    int64_t n = thrum_need_int(rt, argv[1], line, col);
    if (n <= 0) thrum_error(rt, line, col, "chunk size must be > 0", NULL);
    T_VAR(rt, out, thrum_list(rt, (xs->len + (size_t)n - 1) / (size_t)n));
    TList* ol = (TList*)out.as.obj;
    for (size_t i = 0; i < xs->len; i += (size_t)n) {
        size_t end = i + (size_t)n < xs->len ? i + (size_t)n : xs->len;
        T_VAR(rt, c, thrum_list(rt, end - i));
        for (size_t j = i; j < end; j++) thrum_list_push(rt, (TList*)c.as.obj, xs->items[j]);
        thrum_list_push(rt, ol, c);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal ls_window(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    TList* xs = need_list(rt, lv, "list.window", line, col);
    int64_t n = thrum_need_int(rt, argv[1], line, col);
    if (n <= 0) thrum_error(rt, line, col, "window size must be > 0", NULL);
    T_VAR(rt, out, thrum_list(rt, xs->len >= (size_t)n ? xs->len - (size_t)n + 1 : 0));
    TList* ol = (TList*)out.as.obj;
    if ((size_t)n <= xs->len) {
        for (size_t i = 0; i <= xs->len - (size_t)n; i++) {
            T_VAR(rt, w, thrum_list(rt, (size_t)n));
            for (size_t j = 0; j < (size_t)n; j++) thrum_list_push(rt, (TList*)w.as.obj, xs->items[i + j]);
            thrum_list_push(rt, ol, w);
        }
    }
    T_END_FRAME(rt);
    return out;
}

static TVal ls_take(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    TList* xs = need_list(rt, lv, "list.take", line, col);
    int64_t n = thrum_need_int(rt, argv[1], line, col);
    if (n < 0) n = 0;
    size_t k = (size_t)n < xs->len ? (size_t)n : xs->len;
    T_VAR(rt, out, thrum_list(rt, k));
    for (size_t i = 0; i < k; i++) thrum_list_push(rt, (TList*)out.as.obj, xs->items[i]);
    T_END_FRAME(rt);
    return out;
}

static TVal ls_drop(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    TList* xs = need_list(rt, lv, "list.drop", line, col);
    int64_t n = thrum_need_int(rt, argv[1], line, col);
    if (n < 0) n = 0;
    size_t st = (size_t)n < xs->len ? (size_t)n : xs->len;
    T_VAR(rt, out, thrum_list(rt, xs->len - st));
    for (size_t i = st; i < xs->len; i++) thrum_list_push(rt, (TList*)out.as.obj, xs->items[i]);
    T_END_FRAME(rt);
    return out;
}

static TVal ls_partition(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    T_VAR(rt, fv, argv[1]);
    TList* xs = need_list(rt, lv, "list.partition", line, col);
    T_VAR(rt, yes, thrum_list(rt, xs->len));
    T_VAR(rt, no, thrum_list(rt, xs->len));
    for (size_t i = 0; i < xs->len; i++) {
        if (thrum_truthy(thrum_call(rt, fv, 1, &xs->items[i], line, col))) thrum_list_push(rt, (TList*)yes.as.obj, xs->items[i]);
        else thrum_list_push(rt, (TList*)no.as.obj, xs->items[i]);
    }
    T_VAR(rt, out, thrum_list(rt, 2));
    thrum_list_push(rt, (TList*)out.as.obj, yes);
    thrum_list_push(rt, (TList*)out.as.obj, no);
    T_END_FRAME(rt);
    return out;
}

static TVal ls_group_by(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    T_VAR(rt, fv, argv[1]);
    TList* xs = need_list(rt, lv, "list.group_by", line, col);
    T_VAR(rt, mp, thrum_map(rt));
    TMap* m = (TMap*)mp.as.obj;
    for (size_t i = 0; i < xs->len; i++) {
        T_VAR(rt, k, thrum_call(rt, fv, 1, &xs->items[i], line, col));
        if (k.kind == TV_OBJ && (k.as.obj->kind == OBJ_LIST || k.as.obj->kind == OBJ_MAP)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "group_by key cannot be %s", thrum_tname(k));
            thrum_error(rt, line, col, msg, NULL);
        }
        TVal bucket;
        if (!thrum_map_get(m, k, &bucket)) {
            bucket = thrum_list(rt, 4);
            thrum_map_set(rt, m, k, bucket);
        }
        thrum_list_push(rt, (TList*)bucket.as.obj, xs->items[i]);
    }
    T_END_FRAME(rt);
    return mp;
}

static TVal ls_sort_by(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    return bi_sort(rt, argc, argv, line, col);
}

static TVal ls_flat_map(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, lv, argv[0]);
    T_VAR(rt, fv, argv[1]);
    TList* xs = need_list(rt, lv, "list.flat_map", line, col);
    T_VAR(rt, out, thrum_list(rt, xs->len));
    TList* ol = (TList*)out.as.obj;
    for (size_t i = 0; i < xs->len; i++) {
        T_VAR(rt, r, thrum_call(rt, fv, 1, &xs->items[i], line, col));
        if (r.kind == TV_OBJ && r.as.obj->kind == OBJ_LIST) {
            TList* sub = (TList*)r.as.obj;
            for (size_t j = 0; j < sub->len; j++) thrum_list_push(rt, ol, sub->items[j]);
        } else {
            thrum_list_push(rt, ol, r);
        }
    }
    T_END_FRAME(rt);
    return out;
}

static TEnv* build_mod_list(TRuntime* rt) {
    TEnv* sc = thrum_env_new(rt, rt->builtins, false);
    mod_add(rt, sc, "list", "chunk", ls_chunk, 2, 2);
    mod_add(rt, sc, "list", "window", ls_window, 2, 2);
    mod_add(rt, sc, "list", "take", ls_take, 2, 2);
    mod_add(rt, sc, "list", "drop", ls_drop, 2, 2);
    mod_add(rt, sc, "list", "partition", ls_partition, 2, 2);
    mod_add(rt, sc, "list", "group_by", ls_group_by, 2, 2);
    mod_add(rt, sc, "list", "sort_by", ls_sort_by, 2, 2);
    mod_add(rt, sc, "list", "flat_map", ls_flat_map, 2, 2);
    mod_alias_builtin(rt, sc, "zip", "zip", 0, 0);
    mod_alias_builtin(rt, sc, "enumerate", "enumerate", 0, 0);
    mod_alias_builtin(rt, sc, "flatten", "flatten", 0, 0);
    mod_alias_builtin(rt, sc, "uniq", "uniq", 0, 0);
    mod_alias_builtin(rt, sc, "reverse", "reverse", 0, 0);
    mod_alias_builtin(rt, sc, "slice", "slice", 0, 0);
    mod_alias_builtin(rt, sc, "range", "range", 0, 0);
    return sc;
}

/* ============================== std.map ============================== */
static TVal mp_get(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    TMap* m = need_map(rt, argv[0], "map.get", line, col);
    if (argv[1].kind == TV_OBJ && (argv[1].as.obj->kind == OBJ_LIST || argv[1].as.obj->kind == OBJ_MAP)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "map key cannot be %s", thrum_tname(argv[1]));
        thrum_error(rt, line, col, msg, NULL);
    }
    TVal out = (argc > 2) ? argv[2] : thrum_nil();
    thrum_map_get(m, argv[1], &out);
    return out;
}

static TVal mp_has(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TMap* m = need_map(rt, argv[0], "map.has", line, col);
    if (argv[1].kind == TV_OBJ && (argv[1].as.obj->kind == OBJ_LIST || argv[1].as.obj->kind == OBJ_MAP)) return thrum_bool(false);
    return thrum_bool(thrum_map_has(m, argv[1]));
}

static TVal mp_merge(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, av, argv[0]);
    T_VAR(rt, bv, argv[1]);
    TMap* a = need_map(rt, av, "map.merge", line, col);
    TMap* b = need_map(rt, bv, "map.merge", line, col);
    T_VAR(rt, out, thrum_map(rt));
    TMap* nm = (TMap*)out.as.obj;
    for (size_t i = 0; i < a->order_len; i++) { TVal v; if (thrum_map_get(a, a->order_keys[i], &v)) thrum_map_set(rt, nm, a->order_keys[i], v); }
    for (size_t i = 0; i < b->order_len; i++) { TVal v; if (thrum_map_get(b, b->order_keys[i], &v)) thrum_map_set(rt, nm, b->order_keys[i], v); }
    T_END_FRAME(rt);
    return out;
}

static TVal mp_pick(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, mv, argv[0]);
    T_VAR(rt, ksv, argv[1]);
    TMap* m = need_map(rt, mv, "map.pick", line, col);
    TList* ks = need_list(rt, ksv, "map.pick", line, col);
    T_VAR(rt, out, thrum_map(rt));
    TMap* nm = (TMap*)out.as.obj;
    for (size_t i = 0; i < ks->len; i++) {
        TVal k = ks->items[i], v;
        if (k.kind == TV_OBJ && (k.as.obj->kind == OBJ_LIST || k.as.obj->kind == OBJ_MAP)) continue;
        if (thrum_map_get(m, k, &v)) thrum_map_set(rt, nm, k, v);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal mp_omit(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, mv, argv[0]);
    T_VAR(rt, ksv, argv[1]);
    TMap* m = need_map(rt, mv, "map.omit", line, col);
    TList* ks = need_list(rt, ksv, "map.omit", line, col);
    T_VAR(rt, out, thrum_map(rt));
    TMap* nm = (TMap*)out.as.obj;
    for (size_t i = 0; i < m->order_len; i++) {
        TVal k = m->order_keys[i], v;
        if (!thrum_map_get(m, k, &v)) continue;
        bool drop = false;
        for (size_t j = 0; j < ks->len; j++) if (thrum_veq(k, ks->items[j])) { drop = true; break; }
        if (!drop) thrum_map_set(rt, nm, k, v);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal mp_map_vals(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, mv, argv[0]);
    T_VAR(rt, fv, argv[1]);
    TMap* m = need_map(rt, mv, "map.map_vals", line, col);
    T_VAR(rt, out, thrum_map(rt));
    TMap* nm = (TMap*)out.as.obj;
    for (size_t i = 0; i < m->order_len; i++) {
        TVal k = m->order_keys[i], v;
        if (!thrum_map_get(m, k, &v)) continue;
        T_VAR(rt, nv, thrum_call(rt, fv, 1, &v, line, col));
        thrum_map_set(rt, nm, k, nv);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal mp_filter_entries(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, mv, argv[0]);
    T_VAR(rt, fv, argv[1]);
    TMap* m = need_map(rt, mv, "map.filter_entries", line, col);
    T_VAR(rt, out, thrum_map(rt));
    TMap* nm = (TMap*)out.as.obj;
    for (size_t i = 0; i < m->order_len; i++) {
        TVal k = m->order_keys[i], v;
        if (!thrum_map_get(m, k, &v)) continue;
        TVal args[2] = {k, v};
        if (thrum_truthy(thrum_call(rt, fv, 2, args, line, col))) thrum_map_set(rt, nm, k, v);
    }
    T_END_FRAME(rt);
    return out;
}

static TVal mp_from_entries(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    (void)argc;
    T_FRAME(rt);
    T_VAR(rt, pv, argv[0]);
    TList* pairs = need_list(rt, pv, "map.from_entries", line, col);
    T_VAR(rt, out, thrum_map(rt));
    TMap* nm = (TMap*)out.as.obj;
    for (size_t i = 0; i < pairs->len; i++) {
        if (pairs->items[i].kind != TV_OBJ || pairs->items[i].as.obj->kind != OBJ_LIST ||
            ((TList*)pairs->items[i].as.obj)->len != 2) {
            thrum_error(rt, line, col, "from_entries needs [key, val] pairs", NULL);
        }
        TList* pr = (TList*)pairs->items[i].as.obj;
        if (pr->items[0].kind == TV_OBJ && (pr->items[0].as.obj->kind == OBJ_LIST || pr->items[0].as.obj->kind == OBJ_MAP)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "map key cannot be %s", thrum_tname(pr->items[0]));
            thrum_error(rt, line, col, msg, NULL);
        }
        thrum_map_set(rt, nm, pr->items[0], pr->items[1]);
    }
    T_END_FRAME(rt);
    return out;
}

static TEnv* build_mod_map(TRuntime* rt) {
    TEnv* sc = thrum_env_new(rt, rt->builtins, false);
    mod_add(rt, sc, "map", "get", mp_get, 2, 3);
    mod_add(rt, sc, "map", "has", mp_has, 2, 2);
    mod_add(rt, sc, "map", "merge", mp_merge, 2, 2);
    mod_add(rt, sc, "map", "pick", mp_pick, 2, 2);
    mod_add(rt, sc, "map", "omit", mp_omit, 2, 2);
    mod_add(rt, sc, "map", "map_vals", mp_map_vals, 2, 2);
    mod_add(rt, sc, "map", "filter_entries", mp_filter_entries, 2, 2);
    mod_add(rt, sc, "map", "from_entries", mp_from_entries, 1, 1);
    mod_alias_builtin(rt, sc, "keys", "keys", 0, 0);
    mod_alias_builtin(rt, sc, "vals", "vals", 0, 0);
    mod_alias_builtin(rt, sc, "items", "items", 0, 0);
    return sc;
}

/* ============================== std.test ============================== */
typedef struct {
    int passed;
    int failed;
    TEnv* scope; /* pin _failures in scope so GC keeps failure list alive */
} TTestState;

static TVal ts_eq(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    if (!thrum_veq(argv[0], argv[1])) {
        char* s0 = thrum_vstr_alloc(rt, argv[0], true);
        char* s1 = thrum_vstr_alloc(rt, argv[1], true);
        char* msg = (argc > 2) ? thrum_vstr_alloc(rt, argv[2], false) : NULL;
        BSB sb = {0};
        bsb_app(&sb, "test assertion failed: ");
        if (msg && msg[0]) { bsb_app(&sb, msg); bsb_app(&sb, " (expected "); }
        else bsb_app(&sb, "expected ");
        bsb_app(&sb, s0);
        bsb_app(&sb, " == ");
        bsb_app(&sb, s1);
        if (msg && msg[0]) bsb_ch(&sb, ')');
        free(s0); free(s1); free(msg);
        char err[512];
        snprintf(err, sizeof(err), "%s", sb.data);
        free(sb.data);
        thrum_error(rt, line, col, err, NULL);
    }
    return thrum_bool(true);
}

static TVal ts_neq(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    if (thrum_veq(argv[0], argv[1])) {
        char* s0 = thrum_vstr_alloc(rt, argv[0], true);
        char* s1 = thrum_vstr_alloc(rt, argv[1], true);
        char* msg = (argc > 2) ? thrum_vstr_alloc(rt, argv[2], false) : NULL;
        BSB sb = {0};
        bsb_app(&sb, "test assertion failed: ");
        if (msg && msg[0]) { bsb_app(&sb, msg); bsb_app(&sb, " (expected "); }
        else bsb_app(&sb, "expected ");
        bsb_app(&sb, s0);
        bsb_app(&sb, " != ");
        bsb_app(&sb, s1);
        if (msg && msg[0]) bsb_ch(&sb, ')');
        free(s0); free(s1); free(msg);
        char err[512];
        snprintf(err, sizeof(err), "%s", sb.data);
        free(sb.data);
        thrum_error(rt, line, col, err, NULL);
    }
    return thrum_bool(true);
}

static TVal ts_approx(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    thrum_need_q(rt, argv[0], line, col);
    thrum_need_q(rt, argv[1], line, col);
    double k = (argc > 2) ? thrum_need_q(rt, argv[2], line, col).v : 3.0;
    if (!thrum_approx(argv[0], argv[1], k)) {
        char* s0 = thrum_vstr_alloc(rt, argv[0], false);
        char* s1 = thrum_vstr_alloc(rt, argv[1], false);
        char err[256];
        snprintf(err, sizeof(err), "test approx failed: %s ~ %s", s0, s1);
        free(s0); free(s1);
        thrum_error(rt, line, col, err, NULL);
    }
    return thrum_bool(true);
}

static TVal ts_ok(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    if (!thrum_truthy(argv[0])) {
        char* msg;
        char* ms = (argc > 1) ? thrum_vstr_alloc(rt, argv[1], false) : NULL;
        if (ms) msg = ms;
        else {
            char* s0 = thrum_vstr_alloc(rt, argv[0], true);
            BSB sb = {0};
            bsb_app(&sb, "expected truthy value, got ");
            bsb_app(&sb, s0);
            free(s0);
            msg = sb.data;
        }
        char err[256];
        snprintf(err, sizeof(err), "test assertion failed: %s", msg);
        free(msg);
        thrum_error(rt, line, col, err, NULL);
    }
    return thrum_bool(true);
}

static TVal ts_fails(TRuntime* rt, int argc, TVal* argv, int line, int col) {
    const char* substr = (argc > 1) ? need_str(rt, argv[1], "test.fails", line, col)->data : NULL;
    T_FRAME(rt);
    T_VAR(rt, fnv, argv[0]);
    T_VAR(rt, subv, argc > 1 ? argv[1] : thrum_nil());
    int tf = thrum_push_try(rt);
    if (setjmp(rt->try_stack[tf].buf) == 0) {
        thrum_call(rt, fnv, 0, NULL, line, col);
        thrum_pop_try(rt);
        T_END_FRAME(rt);
        thrum_error(rt, line, col, "test.fails expected function to fail, but it succeeded", NULL);
    }
    const char* sub = (subv.kind == TV_OBJ && subv.as.obj->kind == OBJ_STR) ? ((TStr*)subv.as.obj)->data : substr;
    if (sub && (!rt->err_msg || !strstr(rt->err_msg, sub))) {
        char msg[256];
        snprintf(msg, sizeof(msg), "test.fails expected error containing '%s', got '%s'", sub, rt->err_msg ? rt->err_msg : "");
        T_END_FRAME(rt);
        thrum_error(rt, line, col, msg, NULL);
    }
    T_END_FRAME(rt);
    return thrum_bool(true);
}

static TVal ts_run(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TTestState* st = (TTestState*)ctx;
    TStr* name = need_str(rt, argv[0], "test.run", line, col);
    T_FRAME(rt);
    T_VAR(rt, nmv, argv[0]);
    T_VAR(rt, fnv, argv[1]);
    int tf = thrum_push_try(rt);
    if (setjmp(rt->try_stack[tf].buf) == 0) {
        thrum_call(rt, fnv, 0, NULL, line, col);
        thrum_pop_try(rt);
        st->passed++;
        T_END_FRAME(rt);
        return thrum_bool(true);
    }
    (void)name;
    st->failed++;
    TVal fv = thrum_read(rt, st->scope, "__failures", 0, 0);
    TList* fl = (TList*)fv.as.obj;
    T_VAR(rt, ent, thrum_map(rt));
    TMap* em = (TMap*)ent.as.obj;
    thrum_map_set_cs(rt, em, "name", nmv);
    thrum_map_set_cs(rt, em, "error", thrum_cstr(rt, rt->err_msg ? rt->err_msg : ""));
    thrum_list_push(rt, fl, ent);
    T_END_FRAME(rt);
    return thrum_bool(false);
}

static TVal ts_summary(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)argv; (void)line; (void)col;
    TTestState* st = (TTestState*)ctx;
    T_FRAME(rt);
    T_VAR(rt, mp, thrum_map(rt));
    TMap* m = (TMap*)mp.as.obj;
    thrum_map_set_cs(rt, m, "passed", thrum_num_dimless((double)st->passed));
    thrum_map_set_cs(rt, m, "failed", thrum_num_dimless((double)st->failed));
    thrum_map_set_cs(rt, m, "total", thrum_num_dimless((double)(st->passed + st->failed)));
    TVal fv = thrum_read(rt, st->scope, "__failures", 0, 0);
    TList* fl = (TList*)fv.as.obj;
    T_VAR(rt, fcopy, thrum_list(rt, fl->len));
    for (size_t i = 0; i < fl->len; i++) thrum_list_push(rt, (TList*)fcopy.as.obj, fl->items[i]);
    thrum_map_set_cs(rt, m, "failures", fcopy);
    T_END_FRAME(rt);
    return mp;
}

static TEnv* build_mod_test(TRuntime* rt) {
    TEnv* sc = thrum_env_new(rt, rt->builtins, false);
    T_FRAME(rt);
    T_VAR(rt, scv, thrum_obj((TObj*)sc));
    TTestState* st = (TTestState*)calloc(1, sizeof(TTestState));
    st->scope = sc;
    thrum_def_pin(rt, sc, "__failures", thrum_list(rt, 4));
    mod_add(rt, sc, "test", "eq", ts_eq, 2, 3);
    mod_add(rt, sc, "test", "neq", ts_neq, 2, 3);
    mod_add(rt, sc, "test", "approx", ts_approx, 2, 3);
    mod_add(rt, sc, "test", "ok", ts_ok, 1, 2);
    mod_add(rt, sc, "test", "fails", ts_fails, 1, 2);
    T_VAR(rt, rb, thrum_make_builtin_ctx(rt, "test.run", ts_run, st, (TObj*)sc, 2, 2));
    thrum_def_pin(rt, sc, "run", rb);
    T_VAR(rt, sb, thrum_make_builtin_ctx(rt, "test.summary", ts_summary, st, (TObj*)sc, 0, 0));
    thrum_def_pin(rt, sc, "summary", sb);
    T_END_FRAME(rt);
    return sc;
}

/* ============================== Dispatch ============================== */
TVal thrum_get_stdmod(TRuntime* rt, const char* name, int line, int col) {
    char key[96];
    snprintf(key, sizeof(key), "std.%s", name);
    TVal existing = thrum_get_module(rt, key);
    if (existing.kind != TV_NIL) return existing;
    TEnv* sc = NULL;
    if (strcmp(name, "math") == 0) sc = build_mod_math(rt);
    else if (strcmp(name, "const") == 0) sc = build_mod_const(rt);
    else if (strcmp(name, "stat") == 0) sc = build_mod_stat(rt);
    else if (strcmp(name, "units") == 0) sc = build_mod_units(rt);
    else if (strcmp(name, "text") == 0) sc = build_mod_text(rt);
    else if (strcmp(name, "regex") == 0) sc = build_mod_regex(rt);
    else if (strcmp(name, "json") == 0) sc = build_mod_json(rt);
    else if (strcmp(name, "csv") == 0) sc = build_mod_csv(rt);
    else if (strcmp(name, "time") == 0) sc = build_mod_time(rt);
    else if (strcmp(name, "path") == 0) sc = build_mod_path(rt);
    else if (strcmp(name, "list") == 0) sc = build_mod_list(rt);
    else if (strcmp(name, "map") == 0) sc = build_mod_map(rt);
    else if (strcmp(name, "test") == 0) sc = build_mod_test(rt);
    else {
        char msg[160];
        snprintf(msg, sizeof(msg), "unknown module '%s'", name);
        thrum_error(rt, line, col, msg, NULL);
    }
    thrum_reg_module(rt, key, sc);
    return thrum_get_module(rt, key);
}
