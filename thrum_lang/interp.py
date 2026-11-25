"""Thrum tree-walking interpreter, provenance (why/budget), and counterfactuals (whatif)."""

import math
import os
import sys

from .ast_nodes import Node
from .builtins import install_builtins
from .caps import CAPS, Journal
from .checker import resolve_module_path
from .errors import BreakSig, ContinueSig, ReturnSig, ThrumError
from .graph import Cell, Group, Reaction, Scope
from .parser import parse
from .stdmods import STDMODS
from .units import dimlabel, dimless, dimstr
from .values import (
    Builtin,
    Cap,
    Fn,
    Module,
    Q,
    RecordField,
    RecordType,
    RecordVal,
    convert,
    mkq,
    qadd,
    qapprox,
    qcmp,
    qdiv,
    qmod,
    qmul,
    qpow,
    relchg,
    tname,
    truthy,
    veq,
    vstr,
)


class Interp:
    def __init__(self, out=None, journal=None, trace=False, path=None, prog_args=None):
        self.builtins = Scope()
        self.globals = Scope(self.builtins, rel=True)
        self.track, self.frames = [], []
        self.overrides = None
        self.reactions = []
        self.groups = []
        self.queue = []
        self.draining = False
        self.curgroup = None
        self.out = out or sys.stdout
        self.journal = journal or Journal()
        self.trace = trace
        self.path = path
        self.src = ""
        self.depth = 0
        self.modules = {}
        self.prog_args = list(prog_args or [])
        install_builtins(self)

    # ---- cells ----
    def read(self, b):
        if not isinstance(b, Cell):
            return b
        if self.overrides is not None and b in self.overrides:
            return self.overrides[b]
        if self.track and self.frames and b.born < self.frames[0]:
            t = self.track[-1]
            if t is not b:
                t.newdeps.add(b)
        return b.value

    def bind(self, sc, name, val):
        old = sc.vars.get(name)
        if isinstance(old, Cell):
            for d in old.deps:
                d.subs.discard(old)
            old.deps.clear()
            old.subs.clear()
            old.dead = True
            if old.group and old in old.group.cells:
                old.group.cells.remove(old)
        sc.vars[name] = val
        sc.const_names.discard(name)

    def cycle(self, c, deps):
        stack, seen = list(deps), set()
        while stack:
            x = stack.pop()
            if x is c:
                return True
            if x in seen:
                continue
            seen.add(x)
            stack.extend(x.deps)
        return False

    def bump(self, c, seen=None):
        seen = seen if seen is not None else set()
        if c in seen:
            return
        seen.add(c)
        if c.group is not None:
            gh = 0
            for gc in c.group.cells:
                ext = [d for d in gc.deps if d.group is not c.group]
                if ext:
                    gh = max(gh, 1 + max(d.height for d in ext))
            for gc in c.group.cells:
                if gh != gc.height:
                    gc.height = gh
                    for s in gc.subs:
                        if s.group is not c.group:
                            self.bump(s, seen)
        else:
            nh = 0 if not c.deps else 1 + max(d.height for d in c.deps)
            if nh != c.height:
                c.height = nh
                for s in c.subs:
                    self.bump(s, seen)

    def link(self, c, deps, allow_cycle=False):
        if self.cycle(c, deps) and not allow_cycle:
            raise ThrumError(
                "cyclic relation involving '%s'" % c.name,
                c.site,
                "wrap mutually-recursive relations in a 'settle' block with 'init' seeds",
            )
        for d in c.deps - deps:
            d.subs.discard(c)
        for d in deps - c.deps:
            d.subs.add(c)
        c.deps = deps
        self.bump(c)

    def recompute(self, c):
        self.track.append(c)
        self.frames.append(Cell._n)
        c.newdeps = set()
        try:
            v = self.eval(c.expr, c.env)
        finally:
            self.track.pop()
            self.frames.pop()
        self.link(c, c.newdeps, allow_cycle=c.group is not None)
        if self.trace and not veq(c.value, v):
            self.out.write("  [live] %s -> %s\n" % (c.name, vstr(v)))
        c.value = v
        c.eval_id += 1
        return v

    def notify(self, c):
        c.version += 1
        self.queue.append(c)
        if not self.draining:
            self.drain()

    def drain(self):
        if self.draining:
            return
        self.draining = True
        try:
            rounds = 0
            while True:
                if self.queue:
                    seeds, self.queue = self.queue, []
                    self.propagate(seeds)
                self.fire()
                if not self.queue:
                    break
                rounds += 1
                if rounds > 1000:
                    raise ThrumError(
                        "reaction cascade exceeded 1000 rounds",
                        None,
                        "a reaction is probably feeding its own input",
                    )
        finally:
            self.draining = False

    def propagate(self, seeds):
        pending, groups = {}, set()

        def add(x):
            if x.dead:
                return
            if x.group is not None:
                groups.add(x.group)
            else:
                pending[x] = x.height

        for s in seeds:
            for sub in s.subs:
                add(sub)
        steps = 0
        while pending or groups:
            c_min = min(pending, key=lambda k: (pending[k], k.id)) if pending else None
            g_min = min(groups, key=lambda g: g.height) if groups else None
            if c_min is not None and (g_min is None or pending[c_min] <= g_min.height):
                del pending[c_min]
                old = c_min.value
                self.recompute(c_min)
                if not veq(old, c_min.value):
                    c_min.version += 1
                    for sub in c_min.subs:
                        add(sub)
            else:
                groups.remove(g_min)
                changed = self.relax(g_min)
                for c in changed:
                    for sub in c.subs:
                        if sub.group is not g_min:
                            add(sub)
            steps += 1
            if steps > 200000:
                raise ThrumError("relation network did not settle")

    def relax(self, g):
        if not g.cells:
            return []
        before = {c: c.value for c in g.cells}
        order = [c for c in g.cells if not c.has_init] + [c for c in g.cells if c.has_init]
        for _ in range(g.maxit):
            delta = 0.0
            moved = False
            for c in order:
                old = c.value
                self.track.append(c)
                self.frames.append(Cell._n)
                c.newdeps = set()
                try:
                    v = self.eval(c.expr, c.env)
                finally:
                    self.track.pop()
                    self.frames.pop()
                self.link(c, c.newdeps, allow_cycle=True)
                c.value = v
                c.eval_id += 1
                if not veq(old, v):
                    moved = True
                    c.version += 1
                    delta = max(delta, relchg(old, v))
            if not moved or delta <= g.tol:
                return [c for c in g.cells if not veq(before[c], c.value)]
        raise ThrumError(
            "settle did not converge in %d iterations" % g.maxit,
            g.site,
            "increase 'max', loosen 'tol', or check that the relations are contractive",
        )

    def fire(self):
        for r in self.reactions:
            cur = truthy(r.guard.value)
            if r.mode == "rise":
                go = cur and not r.prev
            elif r.mode == "fall":
                go = (not cur) and r.prev
            else:
                go = cur and r.guard.eval_id != r.seen
            r.prev = cur
            r.seen = r.guard.eval_id
            if go:
                self.exec_block(r.body, Scope(r.env, rel=False))

    def dependents(self, seeds):
        out, stack = set(), list(seeds)
        while stack:
            x = stack.pop()
            for s in x.subs:
                if s not in out:
                    out.add(s)
                    stack.append(s)
        return out

    def ancestors(self, c):
        out, stack = set(), list(c.deps)
        while stack:
            x = stack.pop()
            if x not in out:
                out.add(x)
                stack.extend(x.deps)
        return out

    def shadow(self, seeds):
        ov = dict(self.overrides) if self.overrides else {}
        ov.update(seeds)
        affected = self.dependents(seeds)
        order = sorted(affected, key=lambda c: c.height)
        max_iters = max(12, max((c.group.maxit for c in order if c.group is not None), default=0))
        prev, self.overrides = self.overrides, ov
        self.frames.append(Cell._n)
        try:
            for _ in range(max_iters):
                moved = False
                for c in order:
                    nv = self.eval(c.expr, c.env)
                    if c not in ov or not veq(ov[c], nv):
                        ov[c] = nv
                        moved = True
                if not moved:
                    break
        finally:
            self.overrides = prev
            self.frames.pop()
        return ov

    # ---- mutation guard ----
    def guard_mut(self, c, site):
        if self.frames and c.born < self.frames[0]:
            raise ThrumError(
                "cannot mutate '%s' inside a relation" % c.name,
                site,
                "relations must be pure; change inputs from an 'on' reaction or top level",
            )

    # ---- evaluation ----
    def eval(self, n, sc):
        k = n.kind
        if k == "q":
            return n.value
        if k == "name":
            s = sc.find(n.name)
            if s is None:
                raise ThrumError("undefined name '%s'" % n.name, n.site)
            return self.read(s.vars[n.name])
        if k == "text":
            return "".join(p if t == "str" else vstr(self.eval(p, sc)) for t, p in n.parts)
        if k == "list":
            return [self.eval(x, sc) for x in n.items]
        if k == "map":
            out = {}
            for kn, vn in n.entries:
                kv = self.eval(kn, sc)
                if isinstance(kv, (list, dict)):
                    raise ThrumError("map keys cannot be %s" % tname(kv), n.site)
                out[kv] = self.eval(vn, sc)
            return out
        if k == "rec_lit":
            rt = self.eval(n.target, sc)
            if not isinstance(rt, RecordType):
                raise ThrumError("%s is not a record type" % tname(rt), n.site)
            provided = {}
            for kn, vn in n.entries:
                kv = self.eval(kn, sc)
                if not isinstance(kv, str) or kv not in rt.field_map:
                    raise ThrumError("record '%s' has no field '%s'" % (rt.name, vstr(kv)), n.site)
                provided[kv] = self.eval(vn, sc)
            return self.make_record_map(rt, provided, n.site)
        if k == "bin":
            return self.binop(n, sc)
        if k == "un":
            v = self.eval(n.operand, sc)
            if n.op == "not":
                return not truthy(v)
            if not isinstance(v, Q):
                raise ThrumError("cannot negate %s" % tname(v), n.site)
            return Q(-v.v, v.d, v.u, v.disp)
        if k == "convert":
            return convert(self.needq(self.eval(n.obj, sc), n.site), n.unit, n.site)
        if k == "call":
            f = self.eval(n.fn, sc)
            args = [self.eval(a, sc) for a in n.args]
            return self.call(f, args, n.site)
        if k == "index":
            o = self.eval(n.obj, sc)
            ix = self.eval(n.idx, sc)
            return self.getkey(o, ix, n.site)
        if k == "slice":
            o = self.eval(n.obj, sc)
            if not isinstance(o, (list, str)):
                raise ThrumError("cannot slice %s" % tname(o), n.site)
            st = self.needint(self.eval(n.start, sc), n.site) if n.start is not None else None
            sp = self.needint(self.eval(n.stop, sc), n.site) if n.stop is not None else None
            step = self.needint(self.eval(n.step, sc), n.site) if n.step is not None else None
            if step == 0:
                raise ThrumError("slice step cannot be 0", n.site)
            return o[slice(st, sp, step)]
        if k == "dot":
            o = self.eval(n.obj, sc)
            if isinstance(o, Module):
                s = o.scope.find(n.name)
                if s is None:
                    raise ThrumError("module '%s' has no '%s'" % (o.name, n.name), n.site)
                return self.read(s.vars[n.name])
            if isinstance(o, Cap):
                if n.name not in o.methods:
                    raise ThrumError("capability '%s' has no '%s'" % (o.name, n.name), n.site)
                return o.methods[n.name]
            if isinstance(o, RecordVal):
                if n.name not in o.rtype.field_map:
                    raise ThrumError("record '%s' has no field '%s'" % (o.rtype.name, n.name), n.site)
                return o.fields[n.name]
            if isinstance(o, dict):
                return o.get(n.name)
            raise ThrumError("cannot read '.%s' of %s" % (n.name, tname(o)), n.site)
        if k == "lambda":
            f = Fn()
            f.name = None
            f.env = sc
            f.ret = None
            f.site = n.site
            f.params = [Node("param", n.site, name="it", type=None, default=None)]
            f.body = [Node("return", n.site, value=n.body)]
            f.requires = []
            f.ensures = []
            return f
        if k == "fnexpr":
            f = Fn()
            f.name = None
            f.env = sc
            f.ret = n.ret
            f.site = n.site
            f.params, f.body = n.params, n.body
            f.requires = n.requires
            f.ensures = n.ensures
            return f
        if k == "comp":
            it = self.eval(n.iter, sc)
            if not isinstance(it, list):
                raise ThrumError("comprehension needs a list", n.site)
            sub = Scope(sc)
            out = []
            for v in it:
                sub.vars[n.var] = v
                if n.cond is None or truthy(self.eval(n.cond, sub)):
                    out.append(self.eval(n.elem, sub))
            return out
        raise ThrumError("cannot evaluate %s" % k, n.site)

    def make_record_map(self, rt, provided, site):
        fields = {}
        for f in rt.fields:
            if f.name in provided:
                val = provided[f.name]
            elif f.default is not None:
                val = self.eval(f.default, rt.env)
            else:
                raise ThrumError("missing required field '%s' for record '%s'" % (f.name, rt.name), site)
            self.checktype(val, f.type, "field '%s' of %s" % (f.name, rt.name), site)
            fields[f.name] = val
        return RecordVal(rt, fields)

    def getkey(self, o, ix, site):
        if isinstance(o, list):
            i = self.needint(ix, site)
            if i < 0:
                i += len(o)
            if not 0 <= i < len(o):
                raise ThrumError("index %d out of range (len %d)" % (i, len(o)), site)
            return o[i]
        if isinstance(o, dict):
            if isinstance(ix, (list, dict)):
                raise ThrumError("map keys cannot be %s" % tname(ix), site)
            return o.get(ix)
        if isinstance(o, str):
            i = self.needint(ix, site)
            if i < 0:
                i += len(o)
            if not 0 <= i < len(o):
                raise ThrumError("index out of range", site)
            return o[i]
        raise ThrumError("cannot index %s" % tname(o), site)

    def binop(self, n, sc):
        op = n.op
        if op == "and":
            return truthy(self.eval(n.left, sc)) and truthy(self.eval(n.right, sc))
        if op == "or":
            return truthy(self.eval(n.left, sc)) or truthy(self.eval(n.right, sc))
        if op == "??":
            a = self.eval(n.left, sc)
            return self.eval(n.right, sc) if a is None else a
        a = self.eval(n.left, sc)
        b = self.eval(n.right, sc)
        return self.apply(op, a, b, n.site)

    def apply(self, op, a, b, site):
        if op == "==":
            return veq(a, b)
        if op == "!=":
            return not veq(a, b)
        if op == "~":
            if isinstance(a, Q) and isinstance(b, Q):
                return qapprox(a, b)
            return veq(a, b)
        if op in ("in", "notin"):
            if isinstance(b, list):
                r = any(veq(a, x) for x in b)
            elif isinstance(b, dict):
                r = False if isinstance(a, (list, dict)) else (a in b)
            elif isinstance(b, str):
                r = vstr(a) in b
            else:
                raise ThrumError("'in' needs a list, map or text", site)
            return r if op == "in" else not r
        if op in ("..", "..<"):
            i, j = self.needint(a, site), self.needint(b, site)
            if op == "..<":
                j -= 1
            if j - i > 1000000:
                raise ThrumError("range too large", site)
            return [mkq(float(x)) for x in range(i, j + 1)]
        if op in ("<", ">", "<=", ">="):
            if isinstance(a, Q) and isinstance(b, Q):
                return qcmp(a, b, op, site)
            if isinstance(a, str) and isinstance(b, str):
                return {"<": a < b, ">": a > b, "<=": a <= b, ">=": a >= b}[op]
            raise ThrumError("cannot order %s and %s" % (tname(a), tname(b)), site)
        if op == "+":
            if isinstance(a, Q) and isinstance(b, Q):
                return qadd(a, b, 1, site)
            if isinstance(a, str) and isinstance(b, str):
                return a + b
            if isinstance(a, list) and isinstance(b, list):
                return a + b
            if isinstance(a, dict) and isinstance(b, dict):
                return {**a, **b}
            raise ThrumError("cannot add %s and %s" % (tname(a), tname(b)), site)
        if op == "-":
            if isinstance(a, Q) and isinstance(b, Q):
                return qadd(a, b, -1, site)
            raise ThrumError("cannot subtract %s from %s" % (tname(b), tname(a)), site)
        if op == "*":
            if isinstance(a, Q) and isinstance(b, Q):
                return qmul(a, b)
            if isinstance(a, (str, list)) and isinstance(b, Q):
                cnt = self.needint(b, site)
                if cnt > 1000000:
                    raise ThrumError("repeat count too large", site)
                return a * cnt
            raise ThrumError("cannot multiply %s and %s" % (tname(a), tname(b)), site)
        if op == "/":
            if isinstance(a, Q) and isinstance(b, Q):
                return qdiv(a, b, site)
            raise ThrumError("cannot divide %s by %s" % (tname(a), tname(b)), site)
        if op == "%":
            if isinstance(a, Q) and isinstance(b, Q):
                return qmod(a, b, site)
            raise ThrumError("'%' needs numbers", site)
        if op == "^":
            if isinstance(a, Q) and isinstance(b, Q):
                return qpow(a, b, site)
            raise ThrumError("'^' needs numbers", site)
        raise ThrumError("unknown operator '%s'" % op, site)

    def needq(self, v, site=None):
        if not isinstance(v, Q):
            raise ThrumError("expected a number, got %s" % tname(v), site)
        return v

    def needint(self, v, site=None):
        q = self.needq(v, site)
        if not dimless(q.d):
            raise ThrumError("expected a plain count, got %s" % dimlabel(q), site)
        if not math.isfinite(q.v) or q.v != int(q.v):
            raise ThrumError("expected a whole number, got %g" % q.v, site)
        return int(q.v)

    def _matches_type(self, v, t):
        if t is None:
            return True
        if t[0] == "kind":
            if t[1] == "any":
                return True
            if t[1] == "record":
                return isinstance(v, RecordVal)
            return tname(v) == t[1]
        if t[0] == "record":
            return isinstance(v, RecordVal) and v.rtype.name == t[1]
        if t[0] == "list_of":
            return isinstance(v, list) and all(self._matches_type(x, t[1]) for x in v)
        if t[0] == "map_of":
            return isinstance(v, dict) and all(self._matches_type(k, t[1]) and self._matches_type(val, t[2]) for k, val in v.items())
        if t[0] == "union":
            return any(self._matches_type(v, alt) for alt in t[1])
        return isinstance(v, Q) and v.d == t[1]

    def checktype(self, v, t, what, site):
        if t is None:
            return
        if t[0] == "kind":
            if t[1] == "any":
                return
            if t[1] == "record":
                if not isinstance(v, RecordVal):
                    raise ThrumError("%s should be record, got %s" % (what, tname(v)), site)
                return
            if tname(v) != t[1]:
                raise ThrumError("%s should be %s, got %s" % (what, t[1], tname(v)), site)
        elif t[0] == "record":
            if not isinstance(v, RecordVal) or v.rtype.name != t[1]:
                raise ThrumError("%s should be %s, got %s" % (what, t[2], tname(v)), site)
        elif t[0] == "list_of":
            if not isinstance(v, list):
                raise ThrumError("%s should be %s, got %s" % (what, t[2], tname(v)), site)
            for idx, item in enumerate(v):
                self.checktype(item, t[1], "element %d of %s" % (idx, what), site)
        elif t[0] == "map_of":
            if not isinstance(v, dict):
                raise ThrumError("%s should be %s, got %s" % (what, t[3], tname(v)), site)
            for mk, mv in v.items():
                self.checktype(mk, t[1], "key of %s" % what, site)
                self.checktype(mv, t[2], "value of %s" % what, site)
        elif t[0] == "union":
            if not any(self._matches_type(v, alt) for alt in t[1]):
                raise ThrumError("%s should be %s, got %s" % (what, t[2], tname(v)), site)
        else:
            if not isinstance(v, Q) or v.d != t[1]:
                raise ThrumError(
                    "%s should have dimension %s, got %s"
                    % (what, t[2], dimlabel(v) if isinstance(v, Q) else tname(v)),
                    site,
                )

    def call(self, f, args, site):
        if isinstance(f, Builtin):
            if f.arity is not None and len(args) not in f.arity:
                raise ThrumError(
                    "%s expects %s argument(s), got %d" % (f.name, "/".join(map(str, f.arity)), len(args)),
                    site,
                )
            return f.f(self, args, site)
        if isinstance(f, RecordType):
            nreq = sum(1 for fld in f.fields if fld.default is None)
            if len(args) < nreq or len(args) > len(f.fields):
                raise ThrumError(
                    "record %s expects %d..%d arguments, got %d" % (f.name, nreq, len(f.fields), len(args)),
                    site,
                )
            provided = {fld.name: args[i] for i, fld in enumerate(f.fields) if i < len(args)}
            return self.make_record_map(f, provided, site)
        if isinstance(f, Fn):
            self.depth += 1
            if self.depth > 400:
                self.depth -= 1
                raise ThrumError("call stack too deep (400)", site)
            try:
                sc = Scope(f.env, rel=False)
                nreq = sum(1 for p in f.params if p.default is None)
                if len(args) < nreq or len(args) > len(f.params):
                    raise ThrumError(
                        "%s expects %d..%d arguments, got %d" % (f.name or "fn", nreq, len(f.params), len(args)),
                        site,
                    )
                for i, p in enumerate(f.params):
                    v = args[i] if i < len(args) else self.eval(p.default, sc)
                    self.checktype(v, p.type, "parameter '%s'" % p.name, site)
                    sc.vars[p.name] = v
                for c in f.requires:
                    if not truthy(self.eval(c.cond, sc)):
                        raise ThrumError(
                            "contract failed in %s: %s"
                            % (f.name or "fn", vstr(self.eval(c.msg, sc)) if c.msg else c.src),
                            site,
                        )
                ret = None
                try:
                    self.exec_block(f.body, sc)
                except ReturnSig as r:
                    ret = r.value
                for c in f.ensures:
                    sc.vars["result"] = ret
                    if not truthy(self.eval(c.cond, sc)):
                        raise ThrumError(
                            "postcondition failed in %s: %s"
                            % (f.name or "fn", vstr(self.eval(c.msg, sc)) if c.msg else c.src),
                            site,
                        )
                self.checktype(ret, f.ret, "return of %s" % (f.name or "fn"), site)
                return ret
            finally:
                self.depth -= 1
        raise ThrumError("%s is not callable" % tname(f), site)

    # ---- statements ----
    def exec_block(self, body, sc):
        for st in body:
            self.exec(st, sc)

    def exec(self, n, sc):
        k = n.kind
        if k == "exprstmt":
            self.eval(n.value, sc)
            return
        if k == "decl":
            if n.init is not None and self.curgroup is None:
                raise ThrumError("'init' is only allowed inside 'settle'", n.site)
            if n.kw == "pin":
                self.bind(sc, n.name, self.eval(n.value, sc))
                sc.const_names.add(n.name)
                return
            if n.kw == "var":
                self.bind(sc, n.name, Cell(n.name, "source", self.eval(n.value, sc), site=n.site))
                return
            if sc.rel and n.live:
                c = Cell(
                    n.name,
                    "derived",
                    None,
                    expr=n.value,
                    env=sc,
                    site=n.site,
                    src=n.src,
                    group=self.curgroup,
                    has_init=n.init is not None,
                )
                self.bind(sc, n.name, c)
                if self.curgroup is not None:
                    c.value = self.eval(n.init, sc) if n.init is not None else None
                    self.curgroup.cells.append(c)
                else:
                    self.recompute(c)
            else:
                self.bind(sc, n.name, self.eval(n.value, sc))
                sc.const_names.add(n.name)
            return
        if k == "record":
            flds = [RecordField(f.name, f.type, f.default, f.site) for f in n.fields]
            rt = RecordType(n.name, flds, env=sc, site=n.site)
            self.bind(sc, n.name, rt)
            sc.const_names.add(n.name)
            return
        if k == "assign":
            self.do_assign(n, sc)
            return
        if k == "fndecl":
            f = Fn()
            f.name = n.name
            f.params = n.params
            f.body = n.body
            f.env = sc
            f.ret = n.ret
            f.requires = n.requires
            f.ensures = n.ensures
            f.site = n.site
            self.bind(sc, n.name, f)
            sc.const_names.add(n.name)
            return
        if k == "if":
            for cond, body in n.arms:
                if truthy(self.eval(cond, sc)):
                    self.exec_block(body, sc)
                    return
            if n.els:
                self.exec_block(n.els, sc)
            return
        if k == "for":
            it = self.eval(n.iter, sc)
            if isinstance(it, str):
                it = list(it)
            if not isinstance(it, list):
                raise ThrumError("cannot iterate %s" % tname(it), n.site)
            for v in it:
                sub = Scope(sc, rel=False)
                if n.b:
                    if not isinstance(v, list) or len(v) != 2:
                        raise ThrumError("expected pairs to unpack", n.site)
                    sub.vars[n.a], sub.vars[n.b] = v
                else:
                    sub.vars[n.a] = v
                try:
                    self.exec_block(n.body, sub)
                except BreakSig:
                    break
                except ContinueSig:
                    continue
            return
        if k == "while":
            while truthy(self.eval(n.cond, sc)):
                try:
                    self.exec_block(n.body, sc)
                except BreakSig:
                    break
                except ContinueSig:
                    continue
            return
        if k == "repeat":
            for _ in range(self.needint(self.eval(n.count, sc), n.site)):
                try:
                    self.exec_block(n.body, sc)
                except BreakSig:
                    break
                except ContinueSig:
                    continue
            return
        if k == "match":
            subj = self.eval(n.subject, sc)
            for pat, guard, body in n.cases:
                binds = {}
                if matchpat(pat, subj, binds):
                    sub = Scope(sc)
                    sub.vars.update(binds)
                    if guard is None or truthy(self.eval(guard, sub)):
                        self.exec_block(body, sub)
                        return
            return
        if k == "try":
            try:
                self.exec_block(n.body, sc)
            except ThrumError as e:
                sub = Scope(sc, rel=sc.rel)
                sub.vars[n.name] = {
                    "message": e.msg,
                    "hint": e.hint or "",
                    "line": mkq(e.site[0]) if e.site else None,
                }
                self.exec_block(n.handler, sub)
            return
        if k == "return":
            raise ReturnSig(self.eval(n.value, sc) if n.value else None)
        if k == "break":
            raise BreakSig()
        if k == "continue":
            raise ContinueSig()
        if k == "fail":
            raise ThrumError(vstr(self.eval(n.value, sc)), n.site)
        if k == "assert":
            if not truthy(self.eval(n.cond, sc)):
                raise ThrumError("assertion failed" + (": " + vstr(self.eval(n.msg, sc)) if n.msg else ""), n.site)
            return
        if k == "with":
            sub = Scope(sc, rel=sc.rel)
            created = []
            for nm, args in n.caps:
                if nm not in CAPS:
                    raise ThrumError(
                        "unknown capability '%s'" % nm,
                        n.site,
                        "available: " + ", ".join(sorted(CAPS)),
                    )
                cap_obj = CAPS[nm](self, [self.eval(a, sc) for a in args], n.site)
                sub.vars[nm] = cap_obj
                sub.const_names.add(nm)
                created.append(cap_obj)
            try:
                self.exec_block(n.body, sub)
            finally:
                for cap_obj in reversed(created):
                    if cap_obj.cleanup is not None:
                        cap_obj.cleanup()
            return
        if k == "on":
            g = Cell("<guard>", "derived", None, expr=n.cond, env=sc, site=n.site, src=n.src)
            self.recompute(g)
            self.reactions.append(Reaction(g, n.body, sc, n.mode))
            return
        if k == "settle":
            if not sc.rel:
                raise ThrumError("settle needs a relation scope", n.site)
            g = Group(n.maxit, self.needq(self.eval(n.tol, sc), n.site).v if n.tol else 1e-12, n.site)
            self.groups.append(g)
            old, self.curgroup = self.curgroup, g
            try:
                for st in n.body:
                    if st.kind != "decl":
                        raise ThrumError("only declarations allowed inside settle", st.site)
                    self.exec(st, sc)
            finally:
                self.curgroup = old
            self.relax(g)
            return
        if k == "live":
            sub = sc if sc.rel else Scope(sc, rel=True)
            self.exec_block(n.body, sub)
            return
        if k == "whatif":
            seeds = {}
            for nm, ex in n.pairs:
                s = sc.find(nm)
                if s is None or not isinstance(s.vars[nm], Cell):
                    raise ThrumError("whatif needs a live binding, '%s' isn't one" % nm, n.site)
                seeds[s.vars[nm]] = self.eval(ex, sc)
            ov = self.shadow(seeds)
            prev, self.overrides = self.overrides, ov
            self.frames.append(Cell._n)
            try:
                self.exec_block(n.body, Scope(sc, rel=False))
            finally:
                self.overrides = prev
                self.frames.pop()
            return
        if k == "why":
            self.do_why(n, sc)
            return
        if k == "budget":
            self.do_budget(n, sc)
            return
        if k == "use":
            self.do_use(n, sc)
            return
        if k == "contract":
            return
        raise ThrumError("cannot execute %s" % k, n.site)

    def do_assign(self, n, sc):
        path, cur = [], n.target
        while cur.kind in ("index", "dot"):
            path.append(self.eval(cur.idx, sc) if cur.kind == "index" else cur.name)
            cur = cur.obj
        if cur.kind != "name":
            raise ThrumError("invalid assignment target", n.site)
        s = sc.find(cur.name)
        if s is None:
            raise ThrumError("undefined name '%s'" % cur.name, cur.site, "declare inputs with 'var'")
        rev_path = list(reversed(path))
        target_name = cur.name
        b = s.vars[target_name]
        while isinstance(b, Module) and rev_path:
            field = rev_path.pop(0)
            if not isinstance(field, str):
                raise ThrumError("module member must be a name", n.site)
            ms = b.scope.find(field)
            if ms is None:
                raise ThrumError("module '%s' has no '%s'" % (b.name, field), n.site)
            s, target_name, b = ms, field, ms.vars[field]
        if target_name in s.const_names:
            raise ThrumError(
                "cannot assign to immutable '%s'" % target_name,
                n.site,
                "declare it with 'var' if it needs to change",
            )
        base = self.read(b)
        new = self.eval(n.value, sc)
        if n.op != "=":
            old = getpath(base, rev_path, n.site) if rev_path else base
            new = self.apply(n.op[0], old, new, n.site)
        root = setpath(base, rev_path, new, n.site, checktype=self.checktype) if rev_path else new
        if isinstance(b, Cell):
            if b.kind != "source":
                raise ThrumError(
                    "'%s' is a derived relation, not an input" % target_name,
                    n.site,
                    "assign to its inputs instead, or declare it with 'var'",
                )
            self.guard_mut(b, n.site)
            if not veq(b.value, root):
                b.value = root
                self.notify(b)
        else:
            s.vars[target_name] = root

    def do_why(self, n, sc):
        s = sc.find(n.name)
        if s is None:
            raise ThrumError("undefined name '%s'" % n.name, n.site)
        b = s.vars[n.name]
        if not isinstance(b, Cell):
            self.out.write("%s = %s   (snapshot)\n" % (n.name, vstr(b)))
            return
        lines, seen = [], set()

        def walk(c, pre, last, depth):
            val = self.read(c)
            tag = "input" if c.kind == "source" else "relation"
            ln = c.site[0] if c.site else 0
            lines.append(
                "%s%s%s = %s   [%s, line %d]" % (pre, "`- " if last else "|- ", c.name, vstr(val), tag, ln)
            )
            if c.src:
                lines.append("%s   %s" % (pre, c.src.strip()))
            if c in seen or depth > 8:
                return
            seen.add(c)
            ds = sorted(c.deps, key=lambda x: x.name)
            for i, d in enumerate(ds):
                walk(d, pre + ("   " if last else "|  "), i == len(ds) - 1, depth + 1)

        walk(b, "", True, 0)
        self.out.write("\n".join(lines) + "\n")

    def do_budget(self, n, sc):
        s = sc.find(n.name)
        if s is None or not isinstance(s.vars[n.name], Cell):
            raise ThrumError("budget needs a live binding", n.site)
        target = s.vars[n.name]
        base = self.shadow({}).get(target, self.read(target))
        sc_disp, sym = (
            base.disp
            if isinstance(base, Q) and base.disp
            else (1.0, dimstr(base.d) if isinstance(base, Q) else "")
        )
        if sc_disp == 0:
            sc_disp = 1.0
        rows = []
        for d in sorted(self.ancestors(target), key=lambda x: x.name):
            if d.kind != "source":
                continue
            val = self.overrides[d] if (self.overrides is not None and d in self.overrides) else d.value
            if not isinstance(val, Q):
                continue
            x = val
            h = max(abs(x.v) * 1e-6, 1e-10)
            up = self.shadow({d: Q(x.v + h, x.d, x.u, x.disp)}).get(target)
            if not isinstance(up, Q) or not isinstance(base, Q) or up.d != base.d:
                continue
            deriv = None
            try:
                dn = self.shadow({d: Q(x.v - h, x.d, x.u, x.disp)}).get(target)
                if isinstance(dn, Q) and dn.d == base.d:
                    deriv = (up.v - dn.v) / (2.0 * h)
            except ThrumError:
                pass
            if deriv is None:
                deriv = (up.v - base.v) / h
            rows.append((d.name, abs(deriv) * x.u / sc_disp, deriv / sc_disp, x))
        tot = math.sqrt(sum(r[1] ** 2 for r in rows))
        self.out.write("uncertainty budget for %s = %s\n" % (n.name, vstr(base)))
        if not rows:
            self.out.write("  (no numeric inputs)\n")
            return
        unit_hdr = (" (%s)" % sym) if sym else ""
        self.out.write("  %-14s %14s %10s   %s%s\n" % ("input", "contribution", "share", "sensitivity", unit_hdr))
        for nm, contrib, deriv, x in sorted(rows, key=lambda r: -r[1]):
            share = (contrib ** 2 / tot ** 2 * 100) if tot else 0.0
            self.out.write("  %-14s %14s %9.1f%%   d/dx = %.6g\n" % (nm, ("%.4g" % contrib), share, deriv))
        self.out.write("  %-14s %14s\n" % ("combined", "%.4g" % tot))

    def do_use(self, n, sc):
        if n.path is None and n.modpath is None:
            if n.name not in STDMODS:
                raise ThrumError(
                    "unknown module '%s'" % n.name,
                    n.site,
                    "available: " + ", ".join(sorted(STDMODS)),
                )
            nm = n.alias or n.name
            sc.vars[nm] = STDMODS[n.name](self)
            sc.const_names.add(nm)
            return
        p = resolve_module_path(n, self.path)
        if p in self.modules:
            if self.modules[p] is None:
                disp = ".".join(n.modpath) if n.modpath else n.path
                raise ThrumError("circular module import '%s'" % disp, n.site)
        else:
            if not os.path.exists(p):
                disp = ".".join(n.modpath) if n.modpath else n.path
                raise ThrumError("cannot find module '%s'" % disp, n.site)
            with open(p, "r", encoding="utf-8") as fh:
                text = fh.read()
            self.modules[p] = None
            msc = Scope(self.builtins, rel=True)
            oldsrc, oldpath, self.src, self.path = self.src, self.path, text, p
            try:
                self.exec_block(parse(text), msc)
                self.drain()
            except Exception:
                self.modules.pop(p, None)
                raise
            finally:
                self.src, self.path = oldsrc, oldpath
            self.modules[p] = Module(os.path.basename(p), msc)
        if n.alias:
            name = n.alias
        elif n.modpath:
            name = n.modpath[-1]
        else:
            name = os.path.splitext(os.path.basename(p))[0]
        sc.vars[name] = self.modules[p]
        sc.const_names.add(name)

    def run(self, src, path=None):
        self.src, self.path = src, path
        try:
            self.exec_block(parse(src), self.globals)
            self.drain()
        except ReturnSig:
            raise ThrumError("'return' outside of function", None)
        except BreakSig:
            raise ThrumError("'break' outside of loop", None)
        except ContinueSig:
            raise ThrumError("'continue' outside of loop", None)


# ---------- copy-on-write paths ----------
def _idx(p, n, site=None):
    if not isinstance(p, Q) or not dimless(p.d) or not math.isfinite(p.v) or p.v != int(p.v):
        raise ThrumError("list index must be a whole number", site)
    i = int(p.v)
    if i < 0:
        i += n
    if not 0 <= i < n:
        raise ThrumError("index %d out of range (len %d)" % (i, n), site)
    return i


def getpath(v, path, site=None):
    for p in path:
        if isinstance(v, list):
            v = v[_idx(p, len(v), site)]
        elif isinstance(v, dict):
            if isinstance(p, (list, dict)):
                raise ThrumError("map keys cannot be %s" % tname(p), site)
            v = v.get(p)
        elif isinstance(v, RecordVal):
            if not isinstance(p, str) or p not in v.rtype.field_map:
                raise ThrumError("record '%s' has no field '%s'" % (v.rtype.name, vstr(p)), site)
            v = v.fields[p]
        else:
            raise ThrumError("cannot descend into %s" % tname(v), site)
    return v


def setpath(v, path, new, site=None, checktype=None):
    if not path:
        return new
    k = path[0]
    rest = path[1:]
    if isinstance(v, list):
        i = _idx(k, len(v), site)
        out = list(v)
        out[i] = setpath(v[i], rest, new, site, checktype=checktype)
        return out
    if isinstance(v, dict):
        if isinstance(k, (list, dict)):
            raise ThrumError("map keys cannot be %s" % tname(k), site)
        out = dict(v)
        out[k] = setpath(v.get(k), rest, new, site, checktype=checktype)
        return out
    if isinstance(v, RecordVal):
        if not isinstance(k, str) or k not in v.rtype.field_map:
            raise ThrumError("record '%s' has no field '%s'" % (v.rtype.name, vstr(k)), site)
        val = setpath(v.fields[k], rest, new, site, checktype=checktype)
        fld = v.rtype.field_map[k]
        if checktype and not rest:
            checktype(val, fld.type, "field '%s' of %s" % (k, v.rtype.name), site)
        fields = dict(v.fields)
        fields[k] = val
        return RecordVal(v.rtype, fields)
    raise ThrumError("cannot assign into %s" % tname(v), site)


def matchpat(pat, v, binds):
    t = pat[0]
    if t == "wild":
        return True
    if t == "lit":
        return veq(pat[1], v)
    if t == "bind":
        binds[pat[1]] = v
        return True
    if t == "record":
        if not isinstance(v, RecordVal) or v.rtype.name != pat[1]:
            return False
        for key, sub in pat[2]:
            if key not in v.fields:
                return False
            if not matchpat(sub, v.fields[key], binds):
                return False
        return True
    if t == "map":
        if not isinstance(v, dict):
            return False
        for key, sub in pat[1]:
            if key not in v:
                return False
            if not matchpat(sub, v[key], binds):
                return False
        return True
    if t == "list":
        if not isinstance(v, list):
            return False
        items, rest = pat[1], pat[2]
        if rest is None:
            if len(v) != len(items):
                return False
        elif len(v) < len(items):
            return False
        for p, x in zip(items, v):
            if not matchpat(p, x, binds):
                return False
        if rest is not None:
            binds[rest] = v[len(items) :]
        return True
    return False
