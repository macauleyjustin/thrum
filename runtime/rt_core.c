#include "thrum_rt.h"
#include <ctype.h>

/* ============================== GC & Allocation ============================== */
static void mark_val(TVal v);

static void mark_obj(TObj* o) {
    if (!o || o->marked) return;
    o->marked = 1;
    switch (o->kind) {
        case OBJ_STR:
            break;
        case OBJ_BUILTIN:
            mark_obj(((TBuiltin*)o)->owner);
            break;
        case OBJ_LIST: {
            TList* l = (TList*)o;
            for (size_t i = 0; i < l->len; i++) mark_val(l->items[i]);
            break;
        }
        case OBJ_MAP: {
            TMap* m = (TMap*)o;
            for (size_t i = 0; i < m->cap; i++) {
                if (m->entries[i].used && !m->entries[i].tomb) {
                    mark_val(m->entries[i].key);
                    mark_val(m->entries[i].val);
                }
            }
            for (size_t i = 0; i < m->order_len; i++) mark_val(m->order_keys[i]);
            break;
        }
        case OBJ_RECTYPE: {
            TRecType* rt = (TRecType*)o;
            mark_obj((TObj*)rt->env);
            break;
        }
        case OBJ_RECVAL: {
            TRecVal* rv = (TRecVal*)o;
            mark_obj((TObj*)rv->rtype);
            for (int i = 0; i < rv->rtype->nfields; i++) mark_val(rv->fields[i]);
            break;
        }
        case OBJ_FN: {
            TFn* f = (TFn*)o;
            mark_obj((TObj*)f->env);
            break;
        }
        case OBJ_MOD: {
            TMod* m = (TMod*)o;
            mark_obj((TObj*)m->scope);
            break;
        }
        case OBJ_CAP: {
            TCap* c = (TCap*)o;
            mark_obj((TObj*)c->methods);
            break;
        }
        case OBJ_ENV: {
            TEnv* e = (TEnv*)o;
            mark_obj((TObj*)e->parent);
            for (size_t i = 0; i < e->len; i++) mark_val(e->slots[i].val);
            break;
        }
        case OBJ_CELL: {
            TCell* c = (TCell*)o;
            mark_val(c->value);
            mark_obj((TObj*)c->env);
            mark_obj((TObj*)c->group);
            for (int i = 0; i < c->ndeps; i++) mark_obj((TObj*)c->deps[i]);
            for (int i = 0; i < c->nsubs; i++) mark_obj((TObj*)c->subs[i]);
            for (int i = 0; i < c->nnewdeps; i++) mark_obj((TObj*)c->newdeps[i]);
            break;
        }
        case OBJ_GROUP: {
            TGroup* g = (TGroup*)o;
            for (int i = 0; i < g->ncells; i++) mark_obj((TObj*)g->cells[i]);
            break;
        }
    }
}

static void mark_val(TVal v) {
    if (v.kind == TV_OBJ) mark_obj(v.as.obj);
}

static void free_obj(TObj* o) {
    switch (o->kind) {
        case OBJ_LIST: free(((TList*)o)->items); break;
        case OBJ_MAP: free(((TMap*)o)->entries); free(((TMap*)o)->order_keys); break;
        case OBJ_RECTYPE: free(((TRecType*)o)->fields); break;
        case OBJ_RECVAL: free(((TRecVal*)o)->fields); break;
        case OBJ_FN:
            free(((TFn*)o)->params);
            free(((TFn*)o)->reqs);
            free(((TFn*)o)->ens);
            break;
        case OBJ_ENV: free(((TEnv*)o)->slots); break;
        case OBJ_CELL:
            free(((TCell*)o)->deps);
            free(((TCell*)o)->subs);
            free(((TCell*)o)->newdeps);
            break;
        case OBJ_GROUP: free(((TGroup*)o)->cells); break;
        case OBJ_CAP: {
            TCap* c = (TCap*)o;
            if (c->free_state && c->state) c->free_state(c->state);
            break;
        }
        default: break;
    }
    free(o);
}

void thrum_gc(TRuntime* rt) {
    if (!rt) return;
    mark_obj((TObj*)rt->builtins);
    mark_obj((TObj*)rt->globals);
    mark_obj((TObj*)rt->modules);
    for (int i = 0; i < rt->root_top; i++) mark_val(*rt->root_ptrs[i]);
    for (int i = 0; i < rt->track_top; i++) mark_obj((TObj*)rt->track[i]);
    for (int i = 0; i < rt->n_overrides; i++) {
        mark_obj((TObj*)rt->overrides[i].cell);
        mark_val(rt->overrides[i].val);
    }
    for (int i = 0; i < rt->n_reactions; i++) {
        mark_obj((TObj*)rt->reactions[i].guard);
        mark_obj((TObj*)rt->reactions[i].env);
    }
    for (int i = 0; i < rt->n_groups; i++) mark_obj((TObj*)rt->groups[i]);
    for (int i = 0; i < rt->n_queue; i++) mark_obj((TObj*)rt->queue[i]);
    mark_obj((TObj*)rt->curgroup);
    for (int i = 0; i < rt->cleanup_top; i++) mark_obj((TObj*)rt->cleanups[i].cap);
    for (int i = 0; i < rt->jlen; i++) mark_val(rt->jevents[i].value);

    TObj** cur = &rt->all_objs;
    while (*cur) {
        TObj* obj = *cur;
        if (!obj->marked) {
            *cur = obj->next;
            rt->num_objs--;
            free_obj(obj);
        } else {
            obj->marked = 0;
            cur = &obj->next;
        }
    }
    rt->gc_threshold = rt->num_objs * 2 + 1024;
}

TObj* thrum_alloc_obj(TRuntime* rt, TObjKind kind, size_t size) {
    if (rt->num_objs >= rt->gc_threshold) thrum_gc(rt);
    TObj* o = (TObj*)calloc(1, size);
    o->kind = kind;
    o->marked = 0;
    o->next = rt->all_objs;
    rt->all_objs = o;
    rt->num_objs++;
    return o;
}

static inline TObj* alloc_obj(TRuntime* rt, TObjKind kind, size_t size) {
    return thrum_alloc_obj(rt, kind, size);
}

void thrum_push_root(TRuntime* rt, TVal* slot) {
    if (rt->root_top >= rt->root_cap) {
        rt->root_cap = rt->root_cap ? rt->root_cap * 2 : 1024;
        rt->root_ptrs = (TVal**)realloc(rt->root_ptrs, (size_t)rt->root_cap * sizeof(TVal*));
    }
    rt->root_ptrs[rt->root_top++] = slot;
}

void thrum_pop_roots(TRuntime* rt, int target_top) {
    rt->root_top = target_top;
}

static uint64_t fnv1a_hash(const char* s, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

const char* thrum_intern(TRuntime* rt, const char* s) {
    if (!rt || !s) return s;
    size_t slen = strlen(s);
    uint64_t h = fnv1a_hash(s, slen);
    if (rt->intern_bcap == 0 || (size_t)(rt->n_intern + 1) * 2 > rt->intern_bcap) {
        size_t nbcap = rt->intern_bcap ? rt->intern_bcap * 2 : 512;
        uint32_t* nb = (uint32_t*)calloc(nbcap, sizeof(uint32_t));
        size_t mask = nbcap - 1;
        for (int i = 0; i < rt->n_intern; i++) {
            const char* ex = rt->intern_strs[i];
            size_t slot = (size_t)fnv1a_hash(ex, strlen(ex)) & mask;
            while (nb[slot] != 0) slot = (slot + 1) & mask;
            nb[slot] = (uint32_t)(i + 1);
        }
        free(rt->intern_buckets);
        rt->intern_buckets = nb;
        rt->intern_bcap = nbcap;
    }
    size_t mask = rt->intern_bcap - 1;
    size_t slot = (size_t)h & mask;
    while (rt->intern_buckets[slot] != 0) {
        int idx = (int)rt->intern_buckets[slot] - 1;
        if (strcmp(rt->intern_strs[idx], s) == 0) return rt->intern_strs[idx];
        slot = (slot + 1) & mask;
    }
    if (rt->n_intern >= rt->cap_intern) {
        rt->cap_intern = rt->cap_intern ? rt->cap_intern * 2 : 256;
        rt->intern_strs = (char**)realloc(rt->intern_strs, (size_t)rt->cap_intern * sizeof(char*));
    }
    char* copy = (char*)malloc(slen + 1);
    memcpy(copy, s, slen + 1);
    int nidx = rt->n_intern++;
    rt->intern_strs[nidx] = copy;
    rt->intern_buckets[slot] = (uint32_t)(nidx + 1);
    return copy;
}

/* ============================== Errors & Try/Catch ============================== */
void thrum_push_cleanup(TRuntime* rt, TCap* cap) {
    if (rt->cleanup_top >= rt->cleanup_cap) {
        rt->cleanup_cap = rt->cleanup_cap ? rt->cleanup_cap * 2 : 32;
        rt->cleanups = (TCleanupEntry*)realloc(rt->cleanups, (size_t)rt->cleanup_cap * sizeof(TCleanupEntry));
    }
    rt->cleanups[rt->cleanup_top++].cap = cap;
}

void thrum_unwind_cleanups(TRuntime* rt, int target_top) {
    while (rt->cleanup_top > target_top) {
        TCap* cap = rt->cleanups[--rt->cleanup_top].cap;
        if (cap && cap->cleanup) cap->cleanup(rt, cap->state);
    }
}

int thrum_push_try(TRuntime* rt) {
    if (rt->try_top >= rt->try_cap) {
        rt->try_cap = rt->try_cap ? rt->try_cap * 2 : 32;
        rt->try_stack = (TTryFrame*)realloc(rt->try_stack, (size_t)rt->try_cap * sizeof(TTryFrame));
    }
    int idx = rt->try_top++;
    TTryFrame* f = &rt->try_stack[idx];
    f->saved_root_top = rt->root_top;
    f->saved_track_top = rt->track_top;
    f->saved_frames_top = rt->frames_top;
    f->saved_cleanup_top = rt->cleanup_top;
    f->saved_depth = rt->call_depth;
    f->saved_draining = rt->draining;
    f->saved_overrides = rt->overrides;
    f->saved_n_overrides = rt->n_overrides;
    f->saved_cap_overrides = rt->cap_overrides;
    f->saved_curgroup = rt->curgroup;
    return idx;
}

void thrum_pop_try(TRuntime* rt) {
    if (rt->try_top > 0) rt->try_top--;
}

TVal thrum_last_error_map(TRuntime* rt) {
    T_FRAME(rt);
    T_VAR(rt, m, thrum_map(rt));
    TMap* mp = (TMap*)m.as.obj;
    thrum_map_set_cs(rt, mp, "message", thrum_cstr(rt, rt->err_msg ? rt->err_msg : ""));
    thrum_map_set_cs(rt, mp, "hint", thrum_cstr(rt, rt->err_hint ? rt->err_hint : ""));
    thrum_map_set_cs(rt, mp, "line", rt->err_line > 0 ? thrum_num_dimless((double)rt->err_line) : thrum_nil());
    T_END_FRAME(rt);
    return m;
}

_Noreturn void thrum_error(TRuntime* rt, int line, int col, const char* msg, const char* hint) {
    if (rt) {
        rt->err_msg = thrum_intern(rt, msg ? msg : "error");
        rt->err_hint = hint ? thrum_intern(rt, hint) : NULL;
        rt->err_line = line;
        rt->err_col = col;
        if (rt->try_top > 0) {
            TTryFrame f = rt->try_stack[--rt->try_top];
            thrum_unwind_cleanups(rt, f.saved_cleanup_top);
            rt->root_top = f.saved_root_top;
            rt->track_top = f.saved_track_top;
            rt->frames_top = f.saved_frames_top;
            rt->call_depth = f.saved_depth;
            rt->draining = f.saved_draining;
            if (rt->overrides != f.saved_overrides) {
                free(rt->overrides);
                rt->overrides = f.saved_overrides;
                rt->n_overrides = f.saved_n_overrides;
                rt->cap_overrides = f.saved_cap_overrides;
            }
            rt->curgroup = f.saved_curgroup;
            longjmp(f.buf, 1);
        }
        thrum_unwind_cleanups(rt, 0);
        const char* where = rt->prog_path ? rt->prog_path : "<native>";
        if (line > 0) fprintf(stderr, "%s:%d:%d: error: %s\n", where, line, col > 0 ? col : 1, rt->err_msg);
        else fprintf(stderr, "%s: error: %s\n", where, rt->err_msg);
        if (line >= 1 && rt->prog_lines && line <= rt->prog_nlines) {
            fprintf(stderr, "  %5d | %s\n", line, rt->prog_lines[line - 1]);
            int pad = (col > 1) ? (col - 1) : 0;
            fprintf(stderr, "        | %*s^\n", pad, "");
        }
        if (hint) fprintf(stderr, "  hint: %s\n", hint);
    } else {
        fprintf(stderr, "error: %s\n", msg ? msg : "error");
    }
    exit(1);
}

/* ============================== Strings, Lists, Maps ============================== */
static uint64_t fnv1a(const char* s, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

size_t thrum_utf8_count(const char* s, size_t nbytes) {
    size_t c = 0;
    for (size_t i = 0; i < nbytes; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    }
    return c;
}

size_t thrum_utf8_cp_len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t thrum_utf8_byte_off(const TStr* s, size_t char_idx) {
    if (s->ulen == s->len) return char_idx < s->len ? char_idx : s->len;
    size_t off = 0, ci = 0;
    while (off < s->len && ci < char_idx) {
        size_t cl = thrum_utf8_cp_len((unsigned char)s->data[off]);
        off += (off + cl <= s->len) ? cl : (s->len - off);
        ci++;
    }
    return off;
}

TVal thrum_str(TRuntime* rt, const char* s, size_t len) {
    TStr* obj = (TStr*)alloc_obj(rt, OBJ_STR, sizeof(TStr) + len + 1);
    obj->len = len;
    if (len > 0 && s) memcpy(obj->data, s, len);
    obj->data[len] = '\0';
    obj->ulen = thrum_utf8_count(obj->data, len);
    obj->hash = fnv1a(obj->data, len);
    return thrum_obj((TObj*)obj);
}

TVal thrum_cstr(TRuntime* rt, const char* s) {
    return thrum_str(rt, s ? s : "", s ? strlen(s) : 0);
}

TVal thrum_list(TRuntime* rt, size_t cap) {
    TList* l = (TList*)alloc_obj(rt, OBJ_LIST, sizeof(TList));
    l->len = 0;
    l->cap = cap > 4 ? cap : 4;
    l->items = (TVal*)calloc(l->cap, sizeof(TVal));
    return thrum_obj((TObj*)l);
}

void thrum_list_push(TRuntime* rt, TList* lst, TVal v) {
    (void)rt;
    if (lst->len >= lst->cap) {
        lst->cap = lst->cap ? lst->cap * 2 : 8;
        lst->items = (TVal*)realloc(lst->items, lst->cap * sizeof(TVal));
    }
    lst->items[lst->len++] = v;
}

static uint64_t hash_val(TVal v) {
    switch (v.kind) {
        case TV_NIL: return 0x9e3779b97f4a7c15ULL;
        case TV_BOOL: return v.as.b ? 0x517cc1b727220a95ULL : 0x2545F4914F6CDD1DULL;
        case TV_NUM: {
            uint64_t h = 0;
            double dv = v.as.q.v == 0.0 ? 0.0 : v.as.q.v;
            uint64_t bits = 0;
            memcpy(&bits, &dv, sizeof(bits));
            h ^= bits * 0xbf58476d1ce4e5b9ULL;
            for (int i = 0; i < 7; i++) {
                h = (h * 31) ^ (uint64_t)v.as.q.d.e[i].num;
                h = (h * 31) ^ (uint64_t)v.as.q.d.e[i].den;
            }
            return h;
        }
        case TV_OBJ:
            if (v.as.obj->kind == OBJ_STR) return ((TStr*)v.as.obj)->hash;
            return (uint64_t)(uintptr_t)v.as.obj;
    }
    return 0;
}

static bool key_eq(TVal a, TVal b) {
    if (a.kind != b.kind) return false;
    return thrum_veq(a, b);
}

TVal thrum_map(TRuntime* rt) {
    TMap* m = (TMap*)alloc_obj(rt, OBJ_MAP, sizeof(TMap));
    m->cap = 16;
    m->entries = (TMapEntry*)calloc(m->cap, sizeof(TMapEntry));
    m->order_cap = 8;
    m->order_keys = (TVal*)calloc(m->order_cap, sizeof(TVal));
    return thrum_obj((TObj*)m);
}

static void map_rehash(TMap* m, size_t new_cap) {
    TMapEntry* old = m->entries;
    size_t old_cap = m->cap;
    m->entries = (TMapEntry*)calloc(new_cap, sizeof(TMapEntry));
    m->cap = new_cap;
    m->count = 0;
    m->used_slots = 0;
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].used && !old[i].tomb) {
            uint64_t h = hash_val(old[i].key);
            size_t idx = (size_t)(h & (new_cap - 1));
            while (m->entries[idx].used) idx = (idx + 1) & (new_cap - 1);
            m->entries[idx] = old[i];
            m->count++;
            m->used_slots++;
        }
    }
    free(old);
}

void thrum_map_set(TRuntime* rt, TMap* m, TVal k, TVal v) {
    if (k.kind == TV_OBJ && (k.as.obj->kind == OBJ_LIST || k.as.obj->kind == OBJ_MAP)) {
        thrum_error(rt, 0, 0, "map keys cannot be list or map", NULL);
    }
    if ((m->used_slots + 1) * 10 >= m->cap * 7) map_rehash(m, m->cap * 2);
    uint64_t h = hash_val(k);
    size_t idx = (size_t)(h & (m->cap - 1));
    size_t first_tomb = (size_t)-1;
    while (m->entries[idx].used) {
        if (!m->entries[idx].tomb && key_eq(m->entries[idx].key, k)) {
            m->entries[idx].val = v;
            return;
        }
        if (m->entries[idx].tomb && first_tomb == (size_t)-1) first_tomb = idx;
        idx = (idx + 1) & (m->cap - 1);
    }
    size_t ins = (first_tomb != (size_t)-1) ? first_tomb : idx;
    if (first_tomb == (size_t)-1) m->used_slots++;
    m->entries[ins].key = k;
    m->entries[ins].val = v;
    m->entries[ins].used = true;
    m->entries[ins].tomb = false;
    m->count++;
    if (m->order_len >= m->order_cap) {
        m->order_cap *= 2;
        m->order_keys = (TVal*)realloc(m->order_keys, m->order_cap * sizeof(TVal));
    }
    m->order_keys[m->order_len++] = k;
}

void thrum_map_set_cs(TRuntime* rt, TMap* m, const char* k, TVal v) {
    T_FRAME(rt);
    T_VAR(rt, mv, thrum_obj((TObj*)m));
    T_VAR(rt, vv, v);
    T_VAR(rt, kv, thrum_cstr(rt, k));
    (void)mv;
    thrum_map_set(rt, m, kv, vv);
    T_END_FRAME(rt);
}

bool thrum_map_get(TMap* m, TVal k, TVal* out) {
    if (m->count == 0) return false;
    uint64_t h = hash_val(k);
    size_t idx = (size_t)(h & (m->cap - 1));
    while (m->entries[idx].used) {
        if (!m->entries[idx].tomb && key_eq(m->entries[idx].key, k)) {
            if (out) *out = m->entries[idx].val;
            return true;
        }
        idx = (idx + 1) & (m->cap - 1);
    }
    return false;
}

bool thrum_map_has(TMap* m, TVal k) {
    return thrum_map_get(m, k, NULL);
}

/* ============================== Value Ops, Formatting, Equality ============================== */
const char* thrum_tname(TVal v) {
    switch (v.kind) {
        case TV_NIL: return "nil";
        case TV_BOOL: return "bool";
        case TV_NUM: return "num";
        case TV_OBJ:
            switch (v.as.obj->kind) {
                case OBJ_STR: return "text";
                case OBJ_LIST: return "list";
                case OBJ_MAP: return "map";
                case OBJ_RECTYPE: return "record";
                case OBJ_RECVAL: return ((TRecVal*)v.as.obj)->rtype->name;
                case OBJ_FN:
                case OBJ_BUILTIN: return "fn";
                case OBJ_MOD: return "module";
                case OBJ_CAP: return "cap";
                default: return "?";
            }
    }
    return "?";
}

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} SB;

static void sb_init(SB* sb) {
    sb->cap = 64; sb->len = 0; sb->data = (char*)malloc(sb->cap); sb->data[0] = '\0';
}
static void sb_app_n(SB* sb, const char* s, size_t n) {
    if (sb->len + n + 1 > sb->cap) {
        while (sb->len + n + 1 > sb->cap) sb->cap *= 2;
        sb->data = (char*)realloc(sb->data, sb->cap);
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}
static void sb_app(SB* sb, const char* s) { sb_app_n(sb, s, strlen(s)); }

static bool is_ident_str(const char* s, size_t n) {
    if (n == 0) return false;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (size_t i = 1; i < n; i++) {
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return false;
    }
    return true;
}

static void vstr_sb(SB* sb, TVal v, bool quote) {
    switch (v.kind) {
        case TV_NIL: sb_app(sb, "nil"); return;
        case TV_BOOL: sb_app(sb, v.as.b ? "true" : "false"); return;
        case TV_NUM: {
            char buf[200];
            thrum_qfmt(v.as.q, buf, sizeof(buf));
            sb_app(sb, buf);
            return;
        }
        case TV_OBJ: {
            TObj* o = v.as.obj;
            if (o->kind == OBJ_STR) {
                TStr* s = (TStr*)o;
                if (quote) sb_app_n(sb, "\"", 1);
                sb_app_n(sb, s->data, s->len);
                if (quote) sb_app_n(sb, "\"", 1);
                return;
            }
            if (o->kind == OBJ_LIST) {
                TList* l = (TList*)o;
                sb_app_n(sb, "[", 1);
                for (size_t i = 0; i < l->len; i++) {
                    if (i > 0) sb_app(sb, ", ");
                    vstr_sb(sb, l->items[i], true);
                }
                sb_app_n(sb, "]", 1);
                return;
            }
            if (o->kind == OBJ_MAP) {
                TMap* m = (TMap*)o;
                sb_app_n(sb, "{", 1);
                bool first = true;
                for (size_t i = 0; i < m->order_len; i++) {
                    TVal val;
                    if (!thrum_map_get(m, m->order_keys[i], &val)) continue;
                    if (!first) sb_app(sb, ", ");
                    first = false;
                    TVal k = m->order_keys[i];
                    bool qk = (k.kind == TV_OBJ && k.as.obj->kind == OBJ_STR && !is_ident_str(((TStr*)k.as.obj)->data, ((TStr*)k.as.obj)->len));
                    vstr_sb(sb, k, qk);
                    sb_app(sb, ": ");
                    vstr_sb(sb, val, true);
                }
                sb_app_n(sb, "}", 1);
                return;
            }
            if (o->kind == OBJ_RECVAL) {
                TRecVal* rv = (TRecVal*)o;
                sb_app(sb, rv->rtype->name);
                sb_app_n(sb, "{", 1);
                for (int i = 0; i < rv->rtype->nfields; i++) {
                    if (i > 0) sb_app(sb, ", ");
                    sb_app(sb, rv->rtype->fields[i].name);
                    sb_app(sb, ": ");
                    vstr_sb(sb, rv->fields[i], true);
                }
                sb_app_n(sb, "}", 1);
                return;
            }
            if (o->kind == OBJ_RECTYPE) {
                sb_app(sb, "<record "); sb_app(sb, ((TRecType*)o)->name); sb_app(sb, ">"); return;
            }
            if (o->kind == OBJ_FN) {
                const char* nm = ((TFn*)o)->name;
                sb_app(sb, "<fn "); sb_app(sb, nm ? nm : "anonymous"); sb_app(sb, ">"); return;
            }
            if (o->kind == OBJ_BUILTIN) {
                sb_app(sb, "<builtin "); sb_app(sb, ((TBuiltin*)o)->name); sb_app(sb, ">"); return;
            }
            if (o->kind == OBJ_MOD) {
                sb_app(sb, "<module "); sb_app(sb, ((TMod*)o)->name); sb_app(sb, ">"); return;
            }
            if (o->kind == OBJ_CAP) {
                sb_app(sb, "<cap "); sb_app(sb, ((TCap*)o)->name); sb_app(sb, ">"); return;
            }
        }
    }
}

char* thrum_vstr_alloc(TRuntime* rt, TVal v, bool quote) {
    (void)rt;
    SB sb; sb_init(&sb);
    vstr_sb(&sb, v, quote);
    return sb.data;
}

TVal thrum_vstr_val(TRuntime* rt, TVal v, bool quote) {
    char* s = thrum_vstr_alloc(rt, v, quote);
    TVal out = thrum_cstr(rt, s);
    free(s);
    return out;
}

TVal thrum_interp_text(TRuntime* rt, int nparts, TVal* parts) {
    SB sb; sb_init(&sb);
    for (int i = 0; i < nparts; i++) vstr_sb(&sb, parts[i], false);
    TVal out = thrum_str(rt, sb.data, sb.len);
    free(sb.data);
    return out;
}

bool thrum_veq(TVal a, TVal b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case TV_NIL: return true;
        case TV_BOOL: return a.as.b == b.as.b;
        case TV_NUM: return thrum_dim_eq(a.as.q.d, b.as.q.d) && a.as.q.v == b.as.q.v;
        case TV_OBJ: {
            TObj* oa = a.as.obj; TObj* ob = b.as.obj;
            if (oa == ob) return true;
            if (oa->kind != ob->kind) return false;
            if (oa->kind == OBJ_STR) {
                TStr* sa = (TStr*)oa; TStr* sb = (TStr*)ob;
                return sa->len == sb->len && sa->hash == sb->hash && memcmp(sa->data, sb->data, sa->len) == 0;
            }
            if (oa->kind == OBJ_LIST) {
                TList* la = (TList*)oa; TList* lb = (TList*)ob;
                if (la->len != lb->len) return false;
                for (size_t i = 0; i < la->len; i++) if (!thrum_veq(la->items[i], lb->items[i])) return false;
                return true;
            }
            if (oa->kind == OBJ_MAP) {
                TMap* ma = (TMap*)oa; TMap* mb = (TMap*)ob;
                if (ma->count != mb->count) return false;
                for (size_t i = 0; i < ma->cap; i++) {
                    if (ma->entries[i].used && !ma->entries[i].tomb) {
                        TVal bv;
                        if (!thrum_map_get(mb, ma->entries[i].key, &bv) || !thrum_veq(ma->entries[i].val, bv)) return false;
                    }
                }
                return true;
            }
            if (oa->kind == OBJ_RECVAL) {
                TRecVal* ra = (TRecVal*)oa; TRecVal* rb = (TRecVal*)ob;
                if (ra->rtype != rb->rtype) return false;
                for (int i = 0; i < ra->rtype->nfields; i++) {
                    if (!thrum_veq(ra->fields[i], rb->fields[i])) return false;
                }
                return true;
            }
            return false;
        }
    }
    return false;
}

bool thrum_truthy(TVal v) {
    switch (v.kind) {
        case TV_NIL: return false;
        case TV_BOOL: return v.as.b;
        case TV_NUM: return v.as.q.v != 0.0;
        case TV_OBJ:
            if (v.as.obj->kind == OBJ_STR) return ((TStr*)v.as.obj)->len > 0;
            if (v.as.obj->kind == OBJ_LIST) return ((TList*)v.as.obj)->len > 0;
            if (v.as.obj->kind == OBJ_MAP) return ((TMap*)v.as.obj)->count > 0;
            return true;
    }
    return true;
}

double thrum_relchg(TVal a, TVal b) {
    if (a.kind == TV_NUM && b.kind == TV_NUM && thrum_dim_eq(a.as.q.d, b.as.q.d)) {
        double denom = fmax(fmax(fabs(a.as.q.v), fabs(b.as.q.v)), 1e-12);
        return fabs(a.as.q.v - b.as.q.v) / denom;
    }
    return thrum_veq(a, b) ? 0.0 : 1.0;
}

TQ thrum_need_q(TRuntime* rt, TVal v, int line, int col) {
    if (v.kind != TV_NUM) {
        char msg[80];
        snprintf(msg, sizeof(msg), "expected a number, got %s", thrum_tname(v));
        thrum_error(rt, line, col, msg, NULL);
    }
    return v.as.q;
}

double thrum_need_num(TRuntime* rt, TVal v, int line, int col) {
    return thrum_need_q(rt, v, line, col).v;
}

int64_t thrum_need_int(TRuntime* rt, TVal v, int line, int col) {
    TQ q = thrum_need_q(rt, v, line, col);
    if (!thrum_dimless(q.d)) {
        char lb[64], msg[128];
        thrum_dimstr(q.d, lb, sizeof(lb));
        snprintf(msg, sizeof(msg), "expected a plain count, got %s", lb);
        thrum_error(rt, line, col, msg, NULL);
    }
    if (!isfinite(q.v) || q.v != floor(q.v)) {
        char msg[80];
        snprintf(msg, sizeof(msg), "expected a whole number, got %g", q.v);
        thrum_error(rt, line, col, msg, NULL);
    }
    return (int64_t)q.v;
}

bool thrum_in(TRuntime* rt, TVal a, TVal b, int line, int col) {
    if (b.kind == TV_OBJ) {
        if (b.as.obj->kind == OBJ_LIST) {
            TList* l = (TList*)b.as.obj;
            for (size_t i = 0; i < l->len; i++) if (thrum_veq(a, l->items[i])) return true;
            return false;
        }
        if (b.as.obj->kind == OBJ_MAP) {
            if (a.kind == TV_OBJ && (a.as.obj->kind == OBJ_LIST || a.as.obj->kind == OBJ_MAP)) return false;
            return thrum_map_has((TMap*)b.as.obj, a);
        }
        if (b.as.obj->kind == OBJ_STR) {
            char* sa = thrum_vstr_alloc(rt, a, false);
            bool found = strstr(((TStr*)b.as.obj)->data, sa) != NULL;
            free(sa);
            return found;
        }
    }
    thrum_error(rt, line, col, "'in' needs a list, map or text", NULL);
}

TVal thrum_range_op(TRuntime* rt, TVal va, TVal vb, bool excl, int line, int col) {
    int64_t i = thrum_need_int(rt, va, line, col);
    int64_t j = thrum_need_int(rt, vb, line, col);
    if (excl) j--;
    if (j - i > 1000000) thrum_error(rt, line, col, "range too large", NULL);
    T_FRAME(rt);
    T_VAR(rt, lst, thrum_list(rt, (j >= i) ? (size_t)(j - i + 1) : 0));
    TList* L = (TList*)lst.as.obj;
    for (int64_t k = i; k <= j; k++) thrum_list_push(rt, L, thrum_num_dimless((double)k));
    T_END_FRAME(rt);
    return lst;
}

/* ============================== Records, Indexing, Slicing, Types ============================== */
static bool val_matches_type(TVal v, TTypeAnn t) {
    if (t.kind == ANN_NONE) return true;
    if (t.kind == ANN_KIND) {
        if (strcmp(t.name, "any") == 0) return true;
        if (strcmp(t.name, "record") == 0) return v.kind == TV_OBJ && v.as.obj->kind == OBJ_RECVAL;
        return strcmp(thrum_tname(v), t.name) == 0;
    }
    if (t.kind == ANN_RECORD) {
        return v.kind == TV_OBJ && v.as.obj->kind == OBJ_RECVAL &&
               strcmp(((TRecVal*)v.as.obj)->rtype->name, t.name) == 0;
    }
    if (t.kind == ANN_DIM) {
        return v.kind == TV_NUM && thrum_dim_eq(v.as.q.d, t.dim);
    }
    if (t.kind == ANN_LIST) {
        if (v.kind != TV_OBJ || v.as.obj->kind != OBJ_LIST) return false;
        if (t.nsubs >= 1 && t.subs) {
            TList* l = (TList*)v.as.obj;
            for (size_t i = 0; i < l->len; i++) {
                if (!val_matches_type(l->items[i], t.subs[0])) return false;
            }
        }
        return true;
    }
    if (t.kind == ANN_MAP) {
        if (v.kind != TV_OBJ || v.as.obj->kind != OBJ_MAP) return false;
        if (t.nsubs >= 2 && t.subs) {
            TMap* m = (TMap*)v.as.obj;
            for (size_t i = 0; i < m->order_len; i++) {
                TVal k = m->order_keys[i], mv;
                if (thrum_map_get(m, k, &mv)) {
                    if (!val_matches_type(k, t.subs[0]) || !val_matches_type(mv, t.subs[1])) return false;
                }
            }
        }
        return true;
    }
    if (t.kind == ANN_UNION) {
        for (int i = 0; i < t.nsubs; i++) {
            if (val_matches_type(v, t.subs[i])) return true;
        }
        return false;
    }
    return true;
}

void thrum_check_type(TRuntime* rt, TVal v, TTypeAnn t, const char* what, int line, int col) {
    if (t.kind == ANN_NONE) return;
    if (t.kind == ANN_KIND) {
        if (strcmp(t.name, "any") == 0) return;
        if (strcmp(t.name, "record") == 0) {
            if (v.kind != TV_OBJ || v.as.obj->kind != OBJ_RECVAL) {
                char msg[128];
                snprintf(msg, sizeof(msg), "%s should be record, got %s", what, thrum_tname(v));
                thrum_error(rt, line, col, msg, NULL);
            }
            return;
        }
        if (strcmp(thrum_tname(v), t.name) != 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s should be %s, got %s", what, t.name, thrum_tname(v));
            thrum_error(rt, line, col, msg, NULL);
        }
        return;
    }
    if (t.kind == ANN_RECORD) {
        if (v.kind != TV_OBJ || v.as.obj->kind != OBJ_RECVAL || strcmp(((TRecVal*)v.as.obj)->rtype->name, t.name) != 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s should be %s, got %s", what, t.name, thrum_tname(v));
            thrum_error(rt, line, col, msg, NULL);
        }
        return;
    }
    if (t.kind == ANN_DIM) {
        if (v.kind != TV_NUM || !thrum_dim_eq(v.as.q.d, t.dim)) {
            char got[64], msg[160];
            if (v.kind == TV_NUM) {
                thrum_dimstr(v.as.q.d, got, sizeof(got));
                if (!got[0]) strcpy(got, "dimensionless");
            } else {
                snprintf(got, sizeof(got), "%s", thrum_tname(v));
            }
            snprintf(msg, sizeof(msg), "%s should have dimension %s, got %s", what, t.name, got);
            thrum_error(rt, line, col, msg, NULL);
        }
        return;
    }
    if (t.kind == ANN_LIST) {
        if (v.kind != TV_OBJ || v.as.obj->kind != OBJ_LIST) {
            char msg[160];
            snprintf(msg, sizeof(msg), "%s should be %s, got %s", what, t.name ? t.name : "list", thrum_tname(v));
            thrum_error(rt, line, col, msg, NULL);
        }
        if (t.nsubs >= 1 && t.subs) {
            TList* l = (TList*)v.as.obj;
            for (size_t i = 0; i < l->len; i++) {
                char item_what[192];
                snprintf(item_what, sizeof(item_what), "element %zu of %s", i, what);
                thrum_check_type(rt, l->items[i], t.subs[0], item_what, line, col);
            }
        }
        return;
    }
    if (t.kind == ANN_MAP) {
        if (v.kind != TV_OBJ || v.as.obj->kind != OBJ_MAP) {
            char msg[160];
            snprintf(msg, sizeof(msg), "%s should be %s, got %s", what, t.name ? t.name : "map", thrum_tname(v));
            thrum_error(rt, line, col, msg, NULL);
        }
        if (t.nsubs >= 2 && t.subs) {
            TMap* m = (TMap*)v.as.obj;
            for (size_t i = 0; i < m->order_len; i++) {
                TVal k = m->order_keys[i], mv;
                if (thrum_map_get(m, k, &mv)) {
                    char kw[192], vw[192];
                    snprintf(kw, sizeof(kw), "key of %s", what);
                    snprintf(vw, sizeof(vw), "value of %s", what);
                    thrum_check_type(rt, k, t.subs[0], kw, line, col);
                    thrum_check_type(rt, mv, t.subs[1], vw, line, col);
                }
            }
        }
        return;
    }
    if (t.kind == ANN_UNION) {
        if (!val_matches_type(v, t)) {
            char msg[192];
            snprintf(msg, sizeof(msg), "%s should be %s, got %s", what, t.name ? t.name : "union", thrum_tname(v));
            thrum_error(rt, line, col, msg, NULL);
        }
    }
}

TVal thrum_make_record_pos(TRuntime* rt, TRecType* rt_type, int argc, TVal* argv, int line, int col) {
    int nreq = 0;
    for (int i = 0; i < rt_type->nfields; i++) if (!rt_type->fields[i].has_default) nreq++;
    if (argc < nreq || argc > rt_type->nfields) {
        char msg[128];
        snprintf(msg, sizeof(msg), "record %s expects %d..%d arguments, got %d", rt_type->name, nreq, rt_type->nfields, argc);
        thrum_error(rt, line, col, msg, NULL);
    }
    T_FRAME(rt);
    TRecVal* rv = (TRecVal*)alloc_obj(rt, OBJ_RECVAL, sizeof(TRecVal));
    rv->rtype = rt_type;
    rv->fields = (TVal*)calloc((size_t)rt_type->nfields, sizeof(TVal));
    T_VAR(rt, out, thrum_obj((TObj*)rv));
    for (int i = 0; i < rt_type->nfields; i++) {
        TVal val = (i < argc) ? argv[i] : rt_type->fields[i].default_fn(rt, rt_type->env);
        char what[128];
        snprintf(what, sizeof(what), "field '%s' of %s", rt_type->fields[i].name, rt_type->name);
        thrum_check_type(rt, val, rt_type->fields[i].type, what, line, col);
        rv->fields[i] = val;
    }
    T_END_FRAME(rt);
    return out;
}

TVal thrum_make_record_map(TRuntime* rt, TVal target, TMap* provided, int line, int col) {
    if (target.kind != TV_OBJ || target.as.obj->kind != OBJ_RECTYPE) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s is not a record type", thrum_tname(target));
        thrum_error(rt, line, col, msg, NULL);
    }
    TRecType* rtype = (TRecType*)target.as.obj;
    for (size_t i = 0; i < provided->order_len; i++) {
        TVal k = provided->order_keys[i];
        if (!thrum_map_has(provided, k)) continue;
        bool ok = false;
        if (k.kind == TV_OBJ && k.as.obj->kind == OBJ_STR) {
            const char* ks = ((TStr*)k.as.obj)->data;
            for (int f = 0; f < rtype->nfields; f++) if (strcmp(rtype->fields[f].name, ks) == 0) { ok = true; break; }
        }
        if (!ok) {
            char* ks = thrum_vstr_alloc(rt, k, false);
            char msg[160];
            snprintf(msg, sizeof(msg), "record '%s' has no field '%s'", rtype->name, ks);
            free(ks);
            thrum_error(rt, line, col, msg, NULL);
        }
    }
    T_FRAME(rt);
    T_VAR(rt, pmap, thrum_obj((TObj*)provided));
    TRecVal* rv = (TRecVal*)alloc_obj(rt, OBJ_RECVAL, sizeof(TRecVal));
    rv->rtype = rtype;
    rv->fields = (TVal*)calloc((size_t)rtype->nfields, sizeof(TVal));
    T_VAR(rt, out, thrum_obj((TObj*)rv));
    for (int i = 0; i < rtype->nfields; i++) {
        TVal k = thrum_cstr(rt, rtype->fields[i].name);
        TVal val;
        if (!thrum_map_get((TMap*)pmap.as.obj, k, &val)) {
            if (!rtype->fields[i].has_default) {
                char msg[128];
                snprintf(msg, sizeof(msg), "missing required field '%s' for record '%s'", rtype->fields[i].name, rtype->name);
                thrum_error(rt, line, col, msg, NULL);
            }
            val = rtype->fields[i].default_fn(rt, rtype->env);
        }
        char what[128];
        snprintf(what, sizeof(what), "field '%s' of %s", rtype->fields[i].name, rtype->name);
        thrum_check_type(rt, val, rtype->fields[i].type, what, line, col);
        rv->fields[i] = val;
    }
    T_END_FRAME(rt);
    return out;
}

TVal thrum_get_index(TRuntime* rt, TVal obj, TVal idx, int line, int col) {
    if (obj.kind == TV_OBJ) {
        if (obj.as.obj->kind == OBJ_LIST) {
            TList* l = (TList*)obj.as.obj;
            int64_t i = thrum_need_int(rt, idx, line, col);
            if (i < 0) i += (int64_t)l->len;
            if (i < 0 || (size_t)i >= l->len) {
                char msg[96];
                snprintf(msg, sizeof(msg), "index %lld out of range (len %zu)", (long long)i, l->len);
                thrum_error(rt, line, col, msg, NULL);
            }
            return l->items[i];
        }
        if (obj.as.obj->kind == OBJ_MAP) {
            if (idx.kind == TV_OBJ && (idx.as.obj->kind == OBJ_LIST || idx.as.obj->kind == OBJ_MAP)) {
                char msg[96];
                snprintf(msg, sizeof(msg), "map keys cannot be %s", thrum_tname(idx));
                thrum_error(rt, line, col, msg, NULL);
            }
            TVal out = thrum_nil();
            thrum_map_get((TMap*)obj.as.obj, idx, &out);
            return out;
        }
        if (obj.as.obj->kind == OBJ_STR) {
            TStr* s = (TStr*)obj.as.obj;
            int64_t i = thrum_need_int(rt, idx, line, col);
            if (i < 0) i += (int64_t)s->ulen;
            if (i < 0 || (size_t)i >= s->ulen) thrum_error(rt, line, col, "index out of range", NULL);
            size_t off = thrum_utf8_byte_off(s, (size_t)i);
            size_t cp = thrum_utf8_cp_len((unsigned char)s->data[off]);
            if (off + cp > s->len) cp = s->len - off;
            return thrum_str(rt, s->data + off, cp);
        }
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "cannot index %s", thrum_tname(obj));
    thrum_error(rt, line, col, msg, NULL);
}

TVal thrum_get_slice(TRuntime* rt, TVal obj, bool has_start, TVal start_v, bool has_stop, TVal stop_v, bool has_step, TVal step_v, int line, int col) {
    if (obj.kind != TV_OBJ || (obj.as.obj->kind != OBJ_LIST && obj.as.obj->kind != OBJ_STR)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "cannot slice %s", thrum_tname(obj));
        thrum_error(rt, line, col, msg, NULL);
    }
    int64_t n = (obj.as.obj->kind == OBJ_LIST) ? (int64_t)((TList*)obj.as.obj)->len : (int64_t)((TStr*)obj.as.obj)->ulen;
    int64_t step = has_step ? thrum_need_int(rt, step_v, line, col) : 1;
    if (step == 0) thrum_error(rt, line, col, "slice step cannot be 0", NULL);
    int64_t start, stop;
    if (step > 0) {
        start = has_start ? thrum_need_int(rt, start_v, line, col) : 0;
        stop = has_stop ? thrum_need_int(rt, stop_v, line, col) : n;
        if (start < 0) start += n;
        if (stop < 0) stop += n;
        if (start < 0) start = 0; if (start > n) start = n;
        if (stop < 0) stop = 0; if (stop > n) stop = n;
    } else {
        start = has_start ? thrum_need_int(rt, start_v, line, col) : (n - 1);
        stop = has_stop ? thrum_need_int(rt, stop_v, line, col) : (-n - 1);
        if (has_start && start < 0) start += n;
        if (has_stop && stop < 0) stop += n;
        if (start >= n) start = n - 1;
        if (start < -1) start = -1;
        if (stop >= n) stop = n - 1;
        if (stop < -1) stop = -1;
    }
    if (obj.as.obj->kind == OBJ_LIST) {
        T_FRAME(rt);
        T_VAR(rt, vo, obj);
        T_VAR(rt, res, thrum_list(rt, 8));
        TList* src = (TList*)vo.as.obj;
        TList* dst = (TList*)res.as.obj;
        if (step > 0) {
            for (int64_t i = start; i < stop; i += step) thrum_list_push(rt, dst, src->items[i]);
        } else {
            for (int64_t i = start; i > stop; i += step) thrum_list_push(rt, dst, src->items[i]);
        }
        T_END_FRAME(rt);
        return res;
    } else {
        TStr* src = (TStr*)obj.as.obj;
        if (src->ulen == src->len) {
            if (step == 1) {
                size_t len = (stop > start) ? (size_t)(stop - start) : 0;
                return thrum_str(rt, src->data + start, len);
            }
            SB sb; sb_init(&sb);
            if (step > 0) {
                for (int64_t i = start; i < stop; i += step) sb_app_n(&sb, src->data + i, 1);
            } else {
                for (int64_t i = start; i > stop; i += step) sb_app_n(&sb, src->data + i, 1);
            }
            TVal out = thrum_str(rt, sb.data, sb.len);
            free(sb.data);
            return out;
        }
        size_t* offs = (size_t*)malloc((src->ulen + 1) * sizeof(size_t));
        size_t pos = 0;
        for (size_t ci = 0; ci < src->ulen; ci++) {
            offs[ci] = pos;
            size_t cl = thrum_utf8_cp_len((unsigned char)src->data[pos]);
            pos += (pos + cl <= src->len) ? cl : (src->len - pos);
        }
        offs[src->ulen] = src->len;
        if (step == 1) {
            TVal out = (stop > start)
                ? thrum_str(rt, src->data + offs[start], offs[stop] - offs[start])
                : thrum_cstr(rt, "");
            free(offs);
            return out;
        }
        SB sb; sb_init(&sb);
        if (step > 0) {
            for (int64_t i = start; i < stop; i += step) {
                sb_app_n(&sb, src->data + offs[i], offs[i + 1] - offs[i]);
            }
        } else {
            for (int64_t i = start; i > stop; i += step) {
                sb_app_n(&sb, src->data + offs[i], offs[i + 1] - offs[i]);
            }
        }
        free(offs);
        TVal out = thrum_str(rt, sb.data, sb.len);
        free(sb.data);
        return out;
    }
}

TVal thrum_get_dot(TRuntime* rt, TVal obj, const char* name, int line, int col) {
    if (obj.kind == TV_OBJ) {
        TObj* o = obj.as.obj;
        if (o->kind == OBJ_MOD) {
            TMod* m = (TMod*)o;
            TVal v = thrum_read(rt, m->scope, name, line, col);
            return v;
        }
        if (o->kind == OBJ_CAP) {
            TCap* c = (TCap*)o;
            TVal val = thrum_nil();
            if (!thrum_map_get(c->methods, thrum_cstr(rt, name), &val)) {
                char msg[128];
                snprintf(msg, sizeof(msg), "capability '%s' has no '%s'", c->name, name);
                thrum_error(rt, line, col, msg, NULL);
            }
            return val;
        }
        if (o->kind == OBJ_RECVAL) {
            TRecVal* rv = (TRecVal*)o;
            for (int i = 0; i < rv->rtype->nfields; i++) {
                if (strcmp(rv->rtype->fields[i].name, name) == 0) return rv->fields[i];
            }
            char msg[128];
            snprintf(msg, sizeof(msg), "record '%s' has no field '%s'", rv->rtype->name, name);
            thrum_error(rt, line, col, msg, NULL);
        }
        if (o->kind == OBJ_MAP) {
            TVal val = thrum_nil();
            thrum_map_get((TMap*)o, thrum_cstr(rt, name), &val);
            return val;
        }
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "cannot read '.%s' of %s", name, thrum_tname(obj));
    thrum_error(rt, line, col, msg, NULL);
}

/* ============================== Functions & Calls ============================== */
TVal thrum_make_fn(TRuntime* rt, TEnv* env, const char* name, TFnPtr ptr, int nparams, TParamSpec* params, TTypeAnn ret, int nreq, TContractSpec* reqs, int nens, TContractSpec* ens) {
    TFn* f = (TFn*)alloc_obj(rt, OBJ_FN, sizeof(TFn));
    f->name = name ? thrum_intern(rt, name) : NULL;
    f->fn = ptr;
    f->env = env;
    f->nparams = nparams;
    if (nparams > 0 && params) {
        f->params = (TParamSpec*)malloc((size_t)nparams * sizeof(TParamSpec));
        memcpy(f->params, params, (size_t)nparams * sizeof(TParamSpec));
    }
    f->ret = ret;
    f->nreq = nreq;
    if (nreq > 0 && reqs) {
        f->reqs = (TContractSpec*)malloc((size_t)nreq * sizeof(TContractSpec));
        memcpy(f->reqs, reqs, (size_t)nreq * sizeof(TContractSpec));
    }
    f->nens = nens;
    if (nens > 0 && ens) {
        f->ens = (TContractSpec*)malloc((size_t)nens * sizeof(TContractSpec));
        memcpy(f->ens, ens, (size_t)nens * sizeof(TContractSpec));
    }
    return thrum_obj((TObj*)f);
}

TVal thrum_call(TRuntime* rt, TVal callee, int argc, TVal* argv, int line, int col) {
    if (callee.kind != TV_OBJ) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s is not callable", thrum_tname(callee));
        thrum_error(rt, line, col, msg, NULL);
    }
    TObj* o = callee.as.obj;
    if (o->kind == OBJ_BUILTIN) {
        TBuiltin* b = (TBuiltin*)o;
        if (argc < b->min_arity || (b->max_arity >= 0 && argc > b->max_arity)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s expects %d..%d argument(s), got %d", b->name, b->min_arity, b->max_arity, argc);
            thrum_error(rt, line, col, msg, NULL);
        }
        if (b->cfn) return b->cfn(rt, b->ctx, argc, argv, line, col);
        return b->fn(rt, argc, argv, line, col);
    }
    if (o->kind == OBJ_RECTYPE) {
        return thrum_make_record_pos(rt, (TRecType*)o, argc, argv, line, col);
    }
    if (o->kind == OBJ_FN) {
        TFn* f = (TFn*)o;
        rt->call_depth++;
        if (rt->call_depth > 400) {
            rt->call_depth--;
            thrum_error(rt, line, col, "call stack too deep (400)", NULL);
        }
        int nreq = 0;
        for (int i = 0; i < f->nparams; i++) if (!f->params[i].has_default) nreq++;
        if (argc < nreq || argc > f->nparams) {
            rt->call_depth--;
            char msg[128];
            snprintf(msg, sizeof(msg), "%s expects %d..%d arguments, got %d", f->name ? f->name : "fn", nreq, f->nparams, argc);
            thrum_error(rt, line, col, msg, NULL);
        }
        T_FRAME(rt);
        T_VAR(rt, fval, callee);
        TEnv* sc = thrum_env_new(rt, f->env, false);
        T_VAR(rt, scval, thrum_obj((TObj*)sc));
        TVal stk_args[16];
        TVal* call_args = (f->nparams <= 16) ? stk_args : (TVal*)calloc((size_t)f->nparams, sizeof(TVal));
        for (int i = 0; i < f->nparams; i++) {
            call_args[i] = thrum_nil();
            thrum_push_root(rt, &call_args[i]);
        }
        for (int i = 0; i < f->nparams; i++) {
            TVal v = (i < argc) ? argv[i] : f->params[i].default_fn(rt, sc);
            char what[128];
            snprintf(what, sizeof(what), "parameter '%s'", f->params[i].name);
            thrum_check_type(rt, v, f->params[i].type, what, line, col);
            thrum_def_pin(rt, sc, f->params[i].name, v);
            call_args[i] = v;
        }
        for (int i = 0; i < f->nreq; i++) {
            if (!thrum_truthy(f->reqs[i].cond_fn(rt, sc))) {
                char msg[256];
                if (f->reqs[i].msg_fn) {
                    char* ms = thrum_vstr_alloc(rt, f->reqs[i].msg_fn(rt, sc), false);
                    snprintf(msg, sizeof(msg), "contract failed in %s: %s", f->name ? f->name : "fn", ms);
                    free(ms);
                } else {
                    snprintf(msg, sizeof(msg), "contract failed in %s: %s", f->name ? f->name : "fn", f->reqs[i].src);
                }
                rt->call_depth--;
                thrum_error(rt, line, col, msg, NULL);
            }
        }
        T_VAR(rt, ret, f->fn(rt, sc, f->nparams, call_args));
        for (int i = 0; i < f->nens; i++) {
            thrum_def_pin(rt, sc, "result", ret);
            if (!thrum_truthy(f->ens[i].cond_fn(rt, sc))) {
                char msg[256];
                if (f->ens[i].msg_fn) {
                    char* ms = thrum_vstr_alloc(rt, f->ens[i].msg_fn(rt, sc), false);
                    snprintf(msg, sizeof(msg), "postcondition failed in %s: %s", f->name ? f->name : "fn", ms);
                    free(ms);
                } else {
                    snprintf(msg, sizeof(msg), "postcondition failed in %s: %s", f->name ? f->name : "fn", f->ens[i].src);
                }
                rt->call_depth--;
                thrum_error(rt, line, col, msg, NULL);
            }
        }
        char rwhat[128];
        snprintf(rwhat, sizeof(rwhat), "return of %s", f->name ? f->name : "fn");
        thrum_check_type(rt, ret, f->ret, rwhat, line, col);
        if (call_args != stk_args) free(call_args);
        rt->call_depth--;
        T_END_FRAME(rt);
        return ret;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "%s is not callable", thrum_tname(callee));
    thrum_error(rt, line, col, msg, NULL);
}

TVal thrum_def_record(TRuntime* rt, TEnv* env, const char* name, int nfields, TFieldSpec* fields) {
    TRecType* r = (TRecType*)alloc_obj(rt, OBJ_RECTYPE, sizeof(TRecType));
    r->name = thrum_intern(rt, name);
    r->nfields = nfields;
    r->fields = (TFieldSpec*)malloc((size_t)nfields * sizeof(TFieldSpec));
    memcpy(r->fields, fields, (size_t)nfields * sizeof(TFieldSpec));
    r->env = env;
    TVal v = thrum_obj((TObj*)r);
    thrum_def_pin(rt, env, r->name, v);
    return v;
}

/* ============================== Runtime Lifecycle ============================== */
TRuntime* thrum_init(int argc, char** argv) {
    TRuntime* rt = (TRuntime*)calloc(1, sizeof(TRuntime));
    rt->gc_threshold = 4096;
    rt->out = stdout;
    rt->exe_path = (argc > 0 && argv) ? argv[0] : "";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) {
            rt->trace = true;
        } else if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) {
            rt->journal_record = true;
            rt->journal_path = argv[++i];
        } else if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            rt->journal_replay = true;
            rt->journal_path = argv[++i];
        } else if (strcmp(argv[i], "--") == 0) {
            rt->argc = argc - (i + 1);
            rt->argv = argv + (i + 1);
            break;
        } else if (!rt->argv) {
            /* Standalone binary forwards remaining args directly */
            rt->argc = argc - i;
            rt->argv = argv + i;
            break;
        }
    }
    rt->builtins = thrum_env_new(rt, NULL, false);
    rt->globals = thrum_env_new(rt, rt->builtins, true);
    rt->modules = (TMap*)thrum_map(rt).as.obj;
    thrum_install_builtins(rt);
    if (rt->journal_replay && rt->journal_path) {
        thrum_journal_load(rt, rt->journal_path);
    }
    return rt;
}

void thrum_finish(TRuntime* rt) {
    thrum_drain(rt);
    thrum_unwind_cleanups(rt, 0);
    if (rt->journal_record && rt->journal_path) {
        thrum_journal_save(rt);
    }
    if (rt->journal_replay && rt->jdiv > 0) {
        fprintf(stderr, "replay: %d output divergence(s)\n", rt->jdiv);
        exit(2);
    }
}
