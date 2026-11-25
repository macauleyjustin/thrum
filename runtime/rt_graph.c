#include "thrum_rt.h"

/* ============================== Scopes & Cells ============================== */
TEnv* thrum_env_new(TRuntime* rt, TEnv* parent, bool rel) {
    T_FRAME(rt);
    TVal pv = parent ? thrum_obj((TObj*)parent) : thrum_nil();
    thrum_push_root(rt, &pv);
    TEnv* e = (TEnv*)thrum_alloc_obj(rt, OBJ_ENV, sizeof(TEnv));
    e->parent = parent;
    e->rel = rel;
    e->len = 0;
    e->cap = 0;
    e->slots = NULL;
    T_END_FRAME(rt);
    return e;
}

static bool env_find(TEnv* e, const char* name, TEnv** out_env, TEnvSlot** out_slot) {
    TEnv* cur = e;
    while (cur) {
        for (size_t i = 0; i < cur->len; i++) {
            if (cur->slots[i].name == name || strcmp(cur->slots[i].name, name) == 0) {
                if (out_env) *out_env = cur;
                if (out_slot) *out_slot = &cur->slots[i];
                return true;
            }
        }
        cur = cur->parent;
    }
    return false;
}

static void cell_del_sub(TCell* parent, TCell* sub) {
    for (int i = 0; i < parent->nsubs; i++) {
        if (parent->subs[i] == sub) {
            parent->subs[i] = parent->subs[parent->nsubs - 1];
            parent->nsubs--;
            return;
        }
    }
}

static void cell_add_sub(TCell* parent, TCell* sub) {
    for (int i = 0; i < parent->nsubs; i++) {
        if (parent->subs[i] == sub) return;
    }
    if (parent->nsubs >= parent->subs_cap) {
        parent->subs_cap = parent->subs_cap ? parent->subs_cap * 2 : 4;
        parent->subs = (TCell**)realloc(parent->subs, (size_t)parent->subs_cap * sizeof(TCell*));
    }
    parent->subs[parent->nsubs++] = sub;
}

static void cell_add_newdep(TCell* c, TCell* dep) {
    for (int i = 0; i < c->nnewdeps; i++) {
        if (c->newdeps[i] == dep) return;
    }
    if (c->nnewdeps >= c->newdeps_cap) {
        c->newdeps_cap = c->newdeps_cap ? c->newdeps_cap * 2 : 4;
        c->newdeps = (TCell**)realloc(c->newdeps, (size_t)c->newdeps_cap * sizeof(TCell*));
    }
    c->newdeps[c->nnewdeps++] = dep;
}

static bool cell_in_list(TCell** arr, int n, TCell* target) {
    for (int i = 0; i < n; i++) if (arr[i] == target) return true;
    return false;
}

static void unbind_old_val(TVal old) {
    if (old.kind == TV_OBJ && old.as.obj && old.as.obj->kind == OBJ_CELL) {
        TCell* c = (TCell*)old.as.obj;
        for (int i = 0; i < c->ndeps; i++) {
            cell_del_sub(c->deps[i], c);
        }
        c->ndeps = 0;
        c->nsubs = 0;
        c->dead = true;
        if (c->group) {
            TGroup* g = c->group;
            for (int i = 0; i < g->ncells; i++) {
                if (g->cells[i] == c) {
                    for (int j = i + 1; j < g->ncells; j++) g->cells[j - 1] = g->cells[j];
                    g->ncells--;
                    break;
                }
            }
        }
    }
}

static void env_bind(TRuntime* rt, TEnv* e, const char* name, TVal val, bool is_const) {
    T_FRAME(rt);
    T_VAR(rt, ev, thrum_obj((TObj*)e));
    T_VAR(rt, vv, val);
    const char* iname = thrum_intern(rt, name);
    for (size_t i = 0; i < e->len; i++) {
        if (e->slots[i].name == iname || strcmp(e->slots[i].name, iname) == 0) {
            unbind_old_val(e->slots[i].val);
            e->slots[i].val = val;
            e->slots[i].is_const = is_const;
            T_END_FRAME(rt);
            return;
        }
    }
    if (e->len >= e->cap) {
        e->cap = e->cap ? e->cap * 2 : 8;
        e->slots = (TEnvSlot*)realloc(e->slots, e->cap * sizeof(TEnvSlot));
    }
    e->slots[e->len].name = iname;
    e->slots[e->len].val = val;
    e->slots[e->len].is_const = is_const;
    e->len++;
    T_END_FRAME(rt);
}

static TCell* cell_new(TRuntime* rt, const char* name, TCellKind ckind, TVal val,
                       TExprFn expr, TEnv* env, int line, int col, const char* src,
                       TGroup* group, bool has_init) {
    T_FRAME(rt);
    T_VAR(rt, vv, val);
    T_VAR(rt, ev, env ? thrum_obj((TObj*)env) : thrum_nil());
    T_VAR(rt, gv, group ? thrum_obj((TObj*)group) : thrum_nil());
    const char* iname = thrum_intern(rt, name);
    const char* isrc = src ? thrum_intern(rt, src) : NULL;
    TCell* c = (TCell*)thrum_alloc_obj(rt, OBJ_CELL, sizeof(TCell));
    c->id = rt->next_cell_id++;
    c->born = c->id;
    c->name = iname;
    c->ckind = ckind;
    c->value = val;
    c->expr = expr;
    c->env = env;
    c->line = line;
    c->col = col;
    c->src = isrc;
    c->group = group;
    c->has_init = has_init;
    T_END_FRAME(rt);
    return c;
}

static TVal read_cell(TRuntime* rt, TCell* c) {
    if (rt->overrides) {
        for (int i = 0; i < rt->n_overrides; i++) {
            if (rt->overrides[i].cell == c) return rt->overrides[i].val;
        }
    }
    if (rt->track_top > 0 && rt->frames_top > 0 && c->born < rt->frames[0]) {
        TCell* t = rt->track[rt->track_top - 1];
        if (t != c) cell_add_newdep(t, c);
    }
    return c->value;
}

TVal thrum_read(TRuntime* rt, TEnv* env, const char* name, int line, int col) {
    TEnv* found_env = NULL;
    TEnvSlot* slot = NULL;
    if (!env_find(env, name, &found_env, &slot)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "undefined name '%s'", name);
        thrum_error(rt, line, col, msg, NULL);
    }
    if (slot->val.kind == TV_OBJ && slot->val.as.obj && slot->val.as.obj->kind == OBJ_CELL) {
        return read_cell(rt, (TCell*)slot->val.as.obj);
    }
    return slot->val;
}

/* ============================== Dependency Graph & Propagation ============================== */
static void track_push(TRuntime* rt, TCell* c) {
    if (rt->track_top >= rt->track_cap) {
        rt->track_cap = rt->track_cap ? rt->track_cap * 2 : 32;
        rt->track = (TCell**)realloc(rt->track, (size_t)rt->track_cap * sizeof(TCell*));
    }
    rt->track[rt->track_top++] = c;
}

static void frames_push(TRuntime* rt, int id) {
    if (rt->frames_top >= rt->frames_cap) {
        rt->frames_cap = rt->frames_cap ? rt->frames_cap * 2 : 32;
        rt->frames = (int*)realloc(rt->frames, (size_t)rt->frames_cap * sizeof(int));
    }
    rt->frames[rt->frames_top++] = id;
}

static bool has_cycle(TCell* c, TCell** deps, int ndeps) {
    int cap = ndeps > 16 ? ndeps * 2 : 32;
    int sp = 0, seen_n = 0, seen_cap = 64;
    TCell** stack = (TCell**)malloc((size_t)cap * sizeof(TCell*));
    TCell** seen = (TCell**)malloc((size_t)seen_cap * sizeof(TCell*));
    for (int i = 0; i < ndeps; i++) stack[sp++] = deps[i];
    bool found = false;
    while (sp > 0) {
        TCell* x = stack[--sp];
        if (x == c) { found = true; break; }
        if (cell_in_list(seen, seen_n, x)) continue;
        if (seen_n >= seen_cap) {
            seen_cap *= 2;
            seen = (TCell**)realloc(seen, (size_t)seen_cap * sizeof(TCell*));
        }
        seen[seen_n++] = x;
        if (sp + x->ndeps > cap) {
            while (cap < sp + x->ndeps) cap *= 2;
            stack = (TCell**)realloc(stack, (size_t)cap * sizeof(TCell*));
        }
        for (int i = 0; i < x->ndeps; i++) stack[sp++] = x->deps[i];
    }
    free(stack);
    free(seen);
    return found;
}

static void bump_height(TCell* c, TCell*** seen, int* seen_n, int* seen_cap) {
    if (cell_in_list(*seen, *seen_n, c)) return;
    if (*seen_n >= *seen_cap) {
        *seen_cap = *seen_cap ? (*seen_cap) * 2 : 64;
        *seen = (TCell**)realloc(*seen, (size_t)(*seen_cap) * sizeof(TCell*));
    }
    (*seen)[(*seen_n)++] = c;
    if (c->group) {
        int gh = 0;
        TGroup* g = c->group;
        for (int i = 0; i < g->ncells; i++) {
            TCell* gc = g->cells[i];
            for (int j = 0; j < gc->ndeps; j++) {
                if (gc->deps[j]->group != g) {
                    int h = 1 + gc->deps[j]->height;
                    if (h > gh) gh = h;
                }
            }
        }
        for (int i = 0; i < g->ncells; i++) {
            TCell* gc = g->cells[i];
            if (gh != gc->height) {
                gc->height = gh;
                for (int j = 0; j < gc->nsubs; j++) {
                    if (gc->subs[j]->group != g) bump_height(gc->subs[j], seen, seen_n, seen_cap);
                }
            }
        }
    } else {
        int nh = 0;
        for (int i = 0; i < c->ndeps; i++) {
            int h = 1 + c->deps[i]->height;
            if (h > nh) nh = h;
        }
        if (nh != c->height) {
            c->height = nh;
            for (int i = 0; i < c->nsubs; i++) {
                bump_height(c->subs[i], seen, seen_n, seen_cap);
            }
        }
    }
}

static void link_cell(TRuntime* rt, TCell* c, bool allow_cycle) {
    if (has_cycle(c, c->newdeps, c->nnewdeps) && !allow_cycle) {
        char msg[160];
        snprintf(msg, sizeof(msg), "cyclic relation involving '%s'", c->name);
        thrum_error(rt, c->line, c->col, msg,
                    "wrap mutually-recursive relations in a 'settle' block with 'init' seeds");
    }
    for (int i = 0; i < c->ndeps; i++) {
        TCell* d = c->deps[i];
        if (!cell_in_list(c->newdeps, c->nnewdeps, d)) cell_del_sub(d, c);
    }
    for (int i = 0; i < c->nnewdeps; i++) {
        TCell* d = c->newdeps[i];
        if (!cell_in_list(c->deps, c->ndeps, d)) cell_add_sub(d, c);
    }
    if (c->deps_cap < c->nnewdeps) {
        c->deps_cap = c->nnewdeps;
        c->deps = (TCell**)realloc(c->deps, (size_t)c->deps_cap * sizeof(TCell*));
    }
    if (c->nnewdeps > 0) memcpy(c->deps, c->newdeps, (size_t)c->nnewdeps * sizeof(TCell*));
    c->ndeps = c->nnewdeps;
    TCell** seen = NULL;
    int seen_n = 0, seen_cap = 0;
    bump_height(c, &seen, &seen_n, &seen_cap);
    free(seen);
}

static TVal recompute_cell(TRuntime* rt, TCell* c) {
    T_FRAME(rt);
    T_VAR(rt, cv, thrum_obj((TObj*)c));
    track_push(rt, c);
    frames_push(rt, rt->next_cell_id);
    c->nnewdeps = 0;
    T_VAR(rt, v, c->expr(rt, c->env));
    rt->track_top--;
    rt->frames_top--;
    link_cell(rt, c, c->group != NULL);
    if (rt->trace && !thrum_veq(c->value, v)) {
        char* vs = thrum_vstr_alloc(rt, v, false);
        fprintf(rt->out ? rt->out : stdout, "  [live] %s -> %s\n", c->name, vs);
        free(vs);
    }
    c->value = v;
    c->eval_id++;
    T_END_FRAME(rt);
    return v;
}

static int group_height(TGroup* g) {
    int mx = 0;
    for (int i = 0; i < g->ncells; i++) {
        if (g->cells[i]->height > mx) mx = g->cells[i]->height;
    }
    return mx;
}

static void relax_group(TRuntime* rt, TGroup* g, TCell*** out_changed, int* out_n) {
    *out_changed = NULL;
    *out_n = 0;
    if (g->ncells == 0) return;
    T_FRAME(rt);
    T_VAR(rt, gv, thrum_obj((TObj*)g));
    int n = g->ncells;
    TVal* before = (TVal*)malloc((size_t)n * sizeof(TVal));
    for (int i = 0; i < n; i++) {
        before[i] = g->cells[i]->value;
        thrum_push_root(rt, &before[i]);
    }
    TCell** order = (TCell**)malloc((size_t)n * sizeof(TCell*));
    int oi = 0;
    for (int i = 0; i < n; i++) if (!g->cells[i]->has_init) order[oi++] = g->cells[i];
    for (int i = 0; i < n; i++) if (g->cells[i]->has_init) order[oi++] = g->cells[i];

    for (int it = 0; it < g->maxit; it++) {
        double delta = 0.0;
        bool moved = false;
        for (int i = 0; i < n; i++) {
            TCell* c = order[i];
            T_VAR(rt, old, c->value);
            track_push(rt, c);
            frames_push(rt, rt->next_cell_id);
            c->nnewdeps = 0;
            T_VAR(rt, v, c->expr(rt, c->env));
            rt->track_top--;
            rt->frames_top--;
            link_cell(rt, c, true);
            c->value = v;
            c->eval_id++;
            if (!thrum_veq(old, v)) {
                moved = true;
                c->version++;
                double rc = thrum_relchg(old, v);
                if (rc > delta) delta = rc;
            }
        }
        if (!moved || delta <= g->tol) {
            TCell** ch = (TCell**)malloc((size_t)n * sizeof(TCell*));
            int cn = 0;
            for (int i = 0; i < n; i++) {
                if (!thrum_veq(before[i], g->cells[i]->value)) ch[cn++] = g->cells[i];
            }
            *out_changed = ch;
            *out_n = cn;
            free(before);
            free(order);
            T_END_FRAME(rt);
            return;
        }
    }
    free(before);
    free(order);
    char msg[128];
    snprintf(msg, sizeof(msg), "settle did not converge in %d iterations", g->maxit);
    thrum_error(rt, g->line, g->col, msg,
                "increase 'max', loosen 'tol', or check that the relations are contractive");
}

static void propagate_seeds(TRuntime* rt, TCell** seeds, int nseeds) {
    TCell** pending = NULL;
    int npend = 0, cpend = 0;
    TGroup** groups = NULL;
    int ngroups = 0, cgroups = 0;

    for (int i = 0; i < nseeds; i++) {
        TCell* s = seeds[i];
        for (int j = 0; j < s->nsubs; j++) {
            TCell* sub = s->subs[j];
            if (sub->dead) continue;
            if (sub->group) {
                bool seen = false;
                for (int k = 0; k < ngroups; k++) if (groups[k] == sub->group) { seen = true; break; }
                if (!seen) {
                    if (ngroups >= cgroups) {
                        cgroups = cgroups ? cgroups * 2 : 8;
                        groups = (TGroup**)realloc(groups, (size_t)cgroups * sizeof(TGroup*));
                    }
                    groups[ngroups++] = sub->group;
                }
            } else {
                if (!cell_in_list(pending, npend, sub)) {
                    if (npend >= cpend) {
                        cpend = cpend ? cpend * 2 : 16;
                        pending = (TCell**)realloc(pending, (size_t)cpend * sizeof(TCell*));
                    }
                    pending[npend++] = sub;
                }
            }
        }
    }

    int steps = 0;
    while (npend > 0 || ngroups > 0) {
        int best_c = -1;
        for (int i = 0; i < npend; i++) {
            if (best_c < 0 || pending[i]->height < pending[best_c]->height ||
                (pending[i]->height == pending[best_c]->height && pending[i]->id < pending[best_c]->id)) {
                best_c = i;
            }
        }
        int best_g = -1, best_gh = 0;
        for (int i = 0; i < ngroups; i++) {
            int gh = group_height(groups[i]);
            if (best_g < 0 || gh < best_gh) {
                best_g = i;
                best_gh = gh;
            }
        }
        if (best_c >= 0 && (best_g < 0 || pending[best_c]->height <= best_gh)) {
            TCell* c = pending[best_c];
            pending[best_c] = pending[--npend];
            T_FRAME(rt);
            T_VAR(rt, old, c->value);
            T_VAR(rt, nv, recompute_cell(rt, c));
            if (!thrum_veq(old, nv)) {
                c->version++;
                for (int j = 0; j < c->nsubs; j++) {
                    TCell* sub = c->subs[j];
                    if (sub->dead) continue;
                    if (sub->group) {
                        bool seen = false;
                        for (int k = 0; k < ngroups; k++) if (groups[k] == sub->group) { seen = true; break; }
                        if (!seen) {
                            if (ngroups >= cgroups) {
                                cgroups = cgroups ? cgroups * 2 : 8;
                                groups = (TGroup**)realloc(groups, (size_t)cgroups * sizeof(TGroup*));
                            }
                            groups[ngroups++] = sub->group;
                        }
                    } else if (!cell_in_list(pending, npend, sub)) {
                        if (npend >= cpend) {
                            cpend = cpend ? cpend * 2 : 16;
                            pending = (TCell**)realloc(pending, (size_t)cpend * sizeof(TCell*));
                        }
                        pending[npend++] = sub;
                    }
                }
            }
            T_END_FRAME(rt);
        } else {
            TGroup* g = groups[best_g];
            groups[best_g] = groups[--ngroups];
            TCell** changed = NULL;
            int nchanged = 0;
            relax_group(rt, g, &changed, &nchanged);
            for (int i = 0; i < nchanged; i++) {
                TCell* c = changed[i];
                for (int j = 0; j < c->nsubs; j++) {
                    TCell* sub = c->subs[j];
                    if (sub->dead || sub->group == g) continue;
                    if (sub->group) {
                        bool seen = false;
                        for (int k = 0; k < ngroups; k++) if (groups[k] == sub->group) { seen = true; break; }
                        if (!seen) {
                            if (ngroups >= cgroups) {
                                cgroups = cgroups ? cgroups * 2 : 8;
                                groups = (TGroup**)realloc(groups, (size_t)cgroups * sizeof(TGroup*));
                            }
                            groups[ngroups++] = sub->group;
                        }
                    } else if (!cell_in_list(pending, npend, sub)) {
                        if (npend >= cpend) {
                            cpend = cpend ? cpend * 2 : 16;
                            pending = (TCell**)realloc(pending, (size_t)cpend * sizeof(TCell*));
                        }
                        pending[npend++] = sub;
                    }
                }
            }
            free(changed);
        }
        steps++;
        if (steps > 200000) {
            free(pending);
            free(groups);
            thrum_error(rt, 0, 0, "relation network did not settle", NULL);
        }
    }
    free(pending);
    free(groups);
}

static void fire_reactions(TRuntime* rt) {
    for (int i = 0; i < rt->n_reactions; i++) {
        TReaction* r = &rt->reactions[i];
        bool cur = thrum_truthy(r->guard->value);
        bool go = false;
        if (r->mode == REACT_RISE) go = cur && !r->prev;
        else if (r->mode == REACT_FALL) go = !cur && r->prev;
        else go = cur && (r->guard->eval_id != r->seen);
        r->prev = cur;
        r->seen = r->guard->eval_id;
        if (go) {
            T_FRAME(rt);
            TEnv* sub = thrum_env_new(rt, r->env, false);
            T_VAR(rt, sv, thrum_obj((TObj*)sub));
            (void)r->body(rt, sub);
            T_END_FRAME(rt);
        }
    }
}

void thrum_drain(TRuntime* rt) {
    if (!rt || rt->draining) return;
    rt->draining = true;
    int rounds = 0;
    for (;;) {
        if (rt->n_queue > 0) {
            int nseeds = rt->n_queue;
            TCell** seeds = (TCell**)malloc((size_t)nseeds * sizeof(TCell*));
            memcpy(seeds, rt->queue, (size_t)nseeds * sizeof(TCell*));
            rt->n_queue = 0;
            propagate_seeds(rt, seeds, nseeds);
            free(seeds);
        }
        fire_reactions(rt);
        if (rt->n_queue == 0) break;
        rounds++;
        if (rounds > 1000) {
            rt->draining = false;
            thrum_error(rt, 0, 0, "reaction cascade exceeded 1000 rounds",
                        "a reaction is probably feeding its own input");
        }
    }
    rt->draining = false;
}

static void notify_cell(TRuntime* rt, TCell* c) {
    c->version++;
    if (rt->n_queue >= rt->cap_queue) {
        rt->cap_queue = rt->cap_queue ? rt->cap_queue * 2 : 16;
        rt->queue = (TCell**)realloc(rt->queue, (size_t)rt->cap_queue * sizeof(TCell*));
    }
    rt->queue[rt->n_queue++] = c;
    if (!rt->draining) thrum_drain(rt);
}

/* ============================== Declarations & Assignment ============================== */
void thrum_def_var(TRuntime* rt, TEnv* env, const char* name, TVal val, int line, int col) {
    T_FRAME(rt);
    T_VAR(rt, vv, val);
    if (env->rel) {
        TCell* c = cell_new(rt, name, CELL_SOURCE, val, NULL, env, line, col, NULL, NULL, false);
        env_bind(rt, env, name, thrum_obj((TObj*)c), false);
    } else {
        env_bind(rt, env, name, val, false);
    }
    T_END_FRAME(rt);
}

void thrum_def_pin(TRuntime* rt, TEnv* env, const char* name, TVal val) {
    env_bind(rt, env, name, val, true);
}

void thrum_def_let(TRuntime* rt, TEnv* env, const char* name, TExprFn expr, const char* src,
                   int line, int col, bool has_init, TExprFn init_fn) {
    if (env->rel) {
        TCell* c = cell_new(rt, name, CELL_DERIVED, thrum_nil(), expr, env, line, col, src,
                            rt->curgroup, has_init);
        T_FRAME(rt);
        T_VAR(rt, cv, thrum_obj((TObj*)c));
        env_bind(rt, env, name, cv, false);
        if (rt->curgroup) {
            c->value = has_init ? init_fn(rt, env) : thrum_nil();
            if (rt->curgroup->ncells >= rt->curgroup->cap) {
                rt->curgroup->cap = rt->curgroup->cap ? rt->curgroup->cap * 2 : 8;
                rt->curgroup->cells = (TCell**)realloc(rt->curgroup->cells,
                                                       (size_t)rt->curgroup->cap * sizeof(TCell*));
            }
            rt->curgroup->cells[rt->curgroup->ncells++] = c;
        } else {
            recompute_cell(rt, c);
        }
        T_END_FRAME(rt);
    } else {
        T_FRAME(rt);
        T_VAR(rt, v, expr(rt, env));
        thrum_def_pin(rt, env, name, v);
        T_END_FRAME(rt);
    }
}

static TVal getpath(TRuntime* rt, TVal v, int npath, bool* is_idx, TVal* path_vals, int idx0, int line, int col) {
    for (int i = idx0; i < npath; i++) {
        if (is_idx[i]) {
            v = thrum_get_index(rt, v, path_vals[i], line, col);
        } else {
            if (path_vals[i].kind != TV_OBJ || path_vals[i].as.obj->kind != OBJ_STR) {
                thrum_error(rt, line, col, "field name must be text", NULL);
            }
            const char* fn = ((TStr*)path_vals[i].as.obj)->data;
            if (v.kind == TV_OBJ && v.as.obj->kind == OBJ_RECVAL) {
                v = thrum_get_dot(rt, v, fn, line, col);
            } else if (v.kind == TV_OBJ && v.as.obj->kind == OBJ_MAP) {
                TVal out = thrum_nil();
                thrum_map_get((TMap*)v.as.obj, path_vals[i], &out);
                v = out;
            } else {
                char msg[96];
                snprintf(msg, sizeof(msg), "cannot descend into %s", thrum_tname(v));
                thrum_error(rt, line, col, msg, NULL);
            }
        }
    }
    return v;
}

static TVal setpath(TRuntime* rt, TVal v, int npath, bool* is_idx, TVal* path_vals, int i, TVal newv, int line, int col) {
    if (i >= npath) return newv;
    T_FRAME(rt);
    T_VAR(rt, vv, v);
    T_VAR(rt, nv, newv);
    if (vv.kind != TV_OBJ) {
        char msg[96];
        snprintf(msg, sizeof(msg), "cannot assign into %s", thrum_tname(vv));
        thrum_error(rt, line, col, msg, NULL);
    }
    TObj* o = vv.as.obj;
    if (o->kind == OBJ_LIST) {
        if (!is_idx[i]) thrum_error(rt, line, col, "cannot assign field on list", NULL);
        TList* l = (TList*)o;
        int64_t ix = thrum_need_int(rt, path_vals[i], line, col);
        if (ix < 0) ix += (int64_t)l->len;
        if (ix < 0 || (size_t)ix >= l->len) {
            char msg[96];
            snprintf(msg, sizeof(msg), "index %lld out of range (len %zu)", (long long)ix, l->len);
            thrum_error(rt, line, col, msg, NULL);
        }
        T_VAR(rt, sub, setpath(rt, l->items[ix], npath, is_idx, path_vals, i + 1, nv, line, col));
        T_VAR(rt, out, thrum_list(rt, l->len));
        TList* nl = (TList*)out.as.obj;
        for (size_t k = 0; k < l->len; k++) nl->items[k] = l->items[k];
        nl->len = l->len;
        nl->items[ix] = sub;
        T_END_FRAME(rt);
        return out;
    }
    if (o->kind == OBJ_MAP) {
        TVal key = path_vals[i];
        if (key.kind == TV_OBJ && (key.as.obj->kind == OBJ_LIST || key.as.obj->kind == OBJ_MAP)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "map keys cannot be %s", thrum_tname(key));
            thrum_error(rt, line, col, msg, NULL);
        }
        TMap* m = (TMap*)o;
        TVal cur = thrum_nil();
        thrum_map_get(m, key, &cur);
        T_VAR(rt, sub, setpath(rt, cur, npath, is_idx, path_vals, i + 1, nv, line, col));
        T_VAR(rt, out, thrum_map(rt));
        TMap* nm = (TMap*)out.as.obj;
        for (size_t k = 0; k < m->order_len; k++) {
            TVal ek = m->order_keys[k];
            TVal ev;
            if (thrum_map_get(m, ek, &ev)) thrum_map_set(rt, nm, ek, ev);
        }
        thrum_map_set(rt, nm, key, sub);
        T_END_FRAME(rt);
        return out;
    }
    if (o->kind == OBJ_RECVAL) {
        if (is_idx[i] || path_vals[i].kind != TV_OBJ || path_vals[i].as.obj->kind != OBJ_STR) {
            char* ks = thrum_vstr_alloc(rt, path_vals[i], false);
            TRecVal* rv = (TRecVal*)o;
            char msg[160];
            snprintf(msg, sizeof(msg), "record '%s' has no field '%s'", rv->rtype->name, ks);
            free(ks);
            thrum_error(rt, line, col, msg, NULL);
        }
        TRecVal* rv = (TRecVal*)o;
        const char* fn = ((TStr*)path_vals[i].as.obj)->data;
        int fi = -1;
        for (int f = 0; f < rv->rtype->nfields; f++) {
            if (strcmp(rv->rtype->fields[f].name, fn) == 0) { fi = f; break; }
        }
        if (fi < 0) {
            char msg[160];
            snprintf(msg, sizeof(msg), "record '%s' has no field '%s'", rv->rtype->name, fn);
            thrum_error(rt, line, col, msg, NULL);
        }
        T_VAR(rt, sub, setpath(rt, rv->fields[fi], npath, is_idx, path_vals, i + 1, nv, line, col));
        if (i + 1 >= npath) {
            char what[160];
            snprintf(what, sizeof(what), "field '%s' of %s", fn, rv->rtype->name);
            thrum_check_type(rt, sub, rv->rtype->fields[fi].type, what, line, col);
        }
        TRecVal* nrv = (TRecVal*)thrum_alloc_obj(rt, OBJ_RECVAL, sizeof(TRecVal));
        nrv->rtype = rv->rtype;
        nrv->fields = (TVal*)calloc((size_t)rv->rtype->nfields, sizeof(TVal));
        memcpy(nrv->fields, rv->fields, (size_t)rv->rtype->nfields * sizeof(TVal));
        nrv->fields[fi] = sub;
        T_END_FRAME(rt);
        return thrum_obj((TObj*)nrv);
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "cannot assign into %s", thrum_tname(vv));
    thrum_error(rt, line, col, msg, NULL);
}

static TVal apply_assign_op(TRuntime* rt, int op, TVal old, TVal rhs, int line, int col) {
    switch (op) {
        case 1: return thrum_add(rt, old, rhs, line, col);
        case 2: return thrum_sub(rt, old, rhs, line, col);
        case 3: return thrum_mul(rt, old, rhs, line, col);
        case 4: return thrum_div(rt, old, rhs, line, col);
        case 5: return thrum_pow(rt, old, rhs, line, col);
        case 6: return thrum_mod(rt, old, rhs, line, col);
        default: return rhs;
    }
}

void thrum_assign(TRuntime* rt, TEnv* env, const char* root_name, int npath,
                  bool* is_idx, TVal* path_vals, int op, TVal rhs, int line, int col) {
    T_FRAME(rt);
    T_VAR(rt, rv, rhs);
    for (int i = 0; i < npath; i++) thrum_push_root(rt, &path_vals[i]);

    TEnv* s = NULL;
    TEnvSlot* slot = NULL;
    if (!env_find(env, root_name, &s, &slot)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "undefined name '%s'", root_name);
        thrum_error(rt, line, col, msg, "declare inputs with 'var'");
    }
    const char* target_name = root_name;
    TVal b = slot->val;
    int pidx = 0;
    while (pidx < npath && b.kind == TV_OBJ && b.as.obj->kind == OBJ_MOD) {
        if (is_idx[pidx] || path_vals[pidx].kind != TV_OBJ || path_vals[pidx].as.obj->kind != OBJ_STR) {
            thrum_error(rt, line, col, "module member must be a name", NULL);
        }
        TMod* mod = (TMod*)b.as.obj;
        const char* field = ((TStr*)path_vals[pidx].as.obj)->data;
        pidx++;
        if (!env_find(mod->scope, field, &s, &slot)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "module '%s' has no '%s'", mod->name, field);
            thrum_error(rt, line, col, msg, NULL);
        }
        target_name = field;
        b = slot->val;
    }
    if (slot->is_const) {
        char msg[160];
        snprintf(msg, sizeof(msg), "cannot assign to immutable '%s'", target_name);
        thrum_error(rt, line, col, msg, "declare it with 'var' if it needs to change");
    }
    bool is_cell = (b.kind == TV_OBJ && b.as.obj->kind == OBJ_CELL);
    T_VAR(rt, base, is_cell ? read_cell(rt, (TCell*)b.as.obj) : b);
    T_VAR(rt, newv, rv);
    if (op != 0) {
        T_VAR(rt, oldv, (pidx < npath) ? getpath(rt, base, npath, is_idx, path_vals, pidx, line, col) : base);
        newv = apply_assign_op(rt, op, oldv, rv, line, col);
    }
    T_VAR(rt, root, (pidx < npath) ? setpath(rt, base, npath, is_idx, path_vals, pidx, newv, line, col) : newv);

    if (is_cell) {
        TCell* c = (TCell*)b.as.obj;
        if (c->ckind != CELL_SOURCE) {
            char msg[160];
            snprintf(msg, sizeof(msg), "'%s' is a derived relation, not an input", target_name);
            thrum_error(rt, line, col, msg, "assign to its inputs instead, or declare it with 'var'");
        }
        if (rt->frames_top > 0 && c->born < rt->frames[0]) {
            char msg[160];
            snprintf(msg, sizeof(msg), "cannot mutate '%s' inside a relation", c->name);
            thrum_error(rt, line, col, msg, "relations must be pure; change inputs from an 'on' reaction or top level");
        }
        if (!thrum_veq(c->value, root)) {
            c->value = root;
            notify_cell(rt, c);
        }
    } else {
        slot->val = root;
    }
    T_END_FRAME(rt);
}

/* ============================== Settle, On, Whatif, Why, Budget ============================== */
void thrum_settle_begin(TRuntime* rt, TEnv* env, int maxit, bool has_tol, TExprFn tol_fn, int line, int col) {
    if (!env->rel) {
        thrum_error(rt, line, col, "settle needs a relation scope", NULL);
    }
    double tol = 1e-12;
    if (has_tol && tol_fn) {
        T_FRAME(rt);
        T_VAR(rt, tv, tol_fn(rt, env));
        tol = thrum_need_q(rt, tv, line, col).v;
        T_END_FRAME(rt);
    }
    TGroup* g = (TGroup*)thrum_alloc_obj(rt, OBJ_GROUP, sizeof(TGroup));
    g->maxit = maxit;
    g->tol = tol;
    g->line = line;
    g->col = col;
    if (rt->n_groups >= rt->cap_groups) {
        rt->cap_groups = rt->cap_groups ? rt->cap_groups * 2 : 16;
        rt->groups = (TGroup**)realloc(rt->groups, (size_t)rt->cap_groups * sizeof(TGroup*));
    }
    rt->groups[rt->n_groups++] = g;
    rt->curgroup = g;
}

void thrum_settle_end(TRuntime* rt) {
    TGroup* g = rt->curgroup;
    rt->curgroup = NULL;
    if (g) {
        TCell** changed = NULL;
        int nchanged = 0;
        relax_group(rt, g, &changed, &nchanged);
        free(changed);
    }
}

void thrum_on(TRuntime* rt, TEnv* env, TExprFn guard, const char* src, TReactMode mode,
              TExprFn body, int line, int col) {
    TCell* g = cell_new(rt, "<guard>", CELL_DERIVED, thrum_nil(), guard, env, line, col, src, NULL, false);
    T_FRAME(rt);
    T_VAR(rt, gv, thrum_obj((TObj*)g));
    recompute_cell(rt, g);
    if (rt->n_reactions >= rt->cap_reactions) {
        rt->cap_reactions = rt->cap_reactions ? rt->cap_reactions * 2 : 16;
        rt->reactions = (TReaction*)realloc(rt->reactions, (size_t)rt->cap_reactions * sizeof(TReaction));
    }
    TReaction* r = &rt->reactions[rt->n_reactions++];
    r->guard = g;
    r->body = body;
    r->env = env;
    r->mode = mode;
    r->prev = false;
    r->seen = (uint64_t)-1;
    T_END_FRAME(rt);
}

static void shadow_compute(TRuntime* rt, int npairs, TCell** seed_cells, TVal* seed_vals,
                           TOverride** out_ov, int* out_n) {
    int cap = npairs + 16;
    int n = 0;
    TOverride* ov = (TOverride*)malloc((size_t)cap * sizeof(TOverride));
    if (rt->overrides) {
        for (int i = 0; i < rt->n_overrides; i++) {
            if (n >= cap) { cap *= 2; ov = (TOverride*)realloc(ov, (size_t)cap * sizeof(TOverride)); }
            ov[n++] = rt->overrides[i];
        }
    }
    for (int i = 0; i < npairs; i++) {
        bool upd = false;
        for (int k = 0; k < n; k++) {
            if (ov[k].cell == seed_cells[i]) { ov[k].val = seed_vals[i]; upd = true; break; }
        }
        if (!upd) {
            if (n >= cap) { cap *= 2; ov = (TOverride*)realloc(ov, (size_t)cap * sizeof(TOverride)); }
            ov[n].cell = seed_cells[i];
            ov[n].val = seed_vals[i];
            n++;
        }
    }

    /* Dependents BFS */
    int dcap = npairs + 16, dn = 0;
    TCell** stack = (TCell**)malloc((size_t)dcap * sizeof(TCell*));
    for (int i = 0; i < npairs; i++) stack[dn++] = seed_cells[i];
    int ocap = 32, on = 0;
    TCell** order = (TCell**)malloc((size_t)ocap * sizeof(TCell*));
    while (dn > 0) {
        TCell* x = stack[--dn];
        for (int i = 0; i < x->nsubs; i++) {
            TCell* s = x->subs[i];
            if (!cell_in_list(order, on, s)) {
                if (on >= ocap) { ocap *= 2; order = (TCell**)realloc(order, (size_t)ocap * sizeof(TCell*)); }
                order[on++] = s;
                if (dn >= dcap) { dcap *= 2; stack = (TCell**)realloc(stack, (size_t)dcap * sizeof(TCell*)); }
                stack[dn++] = s;
            }
        }
    }
    free(stack);
    /* Sort affected cells by height */
    for (int i = 1; i < on; i++) {
        TCell* key = order[i];
        int j = i - 1;
        while (j >= 0 && order[j]->height > key->height) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
    int max_iters = 12;
    for (int i = 0; i < on; i++) {
        if (order[i]->group && order[i]->group->maxit > max_iters) {
            max_iters = order[i]->group->maxit;
        }
    }

    TOverride* prev_ov = rt->overrides;
    int prev_n = rt->n_overrides;
    int prev_cap = rt->cap_overrides;
    rt->overrides = ov;
    rt->n_overrides = n;
    rt->cap_overrides = cap;
    frames_push(rt, rt->next_cell_id);

    for (int it = 0; it < max_iters; it++) {
        bool moved = false;
        for (int i = 0; i < on; i++) {
            TCell* c = order[i];
            TVal nv = c->expr(rt, c->env);
            int found = -1;
            for (int k = 0; k < rt->n_overrides; k++) {
                if (rt->overrides[k].cell == c) { found = k; break; }
            }
            if (found < 0 || !thrum_veq(rt->overrides[found].val, nv)) {
                if (found < 0) {
                    if (rt->n_overrides >= rt->cap_overrides) {
                        rt->cap_overrides *= 2;
                        rt->overrides = (TOverride*)realloc(rt->overrides, (size_t)rt->cap_overrides * sizeof(TOverride));
                    }
                    rt->overrides[rt->n_overrides].cell = c;
                    rt->overrides[rt->n_overrides].val = nv;
                    rt->n_overrides++;
                } else {
                    rt->overrides[found].val = nv;
                }
                moved = true;
            }
        }
        if (!moved) break;
    }

    ov = rt->overrides;
    n = rt->n_overrides;
    rt->overrides = prev_ov;
    rt->n_overrides = prev_n;
    rt->cap_overrides = prev_cap;
    rt->frames_top--;
    free(order);
    *out_ov = ov;
    *out_n = n;
}

void thrum_whatif(TRuntime* rt, TEnv* env, int npairs, const char** names, TVal* vals,
                  TExprFn body, int line, int col) {
    T_FRAME(rt);
    for (int i = 0; i < npairs; i++) thrum_push_root(rt, &vals[i]);
    TCell** seeds = (TCell**)malloc((size_t)(npairs > 0 ? npairs : 1) * sizeof(TCell*));
    for (int i = 0; i < npairs; i++) {
        TEnv* fe = NULL;
        TEnvSlot* sl = NULL;
        if (!env_find(env, names[i], &fe, &sl) ||
            sl->val.kind != TV_OBJ || sl->val.as.obj->kind != OBJ_CELL) {
            free(seeds);
            char msg[160];
            snprintf(msg, sizeof(msg), "whatif needs a live binding, '%s' isn't one", names[i]);
            thrum_error(rt, line, col, msg, NULL);
        }
        seeds[i] = (TCell*)sl->val.as.obj;
    }
    TOverride* ov = NULL;
    int nov = 0;
    shadow_compute(rt, npairs, seeds, vals, &ov, &nov);
    free(seeds);

    TOverride* prev_ov = rt->overrides;
    int prev_n = rt->n_overrides;
    int prev_cap = rt->cap_overrides;
    rt->overrides = ov;
    rt->n_overrides = nov;
    rt->cap_overrides = nov;
    frames_push(rt, rt->next_cell_id);

    int tf = thrum_push_try(rt);
    if (setjmp(rt->try_stack[tf].buf) == 0) {
        TEnv* sub = thrum_env_new(rt, env, false);
        T_VAR(rt, sv, thrum_obj((TObj*)sub));
        (void)body(rt, sub);
        thrum_pop_try(rt);
        rt->overrides = prev_ov;
        rt->n_overrides = prev_n;
        rt->cap_overrides = prev_cap;
        rt->frames_top--;
        free(ov);
        T_END_FRAME(rt);
    } else {
        /* Uncaught inside whatif: try frame already restored overrides/frames */
        thrum_error(rt, rt->err_line, rt->err_col, rt->err_msg, rt->err_hint);
    }
}

static int cmp_cell_name(const void* a, const void* b) {
    const TCell* ca = *(const TCell* const*)a;
    const TCell* cb = *(const TCell* const*)b;
    return strcmp(ca->name, cb->name);
}

static void why_walk(TRuntime* rt, TCell* c, const char* pre, bool last, int depth,
                     TCell*** seen, int* seen_n, int* seen_cap) {
    FILE* out = rt->out ? rt->out : stdout;
    TVal val = read_cell(rt, c);
    char* vs = thrum_vstr_alloc(rt, val, false);
    const char* tag = (c->ckind == CELL_SOURCE) ? "input" : "relation";
    fprintf(out, "%s%s%s = %s   [%s, line %d]\n", pre, last ? "`- " : "|- ", c->name, vs, tag, c->line);
    free(vs);
    if (c->src) {
        const char* p = c->src;
        while (*p == ' ' || *p == '\t') p++;
        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\n' || p[len - 1] == '\r')) len--;
        fprintf(out, "%s   %.*s\n", pre, (int)len, p);
    }
    if (cell_in_list(*seen, *seen_n, c) || depth > 8) return;
    if (*seen_n >= *seen_cap) {
        *seen_cap = *seen_cap ? (*seen_cap) * 2 : 32;
        *seen = (TCell**)realloc(*seen, (size_t)(*seen_cap) * sizeof(TCell*));
    }
    (*seen)[(*seen_n)++] = c;
    if (c->ndeps == 0) return;
    TCell** ds = (TCell**)malloc((size_t)c->ndeps * sizeof(TCell*));
    memcpy(ds, c->deps, (size_t)c->ndeps * sizeof(TCell*));
    qsort(ds, (size_t)c->ndeps, sizeof(TCell*), cmp_cell_name);
    size_t plen = strlen(pre);
    char* next_pre = (char*)malloc(plen + 4);
    memcpy(next_pre, pre, plen);
    memcpy(next_pre + plen, last ? "   " : "|  ", 4);
    for (int i = 0; i < c->ndeps; i++) {
        why_walk(rt, ds[i], next_pre, i == c->ndeps - 1, depth + 1, seen, seen_n, seen_cap);
    }
    free(next_pre);
    free(ds);
}

void thrum_why(TRuntime* rt, TEnv* env, const char* name, int line, int col) {
    TEnv* fe = NULL;
    TEnvSlot* sl = NULL;
    if (!env_find(env, name, &fe, &sl)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "undefined name '%s'", name);
        thrum_error(rt, line, col, msg, NULL);
    }
    FILE* out = rt->out ? rt->out : stdout;
    if (sl->val.kind != TV_OBJ || sl->val.as.obj->kind != OBJ_CELL) {
        char* vs = thrum_vstr_alloc(rt, sl->val, false);
        fprintf(out, "%s = %s   (snapshot)\n", name, vs);
        free(vs);
        return;
    }
    TCell** seen = NULL;
    int seen_n = 0, seen_cap = 0;
    why_walk(rt, (TCell*)sl->val.as.obj, "", true, 0, &seen, &seen_n, &seen_cap);
    free(seen);
}

typedef struct {
    const char* name;
    double contrib;
    double deriv;
} BudgetRow;

static int cmp_budget_row(const void* a, const void* b) {
    const BudgetRow* ra = (const BudgetRow*)a;
    const BudgetRow* rb = (const BudgetRow*)b;
    if (rb->contrib > ra->contrib) return 1;
    if (rb->contrib < ra->contrib) return -1;
    return 0;
}

static bool ov_lookup(TOverride* ov, int n, TCell* c, TVal* out) {
    for (int i = 0; i < n; i++) {
        if (ov[i].cell == c) { *out = ov[i].val; return true; }
    }
    return false;
}

void thrum_budget(TRuntime* rt, TEnv* env, const char* name, int line, int col) {
    TEnv* fe = NULL;
    TEnvSlot* sl = NULL;
    if (!env_find(env, name, &fe, &sl) ||
        sl->val.kind != TV_OBJ || sl->val.as.obj->kind != OBJ_CELL) {
        thrum_error(rt, line, col, "budget needs a live binding", NULL);
    }
    TCell* target = (TCell*)sl->val.as.obj;
    TOverride* ov0 = NULL;
    int nov0 = 0;
    shadow_compute(rt, 0, NULL, NULL, &ov0, &nov0);
    TVal base = read_cell(rt, target);
    ov_lookup(ov0, nov0, target, &base);
    free(ov0);

    double sc_disp = 1.0;
    char sym[64] = "";
    if (base.kind == TV_NUM) {
        if (base.as.q.disp_sym && base.as.q.disp_sc != 0.0) {
            sc_disp = base.as.q.disp_sc;
            snprintf(sym, sizeof(sym), "%s", base.as.q.disp_sym);
        } else {
            thrum_dimstr(base.as.q.d, sym, sizeof(sym));
        }
    }

    /* Gather ancestor cells */
    int acap = 32, an = 0, scap = target->ndeps + 16, sn = 0;
    TCell** anc = (TCell**)malloc((size_t)acap * sizeof(TCell*));
    TCell** stk = (TCell**)malloc((size_t)scap * sizeof(TCell*));
    for (int i = 0; i < target->ndeps; i++) stk[sn++] = target->deps[i];
    while (sn > 0) {
        TCell* x = stk[--sn];
        if (!cell_in_list(anc, an, x)) {
            if (an >= acap) { acap *= 2; anc = (TCell**)realloc(anc, (size_t)acap * sizeof(TCell*)); }
            anc[an++] = x;
            if (sn + x->ndeps > scap) {
                while (scap < sn + x->ndeps) scap *= 2;
                stk = (TCell**)realloc(stk, (size_t)scap * sizeof(TCell*));
            }
            for (int i = 0; i < x->ndeps; i++) stk[sn++] = x->deps[i];
        }
    }
    free(stk);
    qsort(anc, (size_t)an, sizeof(TCell*), cmp_cell_name);

    BudgetRow* rows = (BudgetRow*)malloc((size_t)(an > 0 ? an : 1) * sizeof(BudgetRow));
    int nrows = 0;
    for (int i = 0; i < an; i++) {
        TCell* d = anc[i];
        if (d->ckind != CELL_SOURCE) continue;
        TVal val = d->value;
        if (rt->overrides) ov_lookup(rt->overrides, rt->n_overrides, d, &val);
        if (val.kind != TV_NUM || base.kind != TV_NUM) continue;
        TQ x = val.as.q;
        double h = fabs(x.v) * 1e-6;
        if (h < 1e-10) h = 1e-10;
        TVal vx_up = thrum_qty(rt, x.v + h, x.u, x.d, x.disp_sc, x.disp_sym);
        TOverride* ov_up = NULL;
        int nov_up = 0;
        shadow_compute(rt, 1, &d, &vx_up, &ov_up, &nov_up);
        TVal up = thrum_nil();
        bool has_up = ov_lookup(ov_up, nov_up, target, &up);
        free(ov_up);
        if (!has_up || up.kind != TV_NUM || !thrum_dim_eq(up.as.q.d, base.as.q.d)) continue;

        bool has_deriv = false;
        double deriv = 0.0;
        int tf = thrum_push_try(rt);
        if (setjmp(rt->try_stack[tf].buf) == 0) {
            TVal vx_dn = thrum_qty(rt, x.v - h, x.u, x.d, x.disp_sc, x.disp_sym);
            TOverride* ov_dn = NULL;
            int nov_dn = 0;
            shadow_compute(rt, 1, &d, &vx_dn, &ov_dn, &nov_dn);
            TVal dn = thrum_nil();
            bool has_dn = ov_lookup(ov_dn, nov_dn, target, &dn);
            free(ov_dn);
            thrum_pop_try(rt);
            if (has_dn && dn.kind == TV_NUM && thrum_dim_eq(dn.as.q.d, base.as.q.d)) {
                deriv = (up.as.q.v - dn.as.q.v) / (2.0 * h);
                has_deriv = true;
            }
        }
        if (!has_deriv) deriv = (up.as.q.v - base.as.q.v) / h;
        rows[nrows].name = d->name;
        rows[nrows].contrib = fabs(deriv) * x.u / sc_disp;
        rows[nrows].deriv = deriv / sc_disp;
        nrows++;
    }
    free(anc);

    FILE* out = rt->out ? rt->out : stdout;
    char* bs = thrum_vstr_alloc(rt, base, false);
    fprintf(out, "uncertainty budget for %s = %s\n", name, bs);
    free(bs);
    if (nrows == 0) {
        fprintf(out, "  (no numeric inputs)\n");
        free(rows);
        return;
    }
    double tot2 = 0.0;
    for (int i = 0; i < nrows; i++) tot2 += rows[i].contrib * rows[i].contrib;
    double tot = sqrt(tot2);
    char hdr[80];
    if (sym[0]) snprintf(hdr, sizeof(hdr), "sensitivity (%s)", sym);
    else snprintf(hdr, sizeof(hdr), "sensitivity");
    fprintf(out, "  %-14s %14s %10s   %s\n", "input", "contribution", "share", hdr);
    qsort(rows, (size_t)nrows, sizeof(BudgetRow), cmp_budget_row);
    for (int i = 0; i < nrows; i++) {
        double share = tot > 0.0 ? (rows[i].contrib * rows[i].contrib / tot2 * 100.0) : 0.0;
        char cbuf[32];
        snprintf(cbuf, sizeof(cbuf), "%.4g", rows[i].contrib);
        fprintf(out, "  %-14s %14s %9.1f%%   d/dx = %.6g\n",
                rows[i].name, cbuf, share, rows[i].deriv);
    }
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "%.4g", tot);
    fprintf(out, "  %-14s %14s\n", "combined", tbuf);
    free(rows);
}

/* ============================== Modules ============================== */
void thrum_reg_loading(TRuntime* rt, const char* key) {
    thrum_map_set_cs(rt, rt->modules, key, thrum_bool(true));
}

void thrum_reg_module(TRuntime* rt, const char* key, TEnv* scope) {
    T_FRAME(rt);
    T_VAR(rt, sv, thrum_obj((TObj*)scope));
    TMod* m = (TMod*)thrum_alloc_obj(rt, OBJ_MOD, sizeof(TMod));
    m->name = thrum_intern(rt, key);
    m->scope = scope;
    T_VAR(rt, mv, thrum_obj((TObj*)m));
    thrum_map_set_cs(rt, rt->modules, key, mv);
    T_END_FRAME(rt);
}

TVal thrum_get_module(TRuntime* rt, const char* key) {
    TVal out = thrum_nil();
    if (thrum_map_get(rt->modules, thrum_cstr(rt, key), &out)) return out;
    return thrum_nil();
}
