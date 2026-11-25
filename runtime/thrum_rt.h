#ifndef THRUM_RT_H
#define THRUM_RT_H

#include <math.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TRuntime TRuntime;
typedef struct TObj TObj;
typedef struct TEnv TEnv;
typedef struct TCell TCell;
typedef struct TGroup TGroup;
typedef struct TReaction TReaction;
typedef struct TRecType TRecType;
typedef struct TCap TCap;

typedef enum {
    TV_NIL = 0,
    TV_BOOL,
    TV_NUM,
    TV_OBJ
} TValKind;

typedef struct {
    int64_t num;
    int64_t den;
} TFrac;

typedef struct {
    TFrac e[7]; /* m, kg, s, A, K, mol, cd */
} TDim;

typedef struct {
    double v;
    double u;
    TDim d;
    double disp_sc;
    const char* disp_sym; /* interned string or NULL */
} TQ;

typedef struct {
    TValKind kind;
    union {
        bool b;
        TQ q;
        TObj* obj;
    } as;
} TVal;

typedef enum {
    OBJ_STR = 1,
    OBJ_LIST,
    OBJ_MAP,
    OBJ_RECTYPE,
    OBJ_RECVAL,
    OBJ_FN,
    OBJ_BUILTIN,
    OBJ_MOD,
    OBJ_CAP,
    OBJ_CELL,
    OBJ_ENV,
    OBJ_GROUP
} TObjKind;

struct TObj {
    TObjKind kind;
    uint8_t marked;
    TObj* next;
};

typedef struct {
    TObj hdr;
    size_t len;
    size_t ulen;
    uint64_t hash;
    char data[];
} TStr;

typedef struct {
    TObj hdr;
    size_t len;
    size_t cap;
    TVal* items;
} TList;

typedef struct {
    TVal key;
    TVal val;
    bool used;
    bool tomb;
} TMapEntry;

typedef struct {
    TObj hdr;
    size_t count;
    size_t used_slots;
    size_t cap;
    TMapEntry* entries;
    /* Insertion-order index tracking */
    size_t order_len;
    size_t order_cap;
    TVal* order_keys;
} TMap;

typedef enum {
    ANN_NONE = 0,
    ANN_KIND,
    ANN_DIM,
    ANN_RECORD
} TAnnKind;

typedef struct {
    TAnnKind kind;
    const char* name; /* kind name, unit sym, or record name */
    TDim dim;
} TTypeAnn;

typedef TVal (*TExprFn)(TRuntime* rt, TEnv* env);
typedef TVal (*TFnPtr)(TRuntime* rt, TEnv* env, int argc, TVal* argv);
typedef TVal (*TBuiltinPtr)(TRuntime* rt, int argc, TVal* argv, int line, int col);
typedef TVal (*TBuiltinCtxPtr)(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col);

typedef struct {
    const char* name;
    TTypeAnn type;
    bool has_default;
    TExprFn default_fn;
} TFieldSpec;

struct TRecType {
    TObj hdr;
    const char* name;
    int nfields;
    TFieldSpec* fields;
    TEnv* env;
};

typedef struct {
    TObj hdr;
    TRecType* rtype;
    TVal* fields;
} TRecVal;

typedef struct {
    const char* name;
    TTypeAnn type;
    bool has_default;
    TExprFn default_fn;
} TParamSpec;

typedef struct {
    bool is_ensures;
    TExprFn cond_fn;
    TExprFn msg_fn; /* NULL if no custom msg */
    const char* src;
} TContractSpec;

typedef struct {
    TObj hdr;
    const char* name;
    int nparams;
    TParamSpec* params;
    TTypeAnn ret;
    int nreq;
    TContractSpec* reqs;
    int nens;
    TContractSpec* ens;
    TFnPtr fn;
    TEnv* env;
} TFn;

typedef struct {
    TObj hdr;
    const char* name;
    TBuiltinPtr fn;
    TBuiltinCtxPtr cfn;
    void* ctx;
    TObj* owner;
    int min_arity;
    int max_arity; /* -1 = variadic */
} TBuiltin;

typedef struct {
    TObj hdr;
    const char* name;
    TEnv* scope;
} TMod;

typedef void (*TCapCleanup)(TRuntime* rt, void* state);

struct TCap {
    TObj hdr;
    const char* name;
    TMap* methods;
    TCapCleanup cleanup;
    void (*free_state)(void* state);
    void* state;
};

typedef struct {
    const char* name;
    TVal val;
    bool is_const;
} TEnvSlot;

struct TEnv {
    TObj hdr;
    TEnv* parent;
    bool rel;
    size_t len;
    size_t cap;
    TEnvSlot* slots;
};

typedef enum {
    CELL_SOURCE = 0,
    CELL_DERIVED = 1
} TCellKind;

struct TCell {
    TObj hdr;
    int id;
    int born;
    const char* name;
    TCellKind ckind;
    TVal value;
    TExprFn expr;
    TEnv* env;
    int line;
    int col;
    const char* src;
    TCell** deps;
    int ndeps;
    int deps_cap;
    TCell** subs;
    int nsubs;
    int subs_cap;
    TCell** newdeps;
    int nnewdeps;
    int newdeps_cap;
    int height;
    TGroup* group;
    uint64_t version;
    uint64_t eval_id;
    bool dead;
    bool has_init;
};

struct TGroup {
    TObj hdr;
    int maxit;
    double tol;
    int line;
    int col;
    TCell** cells;
    int ncells;
    int cap;
};

typedef enum {
    REACT_RISE = 0,
    REACT_FALL,
    REACT_ALWAYS
} TReactMode;

struct TReaction {
    TCell* guard;
    TExprFn body;
    TEnv* env;
    TReactMode mode;
    bool prev;
    uint64_t seen;
};

typedef struct {
    TCell* cell;
    TVal val;
} TOverride;

typedef struct {
    jmp_buf buf;
    int saved_root_top;
    int saved_track_top;
    int saved_frames_top;
    int saved_cleanup_top;
    int saved_depth;
    bool saved_draining;
    TOverride* saved_overrides;
    int saved_n_overrides;
    int saved_cap_overrides;
    TGroup* saved_curgroup;
} TTryFrame;

typedef struct {
    TCap* cap;
} TCleanupEntry;

typedef struct {
    char* kind;
    char* key;
    TVal value;
} TJournalEv;

struct TRuntime {
    /* GC */
    TObj* all_objs;
    size_t num_objs;
    size_t gc_threshold;
    TVal** root_ptrs;
    int root_top;
    int root_cap;

    /* Interned strings */
    char** intern_strs;
    int n_intern;
    int cap_intern;

    /* Scopes & modules */
    TEnv* builtins;
    TEnv* globals;
    TMap* modules; /* path -> TMod* */

    /* Reactive graph state */
    int next_cell_id;
    TCell** track;
    int track_top;
    int track_cap;
    int* frames;
    int frames_top;
    int frames_cap;
    TOverride* overrides;
    int n_overrides;
    int cap_overrides;
    TReaction* reactions;
    int n_reactions;
    int cap_reactions;
    TGroup** groups;
    int n_groups;
    int cap_groups;
    TCell** queue;
    int n_queue;
    int cap_queue;
    bool draining;
    TGroup* curgroup;

    /* Exception / cleanup stacks */
    TTryFrame* try_stack;
    int try_top;
    int try_cap;
    TCleanupEntry* cleanups;
    int cleanup_top;
    int cleanup_cap;

    /* Last error */
    const char* err_msg;
    const char* err_hint;
    int err_line;
    int err_col;

    /* Runtime config & CLI */
    const char* prog_path;
    const char* const* prog_lines;
    int prog_nlines;
    const char* exe_path;
    int argc;
    char** argv;
    bool trace;
    FILE* out;
    int call_depth;

    /* Journal */
    bool journal_record;
    bool journal_replay;
    const char* journal_path;
    TJournalEv* jevents;
    int jlen;
    int jcap;
    int jidx;
    int jdiv;
};

/* ---- Lifecycle & GC ---- */
TRuntime* thrum_init(int argc, char** argv);
void thrum_finish(TRuntime* rt);
TObj* thrum_alloc_obj(TRuntime* rt, TObjKind kind, size_t size);
void thrum_push_root(TRuntime* rt, TVal* slot);
void thrum_pop_roots(TRuntime* rt, int target_top);
void thrum_gc(TRuntime* rt);
const char* thrum_intern(TRuntime* rt, const char* s);

#define T_FRAME(rt) int _t_gc_top = (rt)->root_top
#define T_END_FRAME(rt) ((rt)->root_top = _t_gc_top)
#define T_VAR(rt, name, init) TVal name = (init); thrum_push_root((rt), &name)

/* ---- Error / Try / Cleanup ---- */
_Noreturn void thrum_error(TRuntime* rt, int line, int col, const char* msg, const char* hint);
int thrum_push_try(TRuntime* rt);
void thrum_pop_try(TRuntime* rt);
TVal thrum_last_error_map(TRuntime* rt);
void thrum_push_cleanup(TRuntime* rt, TCap* cap);
void thrum_unwind_cleanups(TRuntime* rt, int target_top);

/* ---- Value constructors & helpers ---- */
static inline TVal thrum_nil(void) { TVal v; v.kind = TV_NIL; v.as.obj = NULL; return v; }
static inline TVal thrum_bool(bool b) { TVal v; v.kind = TV_BOOL; v.as.b = b; return v; }
static inline TVal thrum_obj(TObj* o) { TVal v; v.kind = TV_OBJ; v.as.obj = o; return v; }

TVal thrum_num_dimless(double x);
TVal thrum_qty(TRuntime* rt, double v, double u, TDim d, double disp_sc, const char* disp_sym);
TVal thrum_qty_lit(TRuntime* rt, double v, double u, const char* unit_str);
TVal thrum_str(TRuntime* rt, const char* s, size_t len);
TVal thrum_cstr(TRuntime* rt, const char* s);
size_t thrum_utf8_count(const char* s, size_t nbytes);
size_t thrum_utf8_cp_len(unsigned char lead);
size_t thrum_utf8_byte_off(const TStr* s, size_t char_idx);
TVal thrum_list(TRuntime* rt, size_t cap);
void thrum_list_push(TRuntime* rt, TList* lst, TVal v);
TVal thrum_map(TRuntime* rt);
void thrum_map_set(TRuntime* rt, TMap* m, TVal k, TVal v);
void thrum_map_set_cs(TRuntime* rt, TMap* m, const char* k, TVal v);
bool thrum_map_get(TMap* m, TVal k, TVal* out);
bool thrum_map_has(TMap* m, TVal k);

const char* thrum_tname(TVal v);
char* thrum_vstr_alloc(TRuntime* rt, TVal v, bool quote);
TVal thrum_vstr_val(TRuntime* rt, TVal v, bool quote);
bool thrum_veq(TVal a, TVal b);
bool thrum_truthy(TVal v);
double thrum_relchg(TVal a, TVal b);
double thrum_need_num(TRuntime* rt, TVal v, int line, int col);
int64_t thrum_need_int(TRuntime* rt, TVal v, int line, int col);
TQ thrum_need_q(TRuntime* rt, TVal v, int line, int col);

/* ---- Units & Quantity Arithmetic ---- */
TDim thrum_d0(void);
TDim thrum_dim(int m, int kg, int s, int A, int K, int mol, int cd);
bool thrum_dimless(TDim d);
bool thrum_dim_eq(TDim a, TDim b);
TDim thrum_dadd(TDim a, TDim b);
TDim thrum_dsub(TDim a, TDim b);
TDim thrum_dscale(TDim a, TFrac n);
TFrac thrum_frac(int64_t num, int64_t den);
TFrac thrum_frac_approx(double x, int64_t max_den);
void thrum_dimstr(TDim d, char* buf, size_t bufsz);
void thrum_qfmt(TQ x, char* buf, size_t bufsz);
bool thrum_scan_unit(const char* s, int i, double* out_sc, TDim* out_d, int* out_end);
void thrum_parse_unit(TRuntime* rt, const char* text, int line, int col, double* out_sc, TDim* out_d);
bool thrum_unit_lookup(const char* sym, double* out_sc, TDim* out_d);
void thrum_unit_define(TRuntime* rt, const char* sym, double sc, TDim d);
TVal thrum_units_list(TRuntime* rt);

TVal thrum_add(TRuntime* rt, TVal a, TVal b, int line, int col);
TVal thrum_sub(TRuntime* rt, TVal a, TVal b, int line, int col);
TVal thrum_mul(TRuntime* rt, TVal a, TVal b, int line, int col);
TVal thrum_div(TRuntime* rt, TVal a, TVal b, int line, int col);
TVal thrum_mod(TRuntime* rt, TVal a, TVal b, int line, int col);
TVal thrum_pow(TRuntime* rt, TVal a, TVal b, int line, int col);
TVal thrum_neg(TRuntime* rt, TVal a, int line, int col);
bool thrum_cmp(TRuntime* rt, TVal a, TVal b, int op, int line, int col); /* 0:< 1:> 2:<= 3:>= */
bool thrum_approx(TVal a, TVal b, double k);
bool thrum_in(TRuntime* rt, TVal a, TVal b, int line, int col);
TVal thrum_range_op(TRuntime* rt, TVal a, TVal b, bool excl, int line, int col);
TVal thrum_convert(TRuntime* rt, TVal a, const char* unit, int line, int col);
TVal thrum_interp_text(TRuntime* rt, int nparts, TVal* parts);

/* ---- Records, Indexing, Slicing, Pattern Matching ---- */
TVal thrum_def_record(TRuntime* rt, TEnv* env, const char* name, int nfields, TFieldSpec* fields);
TVal thrum_make_record_pos(TRuntime* rt, TRecType* rt_type, int argc, TVal* argv, int line, int col);
TVal thrum_make_record_map(TRuntime* rt, TVal target, TMap* provided, int line, int col);
TVal thrum_get_index(TRuntime* rt, TVal obj, TVal idx, int line, int col);
TVal thrum_get_slice(TRuntime* rt, TVal obj, bool has_start, TVal start, bool has_stop, TVal stop, bool has_step, TVal step, int line, int col);
TVal thrum_get_dot(TRuntime* rt, TVal obj, const char* name, int line, int col);
void thrum_check_type(TRuntime* rt, TVal v, TTypeAnn t, const char* what, int line, int col);

/* ---- Functions & Calls ---- */
TVal thrum_make_fn(TRuntime* rt, TEnv* env, const char* name, TFnPtr ptr, int nparams, TParamSpec* params, TTypeAnn ret, int nreq, TContractSpec* reqs, int nens, TContractSpec* ens);
TVal thrum_call(TRuntime* rt, TVal callee, int argc, TVal* argv, int line, int col);

/* ---- Reactive Graph, Scopes, Provenance ---- */
TEnv* thrum_env_new(TRuntime* rt, TEnv* parent, bool rel);
TVal thrum_read(TRuntime* rt, TEnv* env, const char* name, int line, int col);
void thrum_def_var(TRuntime* rt, TEnv* env, const char* name, TVal val, int line, int col);
void thrum_def_pin(TRuntime* rt, TEnv* env, const char* name, TVal val);
void thrum_def_let(TRuntime* rt, TEnv* env, const char* name, TExprFn expr, const char* src, int line, int col, bool has_init, TExprFn init_fn);
void thrum_assign(TRuntime* rt, TEnv* env, const char* root_name, int npath, bool* is_idx, TVal* path_vals, int op, TVal rhs, int line, int col);
/* op: 0:= 1:+= 2:-= 3:*= 4:/= 5:^= 6:%= */

void thrum_settle_begin(TRuntime* rt, TEnv* env, int maxit, bool has_tol, TExprFn tol_fn, int line, int col);
void thrum_settle_end(TRuntime* rt);
void thrum_on(TRuntime* rt, TEnv* env, TExprFn guard, const char* src, TReactMode mode, TExprFn body, int line, int col);
void thrum_drain(TRuntime* rt);
void thrum_why(TRuntime* rt, TEnv* env, const char* name, int line, int col);
void thrum_budget(TRuntime* rt, TEnv* env, const char* name, int line, int col);
void thrum_whatif(TRuntime* rt, TEnv* env, int npairs, const char** names, TVal* vals, TExprFn body, int line, int col);

/* ---- Capabilities, Modules, Journal, JSON ---- */
TVal thrum_make_builtin(TRuntime* rt, const char* name, TBuiltinPtr fn, int min_arity, int max_arity);
TVal thrum_make_builtin_ctx(TRuntime* rt, const char* name, TBuiltinCtxPtr fn, void* ctx, TObj* owner, int min_arity, int max_arity);
void thrum_install_builtins(TRuntime* rt);
TCap* thrum_alloc_cap(TRuntime* rt, const char* name, TCapCleanup cleanup, void (*free_state)(void*), void* state);
void thrum_cap_add_method(TRuntime* rt, TCap* cap, const char* mname, TBuiltinCtxPtr fn, int min_a, int max_a);
TVal thrum_cap_gui_create(TRuntime* rt, int line, int col);
TVal thrum_get_cap(TRuntime* rt, const char* name, int argc, TVal* argv, int line, int col);
TVal thrum_get_stdmod(TRuntime* rt, const char* name, int line, int col);
void thrum_reg_loading(TRuntime* rt, const char* key);
void thrum_reg_module(TRuntime* rt, const char* key, TEnv* scope);
TVal thrum_get_module(TRuntime* rt, const char* key);
void thrum_journal_save(TRuntime* rt);
void thrum_journal_load(TRuntime* rt, const char* path);
TVal thrum_journal_get(TRuntime* rt, const char* kind, const char* key, TVal (*produce)(TRuntime*, void*), void* ctx);
TVal thrum_json_parse_str(TRuntime* rt, const char* s, size_t len, bool decode_qty, int line, int col);
char* thrum_json_dump_alloc(TRuntime* rt, TVal v, bool qty_mode, int indent, int line, int col);

#endif /* THRUM_RT_H */
