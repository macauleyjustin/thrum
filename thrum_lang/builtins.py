"""Core builtin functions."""

import math

from .errors import ThrumError
from .units import _d, dimlabel, dimless, dimstr, parse_unit
from .values import Q, Builtin, convert, mkq, qadd, qapprox, qcmp, qdiv, qmul, qpow, tname, veq, vstr, truthy

BUILTIN_NAMES = set()


def install_builtins(I):
    B = I.builtins.vars

    def reg(name, arity, f):
        B[name] = Builtin(name, f, arity)
        BUILTIN_NAMES.add(name)

    def q1(f):
        return lambda I, a, s: f(I.needq(a[0], s))

    def _err(m, s):
        raise ThrumError(m, s)

    def needlist(v, s, what="list"):
        if not isinstance(v, list):
            raise ThrumError("%s needs a list, got %s" % (what, tname(v)), s)
        return v

    def needstr(v, s, what="text"):
        if not isinstance(v, str):
            raise ThrumError("%s needs text, got %s" % (what, tname(v)), s)
        return v

    reg("len", (1,), lambda I, a, s: mkq(float(len(a[0]))) if isinstance(a[0], (list, str, dict)) else _err("len needs a list, text or map", s))
    reg("type", (1,), lambda I, a, s: tname(a[0]))
    reg("str", (1,), lambda I, a, s: vstr(a[0]))
    reg("repr", (1,), lambda I, a, s: vstr(a[0], True))
    reg("num", (1, 2), _num)
    reg("bool", (1,), lambda I, a, s: truthy(a[0]))
    reg("isnil", (1,), lambda I, a, s: a[0] is None)
    reg("range", (1, 2, 3), _range)
    reg(
        "list",
        (1,),
        lambda I, a, s: list(a[0])
        if isinstance(a[0], (list, str))
        else ([[k, v] for k, v in a[0].items()] if isinstance(a[0], dict) else [a[0]]),
    )
    reg("keys", (1,), lambda I, a, s: list(a[0].keys()) if isinstance(a[0], dict) else _err("keys needs a map", s))
    reg("vals", (1,), lambda I, a, s: list(a[0].values()) if isinstance(a[0], dict) else _err("vals needs a map", s))
    reg("items", (1,), lambda I, a, s: [[k, v] for k, v in a[0].items()] if isinstance(a[0], dict) else _err("items needs a map", s))
    reg("push", (2,), lambda I, a, s: needlist(a[0], s, "push") + [a[1]])
    reg("pop", (1,), lambda I, a, s: needlist(a[0], s, "pop")[:-1] if a[0] else _err("pop from empty list", s))
    reg(
        "concat",
        (2,),
        lambda I, a, s: a[0] + a[1]
        if isinstance(a[0], (list, str)) and type(a[0]) is type(a[1])
        else _err("concat needs two lists or two texts", s),
    )
    reg("slice", (2, 3), _slice)
    reg("reverse", (1,), lambda I, a, s: a[0][::-1] if isinstance(a[0], (list, str)) else _err("reverse needs a list or text", s))
    reg("sort", (1, 2), lambda I, a, s: _sort(I, a, s))
    reg("uniq", (1,), _uniq)
    reg("flatten", (1,), lambda I, a, s: [x for sub in needlist(a[0], s, "flatten") for x in (sub if isinstance(sub, list) else [sub])])
    reg("zip", (2,), lambda I, a, s: [[x, y] for x, y in zip(needlist(a[0], s, "zip"), needlist(a[1], s, "zip"))])
    reg(
        "enumerate",
        (1,),
        lambda I, a, s: [[mkq(float(i)), v] for i, v in enumerate(needlist(a[0], s, "enumerate") if isinstance(a[0], list) else list(needstr(a[0], s, "enumerate")))],
    )
    reg("map", (2,), lambda I, a, s: [I.call(a[1], [x], s) for x in needlist(a[0], s, "map")])
    reg("filter", (2,), lambda I, a, s: [x for x in needlist(a[0], s, "filter") if truthy(I.call(a[1], [x], s))])
    reg("find", (2,), lambda I, a, s: next((x for x in needlist(a[0], s, "find") if truthy(I.call(a[1], [x], s))), None))
    reg("count", (2,), lambda I, a, s: mkq(float(sum(1 for x in needlist(a[0], s, "count") if truthy(I.call(a[1], [x], s))))))
    reg("reduce", (3,), _reduce)
    reg("any", (1, 2), lambda I, a, s: any(truthy(x) if len(a) == 1 else truthy(I.call(a[1], [x], s)) for x in needlist(a[0], s, "any")))
    reg("all", (1, 2), lambda I, a, s: all(truthy(x) if len(a) == 1 else truthy(I.call(a[1], [x], s)) for x in needlist(a[0], s, "all")))
    reg("sum", (1,), lambda I, a, s: _fold(needlist(a[0], s, "sum"), lambda x, y: qadd(I.needq(x, s), I.needq(y, s), 1, s), mkq(0.0)))
    reg("product", (1,), lambda I, a, s: _fold(needlist(a[0], s, "product"), lambda x, y: qmul(I.needq(x, s), I.needq(y, s)), mkq(1.0)))
    reg("min", (1,), lambda I, a, s: _extreme(I, needlist(a[0], s, "min"), s, True))
    reg("max", (1,), lambda I, a, s: _extreme(I, needlist(a[0], s, "max"), s, False))
    reg(
        "mean",
        (1,),
        lambda I, a, s: qdiv(_fold(needlist(a[0], s, "mean"), lambda x, y: qadd(I.needq(x, s), I.needq(y, s), 1, s), mkq(0.0)), mkq(float(len(a[0]))), s)
        if a[0]
        else _err("mean of empty list", s),
    )
    reg("median", (1,), _median)
    reg("stdev", (1,), _stdev)
    reg("abs", (1,), q1(lambda x: mkq(abs(x.v), x.d, x.u, x.disp)))
    reg("sqrt", (1,), q1(lambda x: qpow(x, mkq(0.5))))
    reg("sign", (1,), q1(lambda x: mkq(float((x.v > 0) - (x.v < 0)))))
    reg("floor", (1,), q1(lambda x: _floor_ceil(x, math.floor)))
    reg("ceil", (1,), q1(lambda x: _floor_ceil(x, math.ceil)))
    reg("round", (1, 2), _round)
    reg("clamp", (3,), lambda I, a, s: _extreme(I, [a[1], _extreme(I, [a[0], a[2]], s, True)], s, False))
    reg("lerp", (3,), lambda I, a, s: qadd(I.needq(a[0], s), qmul(qadd(I.needq(a[1], s), I.needq(a[0], s), -1, s), I.needq(a[2], s)), 1, s))
    reg("approx", (2, 3), lambda I, a, s: qapprox(I.needq(a[0], s), I.needq(a[1], s), I.needq(a[2], s).v if len(a) > 2 else 3.0))
    reg("unit", (1,), q1(lambda x: (x.disp[1] if x.disp else dimstr(x.d)) or "1"))
    reg("dim", (1,), q1(lambda x: dimstr(x.d) or "1"))
    reg("unc", (1,), q1(lambda x: mkq(x.u, x.d, 0.0, x.disp)))
    reg("bare", (1,), q1(lambda x: mkq(x.v)))
    reg("si", (1,), q1(lambda x: mkq(x.v, x.d, x.u)))
    reg("exact", (1,), q1(lambda x: mkq(x.v, x.d, 0.0, x.disp)))
    reg("with_unc", (2,), _with_unc)
    reg("qty", (2, 3), _qty)
    reg("convert", (2,), lambda I, a, s: convert(I.needq(a[0], s), needstr(a[1], s, "convert"), s))
    reg("c2k", (1,), q1(lambda x: mkq(x.v + 273.15, _d(K=1), x.u, (1.0, "K"))))
    reg("k2c", (1,), q1(lambda x: mkq(x.v - 273.15, _d(K=1), x.u, (1.0, "K"))))
    reg("split", (1, 2), lambda I, a, s: needstr(a[0], s, "split").split(needstr(a[1], s, "split")) if len(a) > 1 else needstr(a[0], s, "split").split())
    reg("join", (2,), lambda I, a, s: needstr(a[0], s, "join").join(vstr(x) for x in needlist(a[1], s, "join")))
    reg("upper", (1,), lambda I, a, s: needstr(a[0], s, "upper").upper())
    reg("lower", (1,), lambda I, a, s: needstr(a[0], s, "lower").lower())
    reg("trim", (1,), lambda I, a, s: needstr(a[0], s, "trim").strip())
    reg("replace", (3,), lambda I, a, s: needstr(a[0], s, "replace").replace(needstr(a[1], s, "replace"), needstr(a[2], s, "replace")))
    reg("starts", (2,), lambda I, a, s: needstr(a[0], s, "starts").startswith(needstr(a[1], s, "starts")))
    reg("ends", (2,), lambda I, a, s: needstr(a[0], s, "ends").endswith(needstr(a[1], s, "ends")))
    reg("fix", (2,), _fixfmt)
    reg("fmt", (1, 2), lambda I, a, s: vstr(a[0]))
    reg("fail", (1,), lambda I, a, s: _err(vstr(a[0]), s))


def _num(I, a, s):
    x = a[0]
    if len(a) == 2:
        if not isinstance(a[1], str):
            raise ThrumError("convert unit must be text", s)
        return convert(I.needq(x, s), a[1], s)
    if isinstance(x, Q):
        return x
    if isinstance(x, (int, float)) and not isinstance(x, bool):
        return mkq(float(x))
    if isinstance(x, str):
        try:
            return mkq(float(x))
        except ValueError:
            pass
    raise ThrumError("cannot convert %s to num" % tname(x), s)


def _floor_ceil(x, fn):
    sc = x.disp[0] if x.disp else 1.0
    if sc == 0:
        sc = 1.0
    return mkq(fn(x.v / sc) * sc, x.d, x.u, x.disp)


def _with_unc(I, a, s):
    x = I.needq(a[0], s)
    u = I.needq(a[1], s)
    if not dimless(u.d) and u.d != x.d:
        raise ThrumError("uncertainty dimension %s does not match %s" % (dimlabel(u), dimlabel(x)), s)
    uv = u.v if u.d == x.d else u.v * (x.disp[0] if x.disp else 1.0)
    return mkq(x.v, x.d, abs(uv), x.disp)


def _range(I, a, s):
    xs = [I.needint(x, s) for x in a]
    if len(xs) == 1:
        i, j, st = 0, xs[0], 1
    elif len(xs) == 2:
        i, j, st = xs[0], xs[1], 1
    else:
        i, j, st = xs
    if st == 0:
        raise ThrumError("range step cannot be 0", s)
    return [mkq(float(x)) for x in range(i, j, st)]


def _slice(I, a, s):
    if not isinstance(a[0], (list, str)):
        raise ThrumError("slice needs list or text", s)
    i = I.needint(a[1], s)
    j = I.needint(a[2], s) if len(a) > 2 else len(a[0])
    return a[0][i:j]


def _sort(I, a, s):
    if not isinstance(a[0], list):
        raise ThrumError("sort needs a list", s)
    xs = list(a[0])
    if len(xs) <= 1 and len(a) == 1:
        return xs
    if len(a) > 1:
        decorated = [(I.call(a[1], [x], s), idx, x) for idx, x in enumerate(xs)]
        if not decorated:
            return []
        k0 = decorated[0][0]
        if isinstance(k0, Q):
            d = k0.d
            for k, _, _ in decorated:
                if not isinstance(k, Q) or k.d != d:
                    raise ThrumError("cannot sort mixed key types or dimensions", s)
            decorated.sort(key=lambda t: (t[0].v, t[1]))
        elif isinstance(k0, str):
            for k, _, _ in decorated:
                if not isinstance(k, str):
                    raise ThrumError("cannot sort mixed key types", s)
            decorated.sort(key=lambda t: (t[0], t[1]))
        else:
            raise ThrumError("cannot sort by %s" % tname(k0), s)
        return [t[2] for t in decorated]
    if all(isinstance(x, Q) for x in xs):
        d = xs[0].d
        if any(x.d != d for x in xs):
            raise ThrumError("cannot sort mixed dimensions", s)
        return sorted(xs, key=lambda x: x.v)
    if all(isinstance(x, str) for x in xs):
        return sorted(xs)
    raise ThrumError("cannot sort mixed types", s)


def _uniq(I, a, s):
    if not isinstance(a[0], list):
        raise ThrumError("uniq needs a list", s)
    out = []
    for x in a[0]:
        if not any(veq(x, y) for y in out):
            out.append(x)
    return out


def _reduce(I, a, s):
    if not isinstance(a[0], list):
        raise ThrumError("reduce needs a list", s)
    acc = a[2]
    for x in a[0]:
        acc = I.call(a[1], [acc, x], s)
    return acc


def _fold(xs, f, init):
    acc = None
    for x in xs:
        acc = x if acc is None else f(acc, x)
    return init if acc is None else acc


def _extreme(I, xs, s, want_min):
    if not xs:
        raise ThrumError("empty list", s)
    best = xs[0]
    if isinstance(best, Q):
        for x in xs[1:]:
            if qcmp(I.needq(x, s), best, "<" if want_min else ">", s):
                best = x
        return best
    if isinstance(best, str):
        for x in xs[1:]:
            if not isinstance(x, str):
                raise ThrumError("cannot compare %s with text" % tname(x), s)
            if (x < best) if want_min else (x > best):
                best = x
        return best
    raise ThrumError("min/max need numbers or text, got %s" % tname(best), s)


def _median(I, a, s):
    xs = _sort(I, a, s)
    if not xs:
        raise ThrumError("median of empty list", s)
    n = len(xs)
    return xs[n // 2] if n % 2 else qdiv(qadd(I.needq(xs[n // 2 - 1], s), I.needq(xs[n // 2], s), 1, s), mkq(2.0), s)


def _stdev(I, a, s):
    xs = a[0]
    if not isinstance(xs, list) or len(xs) < 2:
        raise ThrumError("stdev needs list of >= 2 values", s)
    m = I.call(I.builtins.vars["mean"], [xs], s)
    var = _fold([qpow(qadd(I.needq(x, s), m, -1, s), mkq(2.0)) for x in xs], lambda p, q: qadd(p, q, 1, s), mkq(0.0))
    return qpow(qdiv(var, mkq(float(len(xs) - 1)), s), mkq(0.5))


def _round(I, a, s):
    x = I.needq(a[0], s)
    nd = I.needint(a[1], s) if len(a) > 1 else 0
    sc = x.disp[0] if x.disp else 1.0
    if sc == 0:
        sc = 1.0
    return mkq(round(x.v / sc, nd) * sc, x.d, x.u, x.disp)


def _qty(I, a, s):
    if not isinstance(a[1], str):
        raise ThrumError("qty unit must be text", s)
    sc, d = parse_unit(a[1])
    u = I.needq(a[2], s).v if len(a) > 2 else 0.0
    return mkq(I.needq(a[0], s).v * sc, d, u * sc, (sc, a[1]))


def _fixfmt(I, a, s):
    x = I.needq(a[0], s)
    nd = I.needint(a[1], s)
    if nd < 0:
        raise ThrumError("fix precision must be >= 0", s)
    sc, sym = x.disp if x.disp else (1.0, dimstr(x.d))
    if sc == 0:
        sc = 1.0
    v = x.v / sc
    if not math.isfinite(v):
        tail = (" " + sym) if sym else ""
        return ("inf" if v > 0 else "-inf" if v < 0 else "nan") + tail
    return "%.*f%s" % (nd, v, (" " + sym) if sym else "")
