"""Standard library built-in modules for Thrum."""

import csv as _py_csv
import datetime as _dt
import io as _py_io
import json as _py_json
import math
import os
import re as _py_re

from .builtins import _extreme, _fold, _sort
from .caps import dec, enc
from .errors import ThrumError
from .graph import Scope
from .lexer import lex
from .units import D0, UNITS, _d, dimlabel, dimless, dimstr, parse_unit, scan_unit
from .values import Builtin, Module, Q, RecordVal, convert, mkq, qadd, qapprox, qdiv, qmul, qpow, tname, truthy, veq, vstr


def _scope_of(d, I):
    s = Scope(I.builtins)
    s.vars.update(d)
    s.const_names.update(d.keys())
    return s


def _needlist(v, s, what="list"):
    if not isinstance(v, list):
        raise ThrumError("%s needs a list, got %s" % (what, tname(v)), s)
    return v


def _needmap(v, s, what="map"):
    if not isinstance(v, dict):
        raise ThrumError("%s needs a map, got %s" % (what, tname(v)), s)
    return v


def _needstr(v, s, what="text"):
    if not isinstance(v, str):
        raise ThrumError("%s needs text, got %s" % (what, tname(v)), s)
    return v


# ------------------------------ math ------------------------------
def mod_math(I):
    d = {}
    for k, v in [
        ("pi", math.pi),
        ("tau", math.tau),
        ("e", math.e),
        ("inf", math.inf),
        ("nan", math.nan),
        ("phi", (1.0 + math.sqrt(5.0)) / 2.0),
    ]:
        d[k] = mkq(v)

    def lift(f):
        def g(I, a, s):
            x = I.needq(a[0], s)
            if not dimless(x.d):
                raise ThrumError("math functions need dimensionless arguments", s)
            try:
                val = f(x.v)
            except ValueError as ex:
                raise ThrumError("math domain error: %s" % ex, s)
            h = 1e-7 * max(1.0, abs(x.v))
            try:
                dv = (f(x.v + h) - f(x.v - h)) / (2 * h)
            except ValueError:
                dv = 0.0
            return mkq(val, D0, abs(dv) * x.u)

        return g

    for nm, f in [
        ("sin", math.sin), ("cos", math.cos), ("tan", math.tan),
        ("asin", math.asin), ("acos", math.acos), ("atan", math.atan),
        ("exp", math.exp), ("expm1", math.expm1),
        ("ln", math.log), ("log1p", math.log1p), ("log10", math.log10), ("log2", math.log2),
        ("sinh", math.sinh), ("cosh", math.cosh), ("tanh", math.tanh),
        ("asinh", math.asinh), ("acosh", math.acosh), ("atanh", math.atanh),
    ]:
        d[nm] = Builtin("math." + nm, lift(f), (1,))

    def _deg2rad(I, a, s):
        x = I.needq(a[0], s)
        if not dimless(x.d):
            raise ThrumError("deg2rad needs dimensionless angle", s)
        if x.disp and x.disp[1] in ("deg", "°", "degree", "degrees"):
            return mkq(x.v, D0, x.u, (1.0, "rad"))
        rad_v = x.v * math.pi / 180.0
        rad_u = x.u * math.pi / 180.0
        return mkq(rad_v, D0, rad_u, (1.0, "rad"))

    def _rad2deg(I, a, s):
        x = I.needq(a[0], s)
        if not dimless(x.d):
            raise ThrumError("rad2deg needs dimensionless angle", s)
        return Q(x.v, D0, x.u, (math.pi / 180.0, "deg"))

    def _atan2(I, a, s):
        y, x = I.needq(a[0], s), I.needq(a[1], s)
        if y.d != x.d:
            raise ThrumError("atan2 arguments must have identical dimensions", s)
        r2 = x.v * x.v + y.v * y.v
        u = math.hypot(x.v * y.u, y.v * x.u) / r2 if r2 else 0.0
        return mkq(math.atan2(y.v, x.v), D0, u)

    def _log(I, a, s):
        x, b = I.needq(a[0], s), I.needq(a[1], s)
        if not dimless(x.d) or not dimless(b.d):
            raise ThrumError("log needs dimensionless arguments", s)
        u = abs(x.u / (x.v * math.log(b.v))) if x.v and b.v > 0 and b.v != 1 else 0.0
        return mkq(math.log(x.v, b.v), D0, u)

    def _lcm(I, a, s):
        x, y = I.needint(a[0], s), I.needint(a[1], s)
        return mkq(float(abs(x * y) // math.gcd(x, y)) if x and y else 0.0)

    def _fact(I, a, s):
        n = I.needint(a[0], s)
        if n < 0:
            raise ThrumError("factorial needs n >= 0", s)
        return mkq(float(math.factorial(n)))

    def _comb(I, a, s):
        n, k = I.needint(a[0], s), I.needint(a[1], s)
        if n < 0 or k < 0:
            raise ThrumError("comb needs non-negative integers", s)
        return mkq(float(math.comb(n, k)))

    def _perm(I, a, s):
        n, k = I.needint(a[0], s), I.needint(a[1], s)
        if n < 0 or k < 0:
            raise ThrumError("perm needs non-negative integers", s)
        return mkq(float(math.perm(n, k)))

    def _is_prime(I, a, s):
        n = I.needint(a[0], s)
        if n < 2:
            return False
        if n % 2 == 0:
            return n == 2
        f = 3
        while f * f <= n:
            if n % f == 0:
                return False
            f += 2
        return True

    def _factors(I, a, s):
        n = I.needint(a[0], s)
        if n < 1:
            raise ThrumError("factors needs n >= 1", s)
        out = []
        d_ = 2
        while d_ * d_ <= n:
            while n % d_ == 0:
                out.append(mkq(float(d_)))
                n //= d_
            d_ += 1
        if n > 1:
            out.append(mkq(float(n)))
        return out

    def _linspace(I, a, s):
        start, stop = I.needq(a[0], s), I.needq(a[1], s)
        n = I.needint(a[2], s)
        if start.d != stop.d:
            raise ThrumError("linspace endpoints must have identical dimensions", s)
        if n < 0:
            raise ThrumError("linspace count must be >= 0", s)
        if n == 0:
            return []
        if n == 1:
            return [start]
        step = qdiv(qadd(stop, start, -1, s), mkq(float(n - 1)), s)
        return [qadd(start, qmul(step, mkq(float(i))), 1, s) for i in range(n)]

    def _arange(I, a, s):
        start, stop = I.needq(a[0], s), I.needq(a[1], s)
        step = I.needq(a[2], s) if len(a) > 2 else mkq(1.0, start.d, 0.0, start.disp)
        if start.d != stop.d or start.d != step.d:
            raise ThrumError("arange arguments must have identical dimensions", s)
        if step.v == 0:
            raise ThrumError("arange step cannot be 0", s)
        out = []
        cur = start
        while (cur.v < stop.v - 1e-12 * max(1.0, abs(stop.v))) if step.v > 0 else (cur.v > stop.v + 1e-12 * max(1.0, abs(stop.v))):
            out.append(cur)
            cur = qadd(cur, step, 1, s)
            if len(out) > 100000:
                raise ThrumError("arange too large", s)
        return out

    def _dot(I, a, s):
        xs, ys = _needlist(a[0], s, "dot"), _needlist(a[1], s, "dot")
        if len(xs) != len(ys):
            raise ThrumError("dot needs equal-length lists", s)
        if not xs:
            return mkq(0.0)
        return _fold([qmul(I.needq(x, s), I.needq(y, s)) for x, y in zip(xs, ys)], lambda p, q: qadd(p, q, 1, s), mkq(0.0))

    def _norm(I, a, s):
        xs = _needlist(a[0], s, "norm")
        if not xs:
            return mkq(0.0)
        sq = _fold([qpow(I.needq(x, s), mkq(2.0)) for x in xs], lambda p, q: qadd(p, q, 1, s), mkq(0.0))
        return qpow(sq, mkq(0.5), s)

    def _cumsum(I, a, s):
        xs = _needlist(a[0], s, "cumsum")
        if not xs:
            return []
        acc = I.needq(xs[0], s)
        out = [acc]
        for x in xs[1:]:
            acc = qadd(acc, I.needq(x, s), 1, s)
            out.append(acc)
        return out

    def _diff(I, a, s):
        xs = _needlist(a[0], s, "diff")
        if len(xs) < 2:
            return []
        return [qadd(I.needq(xs[i + 1], s), I.needq(xs[i], s), -1, s) for i in range(len(xs) - 1)]

    d["deg2rad"] = Builtin("math.deg2rad", _deg2rad, (1,))
    d["rad2deg"] = Builtin("math.rad2deg", _rad2deg, (1,))
    d["isfinite"] = Builtin("math.isfinite", lambda I, a, s: math.isfinite(I.needq(a[0], s).v), (1,))
    d["isnan"] = Builtin("math.isnan", lambda I, a, s: math.isnan(I.needq(a[0], s).v), (1,))
    d["isinf"] = Builtin("math.isinf", lambda I, a, s: math.isinf(I.needq(a[0], s).v), (1,))
    d["atan2"] = Builtin("math.atan2", _atan2, (2,))
    d["log"] = Builtin("math.log", _log, (2,))
    d["hypot"] = Builtin("math.hypot", lambda I, a, s: qpow(qadd(qpow(I.needq(a[0], s), mkq(2.0)), qpow(I.needq(a[1], s), mkq(2.0)), 1, s), mkq(0.5)), (2,))
    d["gcd"] = Builtin("math.gcd", lambda I, a, s: mkq(float(math.gcd(I.needint(a[0], s), I.needint(a[1], s)))), (2,))
    d["lcm"] = Builtin("math.lcm", _lcm, (2,))
    d["factorial"] = Builtin("math.factorial", _fact, (1,))
    d["comb"] = Builtin("math.comb", _comb, (2,))
    d["perm"] = Builtin("math.perm", _perm, (2,))
    d["is_prime"] = Builtin("math.is_prime", _is_prime, (1,))
    d["factors"] = Builtin("math.factors", _factors, (1,))
    d["linspace"] = Builtin("math.linspace", _linspace, (3,))
    d["arange"] = Builtin("math.arange", _arange, (2, 3))
    def _shl(I, a, s):
        x, k = I.needint(a[0], s), I.needint(a[1], s)
        if k < 0 or k >= 63:
            raise ThrumError("shift count out of range 0..62", s)
        res = (x << k) & ((1 << 64) - 1)
        if res >= (1 << 63):
            res -= (1 << 64)
        return mkq(float(res))

    def _shr(I, a, s):
        x, k = I.needint(a[0], s), I.needint(a[1], s)
        if k < 0 or k >= 63:
            raise ThrumError("shift count out of range 0..62", s)
        return mkq(float(x >> k))

    d["dot"] = Builtin("math.dot", _dot, (2,))
    d["norm"] = Builtin("math.norm", _norm, (1,))
    d["cumsum"] = Builtin("math.cumsum", _cumsum, (1,))
    d["diff"] = Builtin("math.diff", _diff, (1,))
    d["band"] = Builtin("math.band", lambda I, a, s: mkq(float(I.needint(a[0], s) & I.needint(a[1], s))), (2,))
    d["bor"] = Builtin("math.bor", lambda I, a, s: mkq(float(I.needint(a[0], s) | I.needint(a[1], s))), (2,))
    d["bxor"] = Builtin("math.bxor", lambda I, a, s: mkq(float(I.needint(a[0], s) ^ I.needint(a[1], s))), (2,))
    d["bnot"] = Builtin("math.bnot", lambda I, a, s: mkq(float(~I.needint(a[0], s))), (1,))
    d["shl"] = Builtin("math.shl", _shl, (2,))
    d["shr"] = Builtin("math.shr", _shr, (2,))
    d["clamp"] = I.builtins.vars["clamp"]
    return Module("math", _scope_of(d, I))


# ------------------------------ const ------------------------------
def mod_const(I):
    d = {
        "c": mkq(299792458.0, _d(m=1, s=-1), 0.0, (1.0, "m/s")),
        "h": mkq(6.62607015e-34, _d(kg=1, m=2, s=-1), 0.0, (1.0, "J*s")),
        "hbar": mkq(1.054571817e-34, _d(kg=1, m=2, s=-1), 0.0, (1.0, "J*s")),
        "kB": mkq(1.380649e-23, _d(kg=1, m=2, s=-2, K=-1), 0.0, (1.0, "J/K")),
        "NA": mkq(6.02214076e23, _d(mol=-1), 0.0, (1.0, "1/mol")),
        "e": mkq(1.602176634e-19, _d(A=1, s=1), 0.0, (1.0, "C")),
        "G": mkq(6.67430e-11, _d(m=3, kg=-1, s=-2), 1.5e-15, (1.0, "m^3/(kg*s^2)")),
        "eps0": mkq(8.8541878128e-12, _d(kg=-1, m=-3, s=4, A=2), 1.3e-21, (1.0, "F/m")),
        "mu0": mkq(1.25663706212e-6, _d(kg=1, m=1, s=-2, A=-2), 1.9e-16, (1.0, "N/A^2")),
        "me": mkq(9.1093837015e-31, _d(kg=1), 2.8e-40, (1.0, "kg")),
        "mp": mkq(1.67262192369e-27, _d(kg=1), 5.1e-37, (1.0, "kg")),
        "mn": mkq(1.67492749804e-27, _d(kg=1), 9.5e-37, (1.0, "kg")),
        "u": mkq(1.66053906660e-27, _d(kg=1), 5.0e-37, (1.0, "kg")),
        "alpha": mkq(7.2973525693e-3, D0, 1.1e-12),
        "Ry": mkq(10973731.568160, _d(m=-1), 2.1e-5, (1.0, "1/m")),
        "F": mkq(96485.3321233100184, _d(A=1, s=1, mol=-1), 0.0, (1.0, "C/mol")),
        "muB": mkq(9.2740100783e-24, _d(m=2, A=1), 2.8e-33, (1.0, "J/T")),
        "R": mkq(8.314462618, _d(kg=1, m=2, s=-2, K=-1, mol=-1), 0.0, (1.0, "J/(mol*K)")),
        "g0": mkq(9.80665, _d(m=1, s=-2), 0.0, (1.0, "m/s^2")),
        "atm": mkq(101325.0, _d(kg=1, m=-1, s=-2), 0.0, (1.0, "Pa")),
        "sigma": mkq(5.670374419e-8, _d(kg=1, s=-3, K=-4), 0.0, (1.0, "W/(m^2*K^4)")),
        "au": mkq(1.495978707e11, _d(m=1), 0.0, (1.0, "m")),
        "ly": mkq(9.4607304725808e15, _d(m=1), 0.0, (1.0, "m")),
        "pc": mkq(3.085677581491367e16, _d(m=1), 0.0, (1.0, "m")),
        "M_sun": mkq(1.98847e30, _d(kg=1), 7e25, (1.0, "kg")),
        "M_earth": mkq(5.97217e24, _d(kg=1), 6e19, (1.0, "kg")),
        "R_earth": mkq(6.371e6, _d(m=1), 0.0, (1.0, "m")),
        "L_sun": mkq(3.828e26, _d(kg=1, m=2, s=-3), 0.0, (1.0, "W")),
        "T_cmb": mkq(2.72548, _d(K=1), 0.00057, (1.0, "K")),
    }
    return Module("const", _scope_of(d, I))


# ------------------------------ stat ------------------------------
def mod_stat(I):
    def linfit(I, a, s):
        xs, ys = _needlist(a[0], s, "linfit"), _needlist(a[1], s, "linfit")
        n = len(xs)
        if n < 2 or n != len(ys):
            raise ThrumError("linfit needs equal lists of length >= 2", s)
        mx = I.call(I.builtins.vars["mean"], [xs], s)
        my = I.call(I.builtins.vars["mean"], [ys], s)
        sxy = _fold([qmul(qadd(I.needq(x, s), mx, -1, s), qadd(I.needq(y, s), my, -1, s)) for x, y in zip(xs, ys)], lambda p, q: qadd(p, q, 1, s), mkq(0.0))
        sxx = _fold([qmul(qadd(I.needq(x, s), mx, -1, s), qadd(I.needq(x, s), mx, -1, s)) for x in xs], lambda p, q: qadd(p, q, 1, s), mkq(0.0))
        slope = qdiv(sxy, sxx, s)
        inter = qadd(my, qmul(slope, mx), -1, s)
        ssr = _fold([qpow(qadd(I.needq(y, s), qadd(inter, qmul(slope, I.needq(x, s)), 1, s), -1, s), mkq(2.0)) for x, y in zip(xs, ys)], lambda p, q: qadd(p, q, 1, s), mkq(0.0))
        sst = _fold([qpow(qadd(I.needq(y, s), my, -1, s), mkq(2.0)) for y in ys], lambda p, q: qadd(p, q, 1, s), mkq(0.0))
        r2 = 1.0 - (ssr.v / sst.v) if sst.v else 1.0
        return {"slope": slope, "intercept": inter, "r2": mkq(r2), "n": mkq(float(n))}

    def variance(I, a, s):
        xs = _needlist(a[0], s, "variance")
        if len(xs) < 2:
            raise ThrumError("variance needs >= 2 values", s)
        m = I.call(I.builtins.vars["mean"], [xs], s)
        ss = _fold([qpow(qadd(I.needq(x, s), m, -1, s), mkq(2.0)) for x in xs], lambda p, q: qadd(p, q, 1, s), mkq(0.0))
        return qdiv(ss, mkq(float(len(xs) - 1)), s)

    def quantile(I, a, s):
        xs = _sort(I, [a[0]], s)
        if not xs:
            raise ThrumError("quantile of empty list", s)
        q = I.needq(a[1], s)
        if not dimless(q.d) or not 0.0 <= q.v <= 1.0:
            raise ThrumError("quantile probability must be in [0, 1]", s)
        if len(xs) == 1:
            return xs[0]
        pos = q.v * (len(xs) - 1)
        lo = int(math.floor(pos))
        hi = int(math.ceil(pos))
        if lo == hi:
            return xs[lo]
        frac = mkq(pos - lo)
        return qadd(xs[lo], qmul(qadd(xs[hi], xs[lo], -1, s), frac), 1, s)

    def iqr(I, a, s):
        return qadd(quantile(I, [a[0], mkq(0.75)], s), quantile(I, [a[0], mkq(0.25)], s), -1, s)

    def cov(I, a, s):
        xs, ys = _needlist(a[0], s, "cov"), _needlist(a[1], s, "cov")
        if len(xs) < 2 or len(xs) != len(ys):
            raise ThrumError("cov needs equal lists of length >= 2", s)
        mx = I.call(I.builtins.vars["mean"], [xs], s)
        my = I.call(I.builtins.vars["mean"], [ys], s)
        sxy = _fold([qmul(qadd(I.needq(x, s), mx, -1, s), qadd(I.needq(y, s), my, -1, s)) for x, y in zip(xs, ys)], lambda p, q: qadd(p, q, 1, s), mkq(0.0))
        return qdiv(sxy, mkq(float(len(xs) - 1)), s)

    def corr(I, a, s):
        sx = I.call(I.builtins.vars["stdev"], [a[0]], s)
        sy = I.call(I.builtins.vars["stdev"], [a[1]], s)
        c = cov(I, a, s)
        return qdiv(c, qmul(sx, sy), s)

    def weighted_mean(I, a, s):
        xs = _needlist(a[0], s, "weighted_mean")
        if not xs:
            raise ThrumError("weighted_mean of empty list", s)
        qs = [I.needq(x, s) for x in xs]
        d0 = qs[0].d
        for q in qs[1:]:
            if q.d != d0:
                raise ThrumError("dimension mismatch in weighted_mean: %s vs %s" % (dimlabel(qs[0]), dimlabel(q)), s)
        if all(q.u > 0 for q in qs):
            ws = [1.0 / (q.u * q.u) for q in qs]
            wsum = sum(ws)
            v = sum(w * q.v for w, q in zip(ws, qs)) / wsum
            u = math.sqrt(1.0 / wsum)
            return mkq(v, d0, u, qs[0].disp)
        m = I.call(I.builtins.vars["mean"], [qs], s)
        if len(qs) >= 2:
            sd = I.call(I.builtins.vars["stdev"], [qs], s)
            se = sd.v / math.sqrt(len(qs))
            return mkq(m.v, d0, max(m.u, se), qs[0].disp)
        return m

    def zscore(I, a, s):
        xs = _needlist(a[0], s, "zscore")
        m = I.call(I.builtins.vars["mean"], [xs], s)
        sd = I.call(I.builtins.vars["stdev"], [xs], s)
        return [qdiv(qadd(I.needq(x, s), m, -1, s), sd, s) for x in xs]

    def normalize(I, a, s):
        xs = _needlist(a[0], s, "normalize")
        lo = _extreme(I, xs, s, True)
        hi = _extreme(I, xs, s, False)
        span = qadd(hi, lo, -1, s)
        if span.v == 0:
            return [mkq(0.0) for _ in xs]
        return [qdiv(qadd(I.needq(x, s), lo, -1, s), span, s) for x in xs]

    def histogram(I, a, s):
        xs = _needlist(a[0], s, "histogram")
        bins = I.needint(a[1], s) if len(a) > 1 else 10
        if not xs or bins < 1:
            raise ThrumError("histogram needs non-empty list and bins >= 1", s)
        qs = [I.needq(x, s) for x in xs]
        lo = _extreme(I, qs, s, True)
        hi = _extreme(I, qs, s, False)
        width = qdiv(qadd(hi, lo, -1, s), mkq(float(bins)), s)
        counts = [0] * bins
        if width.v == 0:
            counts[0] = len(qs)
        else:
            for q in qs:
                idx = int((q.v - lo.v) / width.v)
                if idx >= bins:
                    idx = bins - 1
                if idx < 0:
                    idx = 0
                counts[idx] += 1
        out = []
        for i in range(bins):
            b0 = qadd(lo, qmul(width, mkq(float(i))), 1, s)
            b1 = qadd(lo, qmul(width, mkq(float(i + 1))), 1, s)
            out.append({"low": b0, "high": b1, "count": mkq(float(counts[i]))})
        return out

    def mode(I, a, s):
        xs = _needlist(a[0], s, "mode")
        if not xs:
            raise ThrumError("mode of empty list", s)
        counts = []
        for x in xs:
            found = False
            for pair in counts:
                if veq(pair[0], x):
                    pair[1] += 1
                    found = True
                    break
            if not found:
                counts.append([x, 1])
        return max(counts, key=lambda p: p[1])[0]

    d = {
        "linfit": Builtin("stat.linfit", linfit, (2,)),
        "mean": I.builtins.vars["mean"],
        "stdev": I.builtins.vars["stdev"],
        "median": I.builtins.vars["median"],
        "variance": Builtin("stat.variance", variance, (1,)),
        "quantile": Builtin("stat.quantile", quantile, (2,)),
        "iqr": Builtin("stat.iqr", iqr, (1,)),
        "cov": Builtin("stat.cov", cov, (2,)),
        "corr": Builtin("stat.corr", corr, (2,)),
        "weighted_mean": Builtin("stat.weighted_mean", weighted_mean, (1,)),
        "zscore": Builtin("stat.zscore", zscore, (1,)),
        "normalize": Builtin("stat.normalize", normalize, (1,)),
        "histogram": Builtin("stat.histogram", histogram, (1, 2)),
        "mode": Builtin("stat.mode", mode, (1,)),
    }
    return Module("stat", _scope_of(d, I))


# ------------------------------ units ------------------------------
def mod_units(I):
    def define(I, a, s):
        if not isinstance(a[0], str) or not isinstance(a[2], str):
            raise ThrumError("units.define expects (name: text, value: num, base_unit: text)", s)
        sc, d = parse_unit(a[2])
        val = I.needq(a[1], s)
        if not dimless(val.d):
            raise ThrumError("units.define scale factor must be dimensionless", s)
        UNITS[a[0]] = (val.v * sc, d)
        return None

    def compatible(I, a, s):
        x, y = I.needq(a[0], s), I.needq(a[1], s)
        return x.d == y.d

    d = {
        "convert": I.builtins.vars["convert"],
        "qty": I.builtins.vars["qty"],
        "define": Builtin("units.define", define, (3,)),
        "list": Builtin("units.list", lambda I, a, s: sorted(UNITS), (0,)),
        "dim_of": Builtin("units.dim_of", lambda I, a, s: dimstr(I.needq(a[0], s).d) or "1", (1,)),
        "compatible": Builtin("units.compatible", compatible, (2,)),
        "c2k": I.builtins.vars["c2k"],
        "k2c": I.builtins.vars["k2c"],
        "f2k": Builtin("units.f2k", lambda I, a, s: mkq((I.needq(a[0], s).v - 32.0) * 5.0 / 9.0 + 273.15, _d(K=1), I.needq(a[0], s).u * 5.0 / 9.0, (1.0, "K")), (1,)),
        "k2f": Builtin("units.k2f", lambda I, a, s: mkq((I.needq(a[0], s).v - 273.15) * 9.0 / 5.0 + 32.0, _d(K=1), I.needq(a[0], s).u * 9.0 / 5.0, (1.0, "K")), (1,)),
    }
    return Module("units", _scope_of(d, I))


# ------------------------------ text ------------------------------
def mod_text(I):
    B = I.builtins.vars

    def _pad(left):
        def f(I, a, s):
            txt = _needstr(a[0], s, "pad")
            w = I.needint(a[1], s)
            ch = _needstr(a[2], s, "pad") if len(a) > 2 else " "
            if len(ch) != 1:
                raise ThrumError("pad fill must be a single character", s)
            return txt.rjust(w, ch) if left else txt.ljust(w, ch)

        return f

    def _fmt(I, a, s):
        tmpl = _needstr(a[0], s, "text.format")
        args = [vstr(x) for x in a[1:]]
        try:
            return tmpl.format(*args)
        except (IndexError, KeyError, ValueError) as ex:
            raise ThrumError("text.format error: %s" % ex, s)

    def _ord(I, a, s):
        t = _needstr(a[0], s, "text.ord")
        if len(t) != 1:
            raise ThrumError("text.ord expects a single character", s)
        return mkq(float(ord(t)))

    def _chr(I, a, s):
        cp = I.needint(a[0], s)
        if cp < 0 or cp > 0x10FFFF or 0xD800 <= cp <= 0xDFFF:
            raise ThrumError("text.chr codepoint out of valid UTF-8 range", s)
        return chr(cp)

    def _from_hex(I, a, s):
        try:
            return mkq(float(int(_needstr(a[0], s, "text.from_hex").strip(), 16)))
        except ValueError:
            raise ThrumError("invalid hex integer", s)

    d = {
        "split": B["split"],
        "join": B["join"],
        "upper": B["upper"],
        "lower": B["lower"],
        "title": Builtin("text.title", lambda I, a, s: _needstr(a[0], s, "text.title").title(), (1,)),
        "trim": B["trim"],
        "ltrim": Builtin("text.ltrim", lambda I, a, s: _needstr(a[0], s, "text.ltrim").lstrip(), (1,)),
        "rtrim": Builtin("text.rtrim", lambda I, a, s: _needstr(a[0], s, "text.rtrim").rstrip(), (1,)),
        "replace": B["replace"],
        "starts": B["starts"],
        "ends": B["ends"],
        "contains": Builtin("text.contains", lambda I, a, s: _needstr(a[1], s, "text.contains") in _needstr(a[0], s, "text.contains"), (2,)),
        "index_of": Builtin("text.index_of", lambda I, a, s: mkq(float(_needstr(a[0], s, "text.index_of").find(_needstr(a[1], s, "text.index_of")))), (2,)),
        "count": Builtin("text.count", lambda I, a, s: mkq(float(_needstr(a[0], s, "text.count").count(_needstr(a[1], s, "text.count")))), (2,)),
        "pad_left": Builtin("text.pad_left", _pad(True), (2, 3)),
        "pad_right": Builtin("text.pad_right", _pad(False), (2, 3)),
        "repeat": Builtin("text.repeat", lambda I, a, s: _needstr(a[0], s, "text.repeat") * I.needint(a[1], s), (2,)),
        "chars": Builtin("text.chars", lambda I, a, s: list(_needstr(a[0], s, "text.chars")), (1,)),
        "lines": Builtin("text.lines", lambda I, a, s: _needstr(a[0], s, "text.lines").splitlines(), (1,)),
        "words": Builtin("text.words", lambda I, a, s: _needstr(a[0], s, "text.words").split(), (1,)),
        "is_digit": Builtin("text.is_digit", lambda I, a, s: _needstr(a[0], s, "text.is_digit").isdigit(), (1,)),
        "is_alpha": Builtin("text.is_alpha", lambda I, a, s: _needstr(a[0], s, "text.is_alpha").isalpha(), (1,)),
        "format": Builtin("text.format", _fmt, None),
        "ord": Builtin("text.ord", _ord, (1,)),
        "chr": Builtin("text.chr", _chr, (1,)),
        "to_hex": Builtin("text.to_hex", lambda I, a, s: format(I.needint(a[0], s), "x"), (1,)),
        "to_bin": Builtin("text.to_bin", lambda I, a, s: format(I.needint(a[0], s), "b"), (1,)),
        "from_hex": Builtin("text.from_hex", _from_hex, (1,)),
    }
    return Module("text", _scope_of(d, I))


# ------------------------------ regex ------------------------------
def mod_regex(I):
    def _compile(pat, s):
        try:
            return _py_re.compile(_needstr(pat, s, "regex"))
        except _py_re.error as ex:
            raise ThrumError("invalid regex %r: %s" % (pat, ex), s)

    def _m_obj(m):
        if m is None:
            return None
        return {
            "text": m.group(0),
            "start": mkq(float(m.start())),
            "end": mkq(float(m.end())),
            "groups": [g if g is not None else "" for g in m.groups()],
        }

    d = {
        "test": Builtin("regex.test", lambda I, a, s: _compile(a[0], s).search(_needstr(a[1], s, "regex")) is not None, (2,)),
        "match": Builtin("regex.match", lambda I, a, s: _m_obj(_compile(a[0], s).match(_needstr(a[1], s, "regex"))), (2,)),
        "search": Builtin("regex.search", lambda I, a, s: _m_obj(_compile(a[0], s).search(_needstr(a[1], s, "regex"))), (2,)),
        "find_all": Builtin("regex.find_all", lambda I, a, s: [m if isinstance(m, str) else m[0] for m in _compile(a[0], s).findall(_needstr(a[1], s, "regex"))], (2,)),
        "replace": Builtin("regex.replace", lambda I, a, s: _compile(a[0], s).sub(_needstr(a[1], s, "regex"), _needstr(a[2], s, "regex")), (3,)),
        "split": Builtin("regex.split", lambda I, a, s: _compile(a[0], s).split(_needstr(a[1], s, "regex")), (2,)),
    }
    return Module("regex", _scope_of(d, I))


# ------------------------------ json ------------------------------
def _to_thrum_json(v):
    if v is None or isinstance(v, (bool, str)):
        return v
    if isinstance(v, (int, float)):
        return mkq(float(v))
    if isinstance(v, list):
        return [_to_thrum_json(x) for x in v]
    if isinstance(v, dict):
        return {str(k): _to_thrum_json(val) for k, val in v.items()}
    return vstr(v)


def _from_thrum_json(v, site=None):
    if v is None or isinstance(v, (bool, str)):
        return v
    if isinstance(v, Q):
        if not math.isfinite(v.v):
            raise ThrumError("JSON cannot encode non-finite number", site)
        sc = v.disp[0] if v.disp else 1.0
        val = v.v / (sc or 1.0)
        if val == int(val) and abs(val) < 1e15:
            return int(val)
        return val
    if isinstance(v, list):
        return [_from_thrum_json(x, site) for x in v]
    if isinstance(v, dict):
        return {str(k): _from_thrum_json(val, site) for k, val in v.items()}
    if isinstance(v, RecordVal):
        return {k: _from_thrum_json(v.fields[k], site) for k in v.rtype.field_names}
    raise ThrumError("cannot encode %s as JSON" % tname(v), site)


def mod_json(I):
    def parse_j(I, a, s):
        try:
            raw = _py_json.loads(_needstr(a[0], s, "json.parse"))
        except ValueError as ex:
            raise ThrumError("json.parse error: %s" % ex, s)
        return _to_thrum_json(raw)

    def stringify_j(I, a, s):
        ind = I.needint(a[1], s) if len(a) > 1 else None
        return _py_json.dumps(_from_thrum_json(a[0], s), indent=ind, ensure_ascii=False)

    def parse_qty(I, a, s):
        try:
            raw = _py_json.loads(_needstr(a[0], s, "json.parse_qty"))
        except ValueError as ex:
            raise ThrumError("json.parse_qty error: %s" % ex, s)

        def convert_dec(x):
            if isinstance(x, dict) and "$q" in x:
                return dec(x)
            if isinstance(x, list):
                return [convert_dec(i) for i in x]
            if isinstance(x, dict):
                return {k: convert_dec(val) for k, val in x.items()}
            if isinstance(x, (int, float)) and not isinstance(x, bool):
                return mkq(float(x))
            return x

        return convert_dec(raw)

    def stringify_qty(I, a, s):
        ind = I.needint(a[1], s) if len(a) > 1 else None
        return _py_json.dumps(enc(a[0]), indent=ind, ensure_ascii=False)

    d = {
        "parse": Builtin("json.parse", parse_j, (1,)),
        "stringify": Builtin("json.stringify", stringify_j, (1, 2)),
        "parse_qty": Builtin("json.parse_qty", parse_qty, (1,)),
        "stringify_qty": Builtin("json.stringify_qty", stringify_qty, (1, 2)),
    }
    return Module("json", _scope_of(d, I))


# ------------------------------ csv ------------------------------
def _try_parse_qty_cell(cell):
    txt = cell.strip()
    if not txt:
        return cell
    try:
        toks = lex(txt)
    except ThrumError:
        return cell
    # Only accept a single literal quantity token (optionally signed with '-')
    if len(toks) == 3 and toks[0].kind == "QTY" and toks[1].kind == "NEWLINE" and toks[2].kind == "EOF":
        v, u, disp = toks[0].val
        d = parse_unit(disp[1])[1] if disp else D0
        return mkq(v, d, u, disp)
    if (
        len(toks) == 4
        and toks[0].kind == "OP"
        and toks[0].val == "-"
        and toks[1].kind == "QTY"
        and toks[2].kind == "NEWLINE"
        and toks[3].kind == "EOF"
    ):
        v, u, disp = toks[1].val
        d = parse_unit(disp[1])[1] if disp else D0
        return mkq(-v, d, u, disp)
    return cell


def mod_csv(I):
    def _parse(parse_numbers):
        def f(I, a, s):
            raw = _needstr(a[0], s, "csv.parse")
            header = truthy(a[1]) if len(a) > 1 else True
            rows = list(_py_csv.reader(_py_io.StringIO(raw)))
            if not rows:
                return []
            if parse_numbers:
                def conv_row(r):
                    return [_try_parse_qty_cell(c) for c in r]
            else:
                def conv_row(r):
                    return list(r)
            if not header:
                return [conv_row(r) for r in rows]
            cols = [c.strip() for c in rows[0]]
            out = []
            for r in rows[1:]:
                vals = conv_row(r)
                m = {}
                for idx, col in enumerate(cols):
                    m[col] = vals[idx] if idx < len(vals) else ""
                out.append(m)
            return out

        return f

    def stringify(I, a, s):
        rows = _needlist(a[0], s, "csv.stringify")
        headers = _needlist(a[1], s, "csv.stringify") if len(a) > 1 else None
        buf = _py_io.StringIO()
        w = _py_csv.writer(buf, lineterminator="\n")
        if not rows:
            if headers:
                w.writerow([vstr(h) for h in headers])
            return buf.getvalue()
        if isinstance(rows[0], dict):
            cols = [vstr(h) for h in headers] if headers else list(rows[0].keys())
            w.writerow(cols)
            for r in rows:
                w.writerow([vstr(r.get(c)) for c in cols])
        elif isinstance(rows[0], RecordVal):
            cols = [vstr(h) for h in headers] if headers else list(rows[0].rtype.field_names)
            w.writerow(cols)
            for r in rows:
                w.writerow([vstr(r.fields.get(c)) for c in cols])
        else:
            if headers:
                w.writerow([vstr(h) for h in headers])
            for r in rows:
                w.writerow([vstr(x) for x in (r if isinstance(r, list) else [r])])
        return buf.getvalue()

    d = {
        "parse": Builtin("csv.parse", _parse(False), (1, 2)),
        "parse_nums": Builtin("csv.parse_nums", _parse(True), (1, 2)),
        "stringify": Builtin("csv.stringify", stringify, (1, 2)),
    }
    return Module("csv", _scope_of(d, I))


# ------------------------------ time ------------------------------
def mod_time(I):
    def _need_sec(v, s):
        q = I.needq(v, s)
        if q.d != _d(s=1):
            raise ThrumError("time function expects duration/timestamp in seconds (s)", s)
        return q.v

    def parse_iso(I, a, s):
        txt = _needstr(a[0], s, "time.parse_iso")
        if txt.endswith("Z"):
            txt = txt[:-1] + "+00:00"
        try:
            dt = _dt.datetime.fromisoformat(txt)
        except ValueError as ex:
            raise ThrumError("invalid ISO timestamp '%s': %s" % (a[0], ex), s)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=_dt.timezone.utc)
        return mkq(dt.timestamp(), _d(s=1), 0.0, (1.0, "s"))

    def format_iso(I, a, s):
        ts = _need_sec(a[0], s)
        dt = _dt.datetime.fromtimestamp(ts, tz=_dt.timezone.utc)
        return dt.strftime("%Y-%m-%dT%H:%M:%SZ")

    def format_dur(I, a, s):
        sec = _need_sec(a[0], s)
        neg = sec < 0
        rem = abs(sec)
        days, rem = divmod(rem, 86400)
        hrs, rem = divmod(rem, 3600)
        mins, secs = divmod(rem, 60)
        parts = []
        if days >= 1:
            parts.append("%dd" % int(days))
        if hrs >= 1 or parts:
            parts.append("%dh" % int(hrs))
        if mins >= 1 or parts:
            parts.append("%dm" % int(mins))
        if secs == int(secs):
            parts.append("%ds" % int(secs))
        else:
            parts.append("%.3gs" % secs)
        return ("-" if neg else "") + " ".join(parts)

    def components(I, a, s):
        ts = _need_sec(a[0], s)
        dt = _dt.datetime.fromtimestamp(ts, tz=_dt.timezone.utc)
        return {
            "year": mkq(float(dt.year)),
            "month": mkq(float(dt.month)),
            "day": mkq(float(dt.day)),
            "hour": mkq(float(dt.hour)),
            "min": mkq(float(dt.minute)),
            "sec": mkq(float(dt.second) + dt.microsecond / 1e6),
            "weekday": mkq(float(dt.weekday())),
        }

    def from_components(I, a, s):
        vals = [I.needint(x, s) for x in a]
        y, mo, day = vals[0], vals[1], vals[2]
        hr = vals[3] if len(vals) > 3 else 0
        mi = vals[4] if len(vals) > 4 else 0
        sc = vals[5] if len(vals) > 5 else 0
        try:
            dt = _dt.datetime(y, mo, day, hr, mi, sc, tzinfo=_dt.timezone.utc)
        except ValueError as ex:
            raise ThrumError("invalid date/time: %s" % ex, s)
        return mkq(dt.timestamp(), _d(s=1), 0.0, (1.0, "s"))

    d = {
        "parse_iso": Builtin("time.parse_iso", parse_iso, (1,)),
        "format_iso": Builtin("time.format_iso", format_iso, (1,)),
        "format_dur": Builtin("time.format_dur", format_dur, (1,)),
        "components": Builtin("time.components", components, (1,)),
        "from_components": Builtin("time.from_components", from_components, (3, 4, 5, 6)),
    }
    return Module("time", _scope_of(d, I))


# ------------------------------ path ------------------------------
def mod_path(I):
    def _join(I, a, s):
        if not a:
            return ""
        return os.path.join(*[_needstr(x, s, "path.join") for x in a])

    def _stem(I, a, s):
        b = os.path.basename(_needstr(a[0], s, "path.stem"))
        return os.path.splitext(b)[0]

    def _ext(I, a, s):
        return os.path.splitext(_needstr(a[0], s, "path.ext"))[1]

    def _split(I, a, s):
        h, t = os.path.split(_needstr(a[0], s, "path.split"))
        return [h, t]

    d = {
        "join": Builtin("path.join", _join, None),
        "dirname": Builtin("path.dirname", lambda I, a, s: os.path.dirname(_needstr(a[0], s, "path.dirname")), (1,)),
        "basename": Builtin("path.basename", lambda I, a, s: os.path.basename(_needstr(a[0], s, "path.basename")), (1,)),
        "stem": Builtin("path.stem", _stem, (1,)),
        "ext": Builtin("path.ext", _ext, (1,)),
        "normalize": Builtin("path.normalize", lambda I, a, s: os.path.normpath(_needstr(a[0], s, "path.normalize")), (1,)),
        "split": Builtin("path.split", _split, (1,)),
    }
    return Module("path", _scope_of(d, I))


# ------------------------------ list ------------------------------
def mod_list(I):
    B = I.builtins.vars

    def chunk(I, a, s):
        xs = _needlist(a[0], s, "list.chunk")
        n = I.needint(a[1], s)
        if n <= 0:
            raise ThrumError("chunk size must be > 0", s)
        return [xs[i : i + n] for i in range(0, len(xs), n)]

    def window(I, a, s):
        xs = _needlist(a[0], s, "list.window")
        n = I.needint(a[1], s)
        if n <= 0:
            raise ThrumError("window size must be > 0", s)
        if n > len(xs):
            return []
        return [xs[i : i + n] for i in range(len(xs) - n + 1)]

    def take(I, a, s):
        xs = _needlist(a[0], s, "list.take")
        n = max(0, I.needint(a[1], s))
        return xs[:n]

    def drop(I, a, s):
        xs = _needlist(a[0], s, "list.drop")
        n = max(0, I.needint(a[1], s))
        return xs[n:]

    def partition(I, a, s):
        xs = _needlist(a[0], s, "list.partition")
        yes, no = [], []
        for x in xs:
            if truthy(I.call(a[1], [x], s)):
                yes.append(x)
            else:
                no.append(x)
        return [yes, no]

    def group_by(I, a, s):
        xs = _needlist(a[0], s, "list.group_by")
        out = {}
        for x in xs:
            k = I.call(a[1], [x], s)
            if isinstance(k, (list, dict)):
                raise ThrumError("group_by key cannot be %s" % tname(k), s)
            if k not in out:
                out[k] = []
            out[k].append(x)
        return out

    def flat_map(I, a, s):
        xs = _needlist(a[0], s, "list.flat_map")
        out = []
        for x in xs:
            r = I.call(a[1], [x], s)
            if isinstance(r, list):
                out.extend(r)
            else:
                out.append(r)
        return out

    d = {
        "chunk": Builtin("list.chunk", chunk, (2,)),
        "window": Builtin("list.window", window, (2,)),
        "take": Builtin("list.take", take, (2,)),
        "drop": Builtin("list.drop", drop, (2,)),
        "partition": Builtin("list.partition", partition, (2,)),
        "group_by": Builtin("list.group_by", group_by, (2,)),
        "sort_by": Builtin("list.sort_by", lambda I, a, s: _sort(I, a, s), (2,)),
        "flat_map": Builtin("list.flat_map", flat_map, (2,)),
        "zip": B["zip"],
        "enumerate": B["enumerate"],
        "flatten": B["flatten"],
        "uniq": B["uniq"],
        "reverse": B["reverse"],
        "slice": B["slice"],
        "range": B["range"],
    }
    return Module("list", _scope_of(d, I))


# ------------------------------ map ------------------------------
def mod_map(I):
    B = I.builtins.vars

    def get(I, a, s):
        m = _needmap(a[0], s, "map.get")
        if isinstance(a[1], (list, dict)):
            raise ThrumError("map key cannot be %s" % tname(a[1]), s)
        return m.get(a[1], a[2] if len(a) > 2 else None)

    def has(I, a, s):
        m = _needmap(a[0], s, "map.has")
        if isinstance(a[1], (list, dict)):
            return False
        return a[1] in m

    def merge(I, a, s):
        return {**_needmap(a[0], s, "map.merge"), **_needmap(a[1], s, "map.merge")}

    def pick(I, a, s):
        m = _needmap(a[0], s, "map.pick")
        ks = _needlist(a[1], s, "map.pick")
        return {k: m[k] for k in ks if not isinstance(k, (list, dict)) and k in m}

    def omit(I, a, s):
        m = _needmap(a[0], s, "map.omit")
        ks = set(k for k in _needlist(a[1], s, "map.omit") if not isinstance(k, (list, dict)))
        return {k: v for k, v in m.items() if k not in ks}

    def map_vals(I, a, s):
        m = _needmap(a[0], s, "map.map_vals")
        return {k: I.call(a[1], [v], s) for k, v in m.items()}

    def filter_entries(I, a, s):
        m = _needmap(a[0], s, "map.filter_entries")
        return {k: v for k, v in m.items() if truthy(I.call(a[1], [k, v], s))}

    def from_entries(I, a, s):
        pairs = _needlist(a[0], s, "map.from_entries")
        out = {}
        for p in pairs:
            if not isinstance(p, list) or len(p) != 2:
                raise ThrumError("from_entries needs [key, val] pairs", s)
            if isinstance(p[0], (list, dict)):
                raise ThrumError("map key cannot be %s" % tname(p[0]), s)
            out[p[0]] = p[1]
        return out

    d = {
        "get": Builtin("map.get", get, (2, 3)),
        "has": Builtin("map.has", has, (2,)),
        "merge": Builtin("map.merge", merge, (2,)),
        "pick": Builtin("map.pick", pick, (2,)),
        "omit": Builtin("map.omit", omit, (2,)),
        "map_vals": Builtin("map.map_vals", map_vals, (2,)),
        "filter_entries": Builtin("map.filter_entries", filter_entries, (2,)),
        "from_entries": Builtin("map.from_entries", from_entries, (1,)),
        "keys": B["keys"],
        "vals": B["vals"],
        "items": B["items"],
    }
    return Module("map", _scope_of(d, I))


# ------------------------------ test ------------------------------
def mod_test(I):
    state = {"passed": 0, "failed": 0, "failures": []}

    def eq(I, a, s):
        if not veq(a[0], a[1]):
            msg = vstr(a[2]) if len(a) > 2 else ""
            detail = "expected %s == %s" % (vstr(a[0], True), vstr(a[1], True))
            if msg:
                detail = "%s (%s)" % (msg, detail)
            raise ThrumError("test assertion failed: " + detail, s)
        return True

    def neq(I, a, s):
        if veq(a[0], a[1]):
            msg = vstr(a[2]) if len(a) > 2 else ""
            detail = "expected %s != %s" % (vstr(a[0], True), vstr(a[1], True))
            if msg:
                detail = "%s (%s)" % (msg, detail)
            raise ThrumError("test assertion failed: " + detail, s)
        return True

    def approx(I, a, s):
        x, y = I.needq(a[0], s), I.needq(a[1], s)
        k = I.needq(a[2], s).v if len(a) > 2 else 3.0
        if not qapprox(x, y, k):
            raise ThrumError("test approx failed: %s ~ %s" % (vstr(x), vstr(y)), s)
        return True

    def is_true(I, a, s):
        if not truthy(a[0]):
            msg = vstr(a[1]) if len(a) > 1 else "expected truthy value, got %s" % vstr(a[0], True)
            raise ThrumError("test assertion failed: " + msg, s)
        return True

    def fails(I, a, s):
        substr = _needstr(a[1], s, "test.fails") if len(a) > 1 else None
        try:
            I.call(a[0], [], s)
        except ThrumError as ex:
            if substr and substr not in ex.msg:
                raise ThrumError("test.fails expected error containing %r, got %r" % (substr, ex.msg), s)
            return True
        raise ThrumError("test.fails expected function to fail, but it succeeded", s)

    def run_case(I, a, s):
        name = _needstr(a[0], s, "test.run")
        try:
            I.call(a[1], [], s)
            state["passed"] += 1
            return True
        except ThrumError as ex:
            state["failed"] += 1
            state["failures"].append({"name": name, "error": ex.msg})
            return False

    def summary(I, a, s):
        return {
            "passed": mkq(float(state["passed"])),
            "failed": mkq(float(state["failed"])),
            "total": mkq(float(state["passed"] + state["failed"])),
            "failures": list(state["failures"]),
        }

    d = {
        "eq": Builtin("test.eq", eq, (2, 3)),
        "neq": Builtin("test.neq", neq, (2, 3)),
        "approx": Builtin("test.approx", approx, (2, 3)),
        "ok": Builtin("test.ok", is_true, (1, 2)),
        "fails": Builtin("test.fails", fails, (1, 2)),
        "run": Builtin("test.run", run_case, (2,)),
        "summary": Builtin("test.summary", summary, (0,)),
    }
    return Module("test", _scope_of(d, I))


STDMODS = {
    "math": mod_math,
    "const": mod_const,
    "stat": mod_stat,
    "units": mod_units,
    "text": mod_text,
    "regex": mod_regex,
    "json": mod_json,
    "csv": mod_csv,
    "time": mod_time,
    "path": mod_path,
    "list": mod_list,
    "map": mod_map,
    "test": mod_test,
}
