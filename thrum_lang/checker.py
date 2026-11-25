"""Static scope, capability, module, record, and conservative dimension/type checker."""

import os
from fractions import Fraction

from .builtins import BUILTIN_NAMES
from .caps import CAPS
from .errors import ThrumError
from .parser import parse
from .units import D0, _d, dadd, dimless, dimstr, dscale, dsub, parse_unit

STDLIB_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "stdlib")


class _Ty:
    pass


class _AnyTy(_Ty):
    def __repr__(self):
        return "any"


ANY = _AnyTy()


class NumTy(_Ty):
    __slots__ = ("dim",)

    def __init__(self, dim=None):
        self.dim = dim  # None means any numeric dimension; tuple means exact 7-dim

    def __repr__(self):
        if self.dim is None:
            return "num"
        return dimstr(self.dim) or "1"


class KindTy(_Ty):
    __slots__ = ("kind",)

    def __init__(self, kind):
        self.kind = kind

    def __repr__(self):
        return self.kind


class RecordTy(_Ty):
    __slots__ = ("name", "fields")

    def __init__(self, name, fields=None):
        self.name = name
        self.fields = fields or {}  # field -> (Ty, has_default)

    def __repr__(self):
        return self.name


class FnTy(_Ty):
    __slots__ = ("params", "ret")

    def __init__(self, params, ret):
        self.params = params  # list of (name, Ty, has_default)
        self.ret = ret

    def __repr__(self):
        return "fn"


class ModTy(_Ty):
    __slots__ = ("name", "exports")

    def __init__(self, name, exports=None):
        self.name = name
        self.exports = exports or {}

    def __repr__(self):
        return "module(%s)" % self.name


TEXT_TY = KindTy("text")
BOOL_TY = KindTy("bool")
LIST_TY = KindTy("list")
MAP_TY = KindTy("map")
NIL_TY = KindTy("nil")
DIMLESS_TY = NumTy(D0)


def _ty_from_ann(ann, sc, site):
    if ann is None:
        return ANY
    if ann[0] == "kind":
        if ann[1] == "any":
            return ANY
        if ann[1] == "num":
            return NumTy(None)
        return KindTy(ann[1])
    if ann[0] == "dim":
        return NumTy(ann[1])
    if ann[0] == "record":
        _, short_name, full_name = ann
        if "." in full_name:
            parts = full_name.split(".")
            cur_ty, _ = sc.lookup_ty(parts[0])
            for p in parts[1:]:
                if isinstance(cur_ty, ModTy):
                    cur_ty = cur_ty.exports.get(p, ANY)
                else:
                    cur_ty = ANY
            if isinstance(cur_ty, RecordTy):
                return cur_ty
            return RecordTy(short_name)
        ty, _ = sc.lookup_ty(short_name)
        if isinstance(ty, RecordTy):
            return ty
        raise ThrumError("unknown type or unit '%s'" % full_name, site)
    return ANY


def _ty_matches(actual, expected):
    if actual is ANY or expected is ANY:
        return True
    if isinstance(expected, NumTy):
        if not isinstance(actual, NumTy):
            return False
        if expected.dim is not None and actual.dim is not None:
            return expected.dim == actual.dim
        return True
    if isinstance(expected, RecordTy):
        return isinstance(actual, RecordTy) and actual.name == expected.name
    if isinstance(expected, KindTy):
        if expected.kind == "fn":
            return isinstance(actual, FnTy) or (isinstance(actual, KindTy) and actual.kind == "fn")
        return isinstance(actual, KindTy) and actual.kind == expected.kind
    return True


def _ty_label(ty):
    if isinstance(ty, NumTy):
        if ty.dim is None:
            return "num"
        return dimstr(ty.dim) or "dimensionless"
    return repr(ty)


def _stdmod_exports(name):
    if name == "math":
        ex = {
            "pi": DIMLESS_TY, "tau": DIMLESS_TY, "e": DIMLESS_TY, "inf": DIMLESS_TY, "nan": DIMLESS_TY, "phi": DIMLESS_TY,
            "gcd": FnTy([("a", DIMLESS_TY, False), ("b", DIMLESS_TY, False)], DIMLESS_TY),
            "lcm": FnTy([("a", DIMLESS_TY, False), ("b", DIMLESS_TY, False)], DIMLESS_TY),
            "factorial": FnTy([("n", DIMLESS_TY, False)], DIMLESS_TY),
            "comb": FnTy([("n", DIMLESS_TY, False), ("k", DIMLESS_TY, False)], DIMLESS_TY),
            "perm": FnTy([("n", DIMLESS_TY, False), ("k", DIMLESS_TY, False)], DIMLESS_TY),
            "is_prime": FnTy([("n", DIMLESS_TY, False)], BOOL_TY),
            "isfinite": FnTy([("x", NumTy(None), False)], BOOL_TY),
            "isnan": FnTy([("x", NumTy(None), False)], BOOL_TY),
            "isinf": FnTy([("x", NumTy(None), False)], BOOL_TY),
        }
        for fnm in ("sin", "cos", "tan", "asin", "acos", "atan", "exp", "expm1", "ln", "log1p", "log10", "log2", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh"):
            ex[fnm] = FnTy([("x", DIMLESS_TY, False)], DIMLESS_TY)
        return ex
    if name == "const":
        return {
            "c": NumTy(_d(m=1, s=-1)),
            "h": NumTy(_d(kg=1, m=2, s=-1)),
            "hbar": NumTy(_d(kg=1, m=2, s=-1)),
            "kB": NumTy(_d(kg=1, m=2, s=-2, K=-1)),
            "NA": NumTy(_d(mol=-1)),
            "e": NumTy(_d(A=1, s=1)),
            "G": NumTy(_d(m=3, kg=-1, s=-2)),
            "eps0": NumTy(_d(kg=-1, m=-3, s=4, A=2)),
            "mu0": NumTy(_d(kg=1, m=1, s=-2, A=-2)),
            "me": NumTy(_d(kg=1)),
            "mp": NumTy(_d(kg=1)),
            "mn": NumTy(_d(kg=1)),
            "u": NumTy(_d(kg=1)),
            "alpha": DIMLESS_TY,
            "Ry": NumTy(_d(m=-1)),
            "F": NumTy(_d(A=1, s=1, mol=-1)),
            "muB": NumTy(_d(m=2, A=1)),
            "R": NumTy(_d(kg=1, m=2, s=-2, K=-1, mol=-1)),
            "g0": NumTy(_d(m=1, s=-2)),
            "atm": NumTy(_d(kg=1, m=-1, s=-2)),
            "sigma": NumTy(_d(kg=1, s=-3, K=-4)),
            "au": NumTy(_d(m=1)),
            "ly": NumTy(_d(m=1)),
            "pc": NumTy(_d(m=1)),
            "M_sun": NumTy(_d(kg=1)),
            "M_earth": NumTy(_d(kg=1)),
            "R_earth": NumTy(_d(m=1)),
            "L_sun": NumTy(_d(kg=1, m=2, s=-3)),
            "T_cmb": NumTy(_d(K=1)),
        }
    return {}


class _CheckScope:
    def __init__(self, parent=None, rel=False):
        self.names = {}
        self.types = {}
        self.parent = parent
        self.rel = rel

    def lookup(self, name):
        s = self
        while s is not None:
            if name in s.names:
                return s.names[name], s
            s = s.parent
        return None, None

    def lookup_ty(self, name):
        s = self
        while s is not None:
            if name in s.types:
                return s.types[name], s
            s = s.parent
        return ANY, None


def resolve_module_path(n, base_file_path):
    if n.modpath is not None:
        if len(n.modpath) == 2 and n.modpath[0] == "std":
            return os.path.abspath(os.path.join(STDLIB_DIR, n.modpath[1] + ".thrum"))
        raise ThrumError("unknown module path '%s'" % ".".join(n.modpath), n.site)
    if n.path is not None:
        if n.path.startswith("std/"):
            rel = n.path[4:]
            if not rel.endswith(".thrum"):
                rel += ".thrum"
            return os.path.abspath(os.path.join(STDLIB_DIR, rel))
        base = os.path.dirname(os.path.abspath(base_file_path)) if base_file_path else os.getcwd()
        return os.path.abspath(os.path.join(base, n.path))
    return None


def check_program(prog, path=None, seen=None, stdmods=None):
    seen = seen if seen is not None else {}
    stdmod_names = set(stdmods) if stdmods is not None else set(
        "math const stat units text regex json csv time path list map test".split()
    )
    root = _CheckScope(rel=True)
    for b in BUILTIN_NAMES:
        root.names[b] = "builtin"
        root.types[b] = ANY

    def check_expr(n, sc, deferred):
        k = n.kind
        if k == "q":
            v = n.value
            if v is None:
                return NIL_TY
            if isinstance(v, bool):
                return BOOL_TY
            if isinstance(v, str):
                return TEXT_TY
            return NumTy(v.d)
        if k == "name":
            kind, _ = sc.lookup(n.name)
            if kind is None:
                raise ThrumError("undefined name '%s'" % n.name, n.site)
            ty, _ = sc.lookup_ty(n.name)
            return ty
        if k == "text":
            for t, p in n.parts:
                if t == "expr":
                    check_expr(p, sc, deferred)
            return TEXT_TY
        if k == "list":
            for x in n.items:
                check_expr(x, sc, deferred)
            return LIST_TY
        if k == "map":
            for kn, vn in n.entries:
                check_expr(kn, sc, deferred)
                check_expr(vn, sc, deferred)
            return MAP_TY
        if k == "bin":
            return check_bin(n, sc, deferred)
        if k == "un":
            vt = check_expr(n.operand, sc, deferred)
            if n.op == "not":
                return BOOL_TY
            if vt is not ANY and not isinstance(vt, NumTy):
                raise ThrumError("cannot negate %s" % _ty_label(vt), n.site)
            return vt
        if k == "convert":
            ot = check_expr(n.obj, sc, deferred)
            _, td = parse_unit(n.unit)
            if isinstance(ot, NumTy) and ot.dim is not None and ot.dim != td:
                raise ThrumError("cannot convert %s to %s" % (_ty_label(ot), n.unit), n.site)
            return NumTy(td)
        if k == "call":
            ft = check_expr(n.fn, sc, deferred)
            arg_tys = [check_expr(a, sc, deferred) for a in n.args]
            # Builtin dimension propagation for common functions
            if n.fn.kind == "name":
                fnm = n.fn.name
                if fnm == "sqrt" and len(arg_tys) == 1:
                    at = arg_tys[0]
                    if isinstance(at, NumTy) and at.dim is not None:
                        return NumTy(dscale(at.dim, Fraction(1, 2)))
                    return NumTy(None)
                if fnm in ("abs", "floor", "ceil", "round", "unc", "si", "exact") and arg_tys:
                    if isinstance(arg_tys[0], NumTy):
                        return arg_tys[0]
                    return NumTy(None)
                if fnm in ("bare", "len", "sign", "count"):
                    return DIMLESS_TY
                if fnm in ("str", "repr", "type", "unit", "dim", "upper", "lower", "trim", "replace", "join", "fix", "fmt"):
                    return TEXT_TY
                if fnm == "bool" or fnm == "isnil" or fnm == "approx" or fnm in ("starts", "ends", "any", "all"):
                    return BOOL_TY
                if fnm == "c2k" or fnm == "k2c":
                    return NumTy(_d(K=1))
                if fnm == "qty" and len(n.args) >= 2 and n.args[1].kind == "q" and isinstance(n.args[1].value, str):
                    _, qd = parse_unit(n.args[1].value)
                    return NumTy(qd)
                if fnm == "convert" and len(n.args) == 2 and n.args[1].kind == "q" and isinstance(n.args[1].value, str):
                    _, qd = parse_unit(n.args[1].value)
                    if isinstance(arg_tys[0], NumTy) and arg_tys[0].dim is not None and arg_tys[0].dim != qd:
                        raise ThrumError("cannot convert %s to %s" % (_ty_label(arg_tys[0]), n.args[1].value), n.site)
                    return NumTy(qd)
            if isinstance(ft, RecordTy):
                nreq = sum(1 for _, (_, has_def) in ft.fields.items() if not has_def)
                if len(arg_tys) < nreq or len(arg_tys) > len(ft.fields):
                    raise ThrumError(
                        "record %s expects %d..%d arguments, got %d" % (ft.name, nreq, len(ft.fields), len(arg_tys)),
                        n.site,
                    )
                for (fname, (fty, _)), aty in zip(ft.fields.items(), arg_tys):
                    if not _ty_matches(aty, fty):
                        raise ThrumError(
                            "field '%s' of %s should be %s, got %s" % (fname, ft.name, _ty_label(fty), _ty_label(aty)),
                            n.site,
                        )
                return ft
            if isinstance(ft, FnTy):
                nreq = sum(1 for _, _, hd in ft.params if not hd)
                if len(arg_tys) < nreq or len(arg_tys) > len(ft.params):
                    raise ThrumError(
                        "function expects %d..%d arguments, got %d" % (nreq, len(ft.params), len(arg_tys)),
                        n.site,
                    )
                for (pname, pty, _), aty in zip(ft.params, arg_tys):
                    if not _ty_matches(aty, pty):
                        if isinstance(pty, NumTy) and isinstance(aty, NumTy):
                            raise ThrumError(
                                "parameter '%s' should have dimension %s, got %s" % (pname, _ty_label(pty), _ty_label(aty)),
                                n.site,
                            )
                        raise ThrumError(
                            "parameter '%s' should be %s, got %s" % (pname, _ty_label(pty), _ty_label(aty)),
                            n.site,
                        )
                return ft.ret
            return ANY
        if k == "rec_lit":
            rt = check_expr(n.target, sc, deferred)
            seen_keys = set()
            for kn, vn in n.entries:
                check_expr(kn, sc, deferred)
                vty = check_expr(vn, sc, deferred)
                if isinstance(rt, RecordTy) and kn.kind == "q" and isinstance(kn.value, str):
                    fnm = kn.value
                    if fnm not in rt.fields:
                        raise ThrumError("record '%s' has no field '%s'" % (rt.name, fnm), n.site)
                    seen_keys.add(fnm)
                    fty, _ = rt.fields[fnm]
                    if not _ty_matches(vty, fty):
                        raise ThrumError(
                            "field '%s' of %s should be %s, got %s" % (fnm, rt.name, _ty_label(fty), _ty_label(vty)),
                            n.site,
                        )
            if isinstance(rt, RecordTy):
                for fnm, (_, has_def) in rt.fields.items():
                    if not has_def and fnm not in seen_keys:
                        raise ThrumError("missing required field '%s' for record '%s'" % (fnm, rt.name), n.site)
                return rt
            return ANY
        if k == "index":
            check_expr(n.obj, sc, deferred)
            check_expr(n.idx, sc, deferred)
            return ANY
        if k == "slice":
            ot = check_expr(n.obj, sc, deferred)
            if n.start is not None:
                check_expr(n.start, sc, deferred)
            if n.stop is not None:
                check_expr(n.stop, sc, deferred)
            if n.step is not None:
                check_expr(n.step, sc, deferred)
            return ot if isinstance(ot, KindTy) and ot.kind in ("list", "text") else ANY
        if k == "dot":
            ot = check_expr(n.obj, sc, deferred)
            if isinstance(ot, ModTy):
                return ot.exports.get(n.name, ANY)
            if isinstance(ot, RecordTy):
                if n.name not in ot.fields:
                    raise ThrumError("record '%s' has no field '%s'" % (ot.name, n.name), n.site)
                return ot.fields[n.name][0]
            return ANY
        if k == "lambda":
            sub = _CheckScope(sc, rel=False)
            sub.names["it"] = "param"
            sub.types["it"] = ANY
            deferred.append(("expr", n.body, sub))
            return FnTy([("it", ANY, False)], ANY)
        if k == "fnexpr":
            sub = _CheckScope(sc, rel=False)
            ptys = []
            for p in n.params:
                if p.default is not None:
                    check_expr(p.default, sub, deferred)
                pty = _ty_from_ann(p.type, sc, p.site)
                sub.names[p.name] = "param"
                sub.types[p.name] = pty
                ptys.append((p.name, pty, p.default is not None))
            rty = _ty_from_ann(n.ret, sc, n.site)
            deferred.append(("fn", n, sub, rty))
            return FnTy(ptys, rty)
        if k == "comp":
            check_expr(n.iter, sc, deferred)
            sub = _CheckScope(sc, rel=False)
            sub.names[n.var] = "local"
            sub.types[n.var] = ANY
            if n.cond is not None:
                check_expr(n.cond, sub, deferred)
            check_expr(n.elem, sub, deferred)
            return LIST_TY
        raise ThrumError("cannot check %s" % k, n.site)

    def check_bin(n, sc, deferred):
        op = n.op
        if op in ("and", "or"):
            check_expr(n.left, sc, deferred)
            check_expr(n.right, sc, deferred)
            return BOOL_TY
        if op == "??":
            lt = check_expr(n.left, sc, deferred)
            rt = check_expr(n.right, sc, deferred)
            return rt if lt == NIL_TY else (lt if lt == rt else ANY)
        lt = check_expr(n.left, sc, deferred)
        rt = check_expr(n.right, sc, deferred)
        if op in ("==", "!=", "~"):
            return BOOL_TY
        if op in ("in", "notin"):
            return BOOL_TY
        if op in ("..", "..<"):
            return LIST_TY
        if op in ("<", ">", "<=", ">="):
            if isinstance(lt, NumTy) and isinstance(rt, NumTy) and lt.dim is not None and rt.dim is not None and lt.dim != rt.dim:
                raise ThrumError("cannot compare %s with %s" % (_ty_label(lt), _ty_label(rt)), n.site)
            return BOOL_TY
        if op in ("+", "-"):
            if isinstance(lt, NumTy) and isinstance(rt, NumTy):
                if lt.dim is not None and rt.dim is not None:
                    if lt.dim != rt.dim:
                        raise ThrumError(
                            "dimension mismatch: %s %s %s" % (_ty_label(lt), op, _ty_label(rt)),
                            n.site,
                            "convert with 'as', or check your units — Thrum never guesses",
                        )
                    return NumTy(lt.dim)
                return NumTy(lt.dim if lt.dim is not None else rt.dim)
            if op == "+" and isinstance(lt, KindTy) and isinstance(rt, KindTy) and lt.kind == rt.kind and lt.kind in ("text", "list", "map"):
                return lt
            if lt is not ANY and rt is not ANY and type(lt) is not type(rt):
                raise ThrumError(
                    "cannot %s %s and %s" % ("add" if op == "+" else "subtract", _ty_label(lt), _ty_label(rt)),
                    n.site,
                )
            return ANY
        if op == "*":
            if isinstance(lt, NumTy) and isinstance(rt, NumTy):
                if lt.dim is not None and rt.dim is not None:
                    return NumTy(dadd(lt.dim, rt.dim))
                return NumTy(None)
            return ANY
        if op == "/":
            if isinstance(lt, NumTy) and isinstance(rt, NumTy):
                if lt.dim is not None and rt.dim is not None:
                    return NumTy(dsub(lt.dim, rt.dim))
                return NumTy(None)
            return ANY
        if op == "%":
            if isinstance(lt, NumTy) and isinstance(rt, NumTy) and lt.dim is not None and rt.dim is not None and lt.dim != rt.dim:
                raise ThrumError("dimension mismatch in '%'", n.site)
            return lt if isinstance(lt, NumTy) else NumTy(None)
        if op == "^":
            if isinstance(rt, NumTy) and rt.dim is not None and not dimless(rt.dim):
                raise ThrumError("exponent must be dimensionless", n.site)
            if isinstance(lt, NumTy) and lt.dim is not None and n.right.kind == "q" and hasattr(n.right.value, "v"):
                e = Fraction(n.right.value.v).limit_denominator(1000)
                return NumTy(dscale(lt.dim, e))
            return DIMLESS_TY if (isinstance(lt, NumTy) and lt.dim is not None and dimless(lt.dim)) else NumTy(None)
        return ANY

    def pat_bind(pat, sc):
        t = pat[0]
        if t == "bind":
            sc.names[pat[1]] = "local"
            sc.types[pat[1]] = ANY
        elif t == "map":
            for _, sub in pat[1]:
                pat_bind(sub, sc)
        elif t == "record":
            for _, sub in pat[2]:
                pat_bind(sub, sc)
        elif t == "list":
            for sub in pat[1]:
                pat_bind(sub, sc)
            if pat[2] is not None:
                sc.names[pat[2]] = "local"
                sc.types[pat[2]] = LIST_TY

    def run_deferred(deferred):
        for item in deferred:
            if item[0] == "expr":
                _, ex, sub = item
                d2 = []
                check_expr(ex, sub, d2)
                run_deferred(d2)
            elif item[0] == "block":
                _, body, sub = item
                check_block(body, sub)
            elif item[0] == "fn":
                _, fn, sub, expected_ret = item
                d2 = []
                for c in fn.requires:
                    check_expr(c.cond, sub, d2)
                    if c.msg is not None:
                        check_expr(c.msg, sub, d2)
                run_deferred(d2)
                check_block(fn.body, sub, expected_ret=expected_ret, fn_name=fn.name or "fn")
                ens_sc = _CheckScope(sub, rel=False)
                ens_sc.names["result"] = "local"
                ens_sc.types["result"] = expected_ret
                d3 = []
                for c in fn.ensures:
                    check_expr(c.cond, ens_sc, d3)
                    if c.msg is not None:
                        check_expr(c.msg, ens_sc, d3)
                run_deferred(d3)

    def check_stmt(n, sc, deferred, in_settle=False, expected_ret=None, fn_name=None):
        k = n.kind
        if k == "exprstmt":
            check_expr(n.value, sc, deferred)
            return
        if k == "decl":
            if n.init is not None and not in_settle:
                raise ThrumError("'init' is only allowed inside 'settle'", n.site)
            if n.init is not None:
                check_expr(n.init, sc, deferred)
            vty = check_expr(n.value, sc, deferred)
            if not in_settle:
                sc.names[n.name] = n.kw
                sc.types[n.name] = vty
            return
        if k == "record":
            fields = {}
            for f in n.fields:
                if f.default is not None:
                    check_expr(f.default, sc, deferred)
                fty = _ty_from_ann(f.type, sc, f.site)
                fields[f.name] = (fty, f.default is not None)
            rty = RecordTy(n.name, fields)
            sc.names[n.name] = "record"
            sc.types[n.name] = rty
            return
        if k == "assign":
            vty = check_expr(n.value, sc, deferred)
            cur = n.target
            while cur.kind in ("index", "dot"):
                if cur.kind == "index":
                    check_expr(cur.idx, sc, deferred)
                cur = cur.obj
            if cur.kind != "name":
                raise ThrumError("invalid assignment target", n.site)
            kind, fsc = sc.lookup(cur.name)
            if kind is None:
                raise ThrumError("undefined name '%s'" % cur.name, cur.site, "declare inputs with 'var'")
            if kind == "let":
                if fsc.rel:
                    raise ThrumError(
                        "'%s' is a derived relation, not an input" % cur.name,
                        n.site,
                        "assign to its inputs instead, or declare it with 'var'",
                    )
                raise ThrumError(
                    "cannot assign to immutable '%s'" % cur.name,
                    n.site,
                    "declare it with 'var' if it needs to change",
                )
            if kind in ("pin", "fn", "cap", "builtin", "record") or (kind == "module" and n.target.kind == "name"):
                raise ThrumError(
                    "cannot assign to immutable '%s'" % cur.name,
                    n.site,
                    "declare it with 'var' if it needs to change",
                )
            if n.target.kind == "name" and n.op == "=":
                old_ty = fsc.types.get(cur.name, ANY)
                if old_ty is not ANY and vty is not ANY and not _ty_matches(vty, old_ty):
                    fsc.types[cur.name] = ANY
            return
        if k == "fndecl":
            sub = _CheckScope(sc, rel=False)
            ptys = []
            for p in n.params:
                if p.default is not None:
                    check_expr(p.default, sub, deferred)
                pty = _ty_from_ann(p.type, sc, p.site)
                sub.names[p.name] = "param"
                sub.types[p.name] = pty
                ptys.append((p.name, pty, p.default is not None))
            rty = _ty_from_ann(n.ret, sc, n.site)
            sc.names[n.name] = "fn"
            sc.types[n.name] = FnTy(ptys, rty)
            deferred.append(("fn", n, sub, rty))
            return
        if k == "if":
            for cond, body in n.arms:
                check_expr(cond, sc, deferred)
                check_block(body, sc, expected_ret=expected_ret, fn_name=fn_name)
            if n.els:
                check_block(n.els, sc, expected_ret=expected_ret, fn_name=fn_name)
            return
        if k == "for":
            check_expr(n.iter, sc, deferred)
            sub = _CheckScope(sc, rel=False)
            sub.names[n.a] = "local"
            sub.types[n.a] = ANY
            if n.b:
                sub.names[n.b] = "local"
                sub.types[n.b] = ANY
            check_block(n.body, sub, expected_ret=expected_ret, fn_name=fn_name)
            return
        if k == "while":
            check_expr(n.cond, sc, deferred)
            check_block(n.body, sc, expected_ret=expected_ret, fn_name=fn_name)
            return
        if k == "repeat":
            check_expr(n.count, sc, deferred)
            check_block(n.body, sc, expected_ret=expected_ret, fn_name=fn_name)
            return
        if k == "match":
            check_expr(n.subject, sc, deferred)
            for pat, guard, body in n.cases:
                sub = _CheckScope(sc, rel=False)
                pat_bind(pat, sub)
                if guard is not None:
                    check_expr(guard, sub, deferred)
                check_block(body, sub, expected_ret=expected_ret, fn_name=fn_name)
            return
        if k == "try":
            check_block(n.body, sc, expected_ret=expected_ret, fn_name=fn_name)
            sub = _CheckScope(sc, rel=sc.rel)
            sub.names[n.name] = "local"
            sub.types[n.name] = MAP_TY
            check_block(n.handler, sub, expected_ret=expected_ret, fn_name=fn_name)
            return
        if k == "return":
            rty = check_expr(n.value, sc, deferred) if n.value is not None else NIL_TY
            if expected_ret is not None and expected_ret is not ANY and not _ty_matches(rty, expected_ret):
                if isinstance(expected_ret, NumTy) and isinstance(rty, NumTy):
                    raise ThrumError(
                        "return of %s should have dimension %s, got %s" % (fn_name or "fn", _ty_label(expected_ret), _ty_label(rty)),
                        n.site,
                    )
                raise ThrumError(
                    "return of %s should be %s, got %s" % (fn_name or "fn", _ty_label(expected_ret), _ty_label(rty)),
                    n.site,
                )
            return
        if k in ("break", "continue"):
            return
        if k == "fail":
            check_expr(n.value, sc, deferred)
            return
        if k == "assert":
            check_expr(n.cond, sc, deferred)
            if n.msg is not None:
                check_expr(n.msg, sc, deferred)
            return
        if k == "with":
            sub = _CheckScope(sc, rel=sc.rel)
            for nm, args in n.caps:
                if nm not in CAPS:
                    raise ThrumError("unknown capability '%s'" % nm, n.site, "available: " + ", ".join(sorted(CAPS)))
                for a in args:
                    check_expr(a, sc, deferred)
                sub.names[nm] = "cap"
                sub.types[nm] = KindTy("cap")
            check_block(n.body, sub, expected_ret=expected_ret, fn_name=fn_name)
            return
        if k == "on":
            check_expr(n.cond, sc, deferred)
            sub = _CheckScope(sc, rel=False)
            deferred.append(("block", n.body, sub))
            return
        if k == "settle":
            if not sc.rel:
                raise ThrumError("settle needs a relation scope", n.site)
            if n.tol is not None:
                check_expr(n.tol, sc, deferred)
            for st in n.body:
                if st.kind != "decl":
                    raise ThrumError("only declarations allowed inside settle", st.site)
                sc.names[st.name] = st.kw
                sc.types[st.name] = ANY
            for st in n.body:
                check_stmt(st, sc, deferred, in_settle=True)
            return
        if k == "live":
            sub = sc if sc.rel else _CheckScope(sc, rel=True)
            check_block(n.body, sub)
            return
        if k == "whatif":
            for nm, ex in n.pairs:
                kind, _ = sc.lookup(nm)
                if kind is None or kind not in ("var", "let"):
                    raise ThrumError("whatif needs a live binding, '%s' isn't one" % nm, n.site)
                check_expr(ex, sc, deferred)
            sub = _CheckScope(sc, rel=False)
            check_block(n.body, sub)
            return
        if k in ("why", "budget"):
            kind, _ = sc.lookup(n.name)
            if kind is None:
                raise ThrumError("undefined name '%s'" % n.name, n.site)
            if k == "budget" and kind not in ("var", "let"):
                raise ThrumError("budget needs a live binding", n.site)
            return
        if k == "use":
            if n.path is None and n.modpath is None:
                if n.name not in stdmod_names:
                    raise ThrumError("unknown module '%s'" % n.name, n.site, "available: " + ", ".join(sorted(stdmod_names)))
                nm = n.alias or n.name
                sc.names[nm] = "module"
                sc.types[nm] = ModTy(n.name, _stdmod_exports(n.name))
                return
            p = resolve_module_path(n, path)
            if not os.path.exists(p):
                disp = ".".join(n.modpath) if n.modpath else n.path
                raise ThrumError("cannot find module '%s'" % disp, n.site)
            if p not in seen:
                seen[p] = {}
                with open(p, "r", encoding="utf-8") as fh:
                    text = fh.read()
                exports = check_program(parse(text), p, seen, stdmods=stdmod_names)
                seen[p] = exports
            if n.alias:
                name = n.alias
            elif n.modpath:
                name = n.modpath[-1]
            else:
                name = os.path.splitext(os.path.basename(p))[0]
            sc.names[name] = "module"
            sc.types[name] = ModTy(name, seen[p])
            return
        if k == "contract":
            return

    def check_block(body, sc, expected_ret=None, fn_name=None):
        deferred = []
        for st in body:
            check_stmt(st, sc, deferred, expected_ret=expected_ret, fn_name=fn_name)
        run_deferred(deferred)

    check_block(prog, root)
    return dict(root.types)
