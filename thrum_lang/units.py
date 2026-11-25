"""SI base dimensions, unit registry, prefixes, and unit expression scanner."""

import math
import re
from fractions import Fraction

from .errors import ThrumError

BASE = ("m", "kg", "s", "A", "K", "mol", "cd")
D0 = (Fraction(0),) * 7


def dadd(a, b):
    return tuple(x + y for x, y in zip(a, b))


def dsub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def dscale(a, n):
    return tuple(x * n for x in a)


def dimless(d):
    return all(x == 0 for x in d)


UNITS = {}
_PREFIXABLE = []


def U(sym, scale, dim, prefixable=False, *aliases):
    UNITS[sym] = (float(scale), dim)
    for a in aliases:
        UNITS[a] = (float(scale), dim)
    if prefixable:
        _PREFIXABLE.append(sym)


def _d(**kw):
    return tuple(Fraction(kw.get(b, 0)) for b in BASE)


U("m", 1, _d(m=1), True, "meter", "meters", "metre", "metres")
U("g", 1e-3, _d(kg=1), True, "gram", "grams")
U("s", 1, _d(s=1), True, "sec", "secs", "second", "seconds")
U("A", 1, _d(A=1), True, "amp", "amps", "ampere", "amperes")
U("K", 1, _d(K=1), True, "kelvin")
U("mol", 1, _d(mol=1), True, "mole", "moles")
U("cd", 1, _d(cd=1), True, "candela")
U("N", 1, _d(kg=1, m=1, s=-2), True, "newton", "newtons")
U("Pa", 1, _d(kg=1, m=-1, s=-2), True, "pascal", "pascals")
U("J", 1, _d(kg=1, m=2, s=-2), True, "joule", "joules")
U("W", 1, _d(kg=1, m=2, s=-3), True, "watt", "watts")
U("C", 1, _d(A=1, s=1), True, "coulomb", "coulombs")
U("V", 1, _d(kg=1, m=2, s=-3, A=-1), True, "volt", "volts")
U("F", 1, _d(kg=-1, m=-2, s=4, A=2), True, "farad", "farads")
U("H", 1, _d(kg=1, m=2, s=-2, A=-2), True, "henry", "henries")
U("S", 1, _d(kg=-1, m=-2, s=3, A=2), True, "siemens")
U("Hz", 1, _d(s=-1), True, "hertz")
U("Wb", 1, _d(kg=1, m=2, s=-2, A=-1), True, "weber", "webers")
U("T", 1, _d(kg=1, s=-2, A=-1), True, "tesla", "teslas")
U("lm", 1, _d(cd=1), True, "lumen", "lumens")
U("lx", 1, _d(cd=1, m=-2), True, "lux")
U("Bq", 1, _d(s=-1), True)
U("Gy", 1, _d(m=2, s=-2), True)
U("Sv", 1, _d(m=2, s=-2), True)
U("ohm", 1, _d(kg=1, m=2, s=-3, A=-2), True, "Ω", "ohms")
U("min", 60, _d(s=1), False, "minute", "minutes")
U("h", 3600, _d(s=1), False, "hr", "hrs", "hour", "hours")
U("day", 86400, _d(s=1), False, "days")
U("yr", 31557600, _d(s=1), False, "year", "years")
U("L", 1e-3, _d(m=3), True, "liter", "liters", "litre", "litres")
U("t", 1000, _d(kg=1), True, "tonne", "tonnes")
U("eV", 1.602176634e-19, _d(kg=1, m=2, s=-2), True)
U("cal", 4.184, _d(kg=1, m=2, s=-2), True)
U("bar", 1e5, _d(kg=1, m=-1, s=-2), True)
U("atm", 101325, _d(kg=1, m=-1, s=-2))
U("angstrom", 1e-10, _d(m=1), False, "Å")
U("au", 1.495978707e11, _d(m=1))
U("ly", 9.4607304725808e15, _d(m=1))
U("pc", 3.085677581491367e16, _d(m=1), False, "parsec", "parsecs")
U("in", 0.0254, _d(m=1), False, "inch", "inches")
U("ft", 0.3048, _d(m=1), False, "foot", "feet")
U("yd", 0.9144, _d(m=1), False, "yard", "yards")
U("mi", 1609.344, _d(m=1), False, "mile", "miles")
U("lb", 0.45359237, _d(kg=1), False, "lbs", "pound", "pounds")
U("oz", 0.028349523125, _d(kg=1), False, "ounce", "ounces")
U("psi", 6894.757293168, _d(kg=1, m=-1, s=-2))
U("mph", 0.44704, _d(m=1, s=-1))
U("knot", 0.5144444444444445, _d(m=1, s=-1), False, "knots")
U("gal", 3.785411784e-3, _d(m=3), False, "gallon", "gallons")
U("rad", 1, D0, True, "radian", "radians")
U("deg", math.pi / 180, D0, False, "°", "degree", "degrees")
U("sr", 1, D0, True)
U("pct", 0.01, D0, False, "percent")
U("ppm", 1e-6, D0)
U("ppb", 1e-9, D0)
U("count", 1, D0)
U("bit", 1, D0, False, "bits")
U("byte", 8, D0, False, "bytes")

for _p, _f in [
    ("Q", 1e30), ("R", 1e27), ("Y", 1e24), ("Z", 1e21), ("E", 1e18), ("P", 1e15),
    ("T", 1e12), ("G", 1e9), ("M", 1e6), ("k", 1e3), ("h", 1e2), ("da", 1e1),
    ("d", 1e-1), ("c", 1e-2), ("m", 1e-3), ("u", 1e-6), ("µ", 1e-6), ("n", 1e-9),
    ("p", 1e-12), ("f", 1e-15), ("a", 1e-18), ("z", 1e-21), ("y", 1e-24),
    ("r", 1e-27), ("q", 1e-30),
]:
    for _s in list(_PREFIXABLE):
        _k = _p + _s
        if _k not in UNITS:
            _sc, _dd = UNITS[_s]
            UNITS[_k] = (_sc * _f, _dd)

DIMNAME = {}
for _s in ["Hz", "N", "Pa", "J", "W", "C", "V", "F", "H", "S", "Wb", "T", "lm", "lx", "Bq", "Gy", "Sv", "ohm"] + list(BASE):
    _sc, _dd = UNITS[_s]
    if _sc == 1.0 and _dd not in DIMNAME and not dimless(_dd):
        DIMNAME[_dd] = _s


def expstr(e):
    e = Fraction(e)
    return str(e.numerator) if e.denominator == 1 else "%d/%d" % (e.numerator, e.denominator)


def dimstr(d):
    if dimless(d):
        return ""
    if d in DIMNAME:
        return DIMNAME[d]

    def fmt(items):
        return "*".join(s if e == 1 else "%s^%s" % (s, expstr(e)) for s, e in items)

    pos = [(BASE[i], d[i]) for i in range(7) if d[i] > 0]
    neg = [(BASE[i], -d[i]) for i in range(7) if d[i] < 0]
    a = fmt(pos) or "1"
    if not neg:
        return a
    b = fmt(neg)
    return a + "/" + (b if len(neg) == 1 else "(" + b + ")")


def dimlabel(x):
    return dimstr(x.d) or "dimensionless"


_URE = re.compile(r"[A-Za-z_µ°Ω][A-Za-z_0-9µ°Ω]*")
_ERE = re.compile(r"[+-]?\d+(?:/\d+)?|\(([+-]?\d+(?:/\d+)?)\)")


def scan_unit(s, i):
    """Greedily parse a unit expression at s[i:]. Return (scale, dim, end) or None."""
    n = len(s)

    def term(j):
        if j < n and s[j] == "(":
            r = expr(j + 1)
            if r and r[2] < n and s[r[2]] == ")":
                return (r[0], r[1], r[2] + 1)
            return None
        if j < n and s[j] == "1":
            if j + 1 >= n or not (s[j + 1].isdigit() or s[j + 1] in "_A-Za-z."):
                return (1.0, D0, j + 1)
            return None
        m = _URE.match(s, j)
        if not m or m.group(0) not in UNITS:
            return None
        sc, d = UNITS[m.group(0)]
        k = m.end()
        if k < n and s[k] == "^":
            m2 = _ERE.match(s, k + 1)
            if m2:
                raw_exp = m2.group(1) if m2.group(1) is not None else m2.group(0)
                e = Fraction(raw_exp)
                k = m2.end()
                sc = sc ** float(e)
                d = dscale(d, e)
        return (sc, d, k)

    def expr(j):
        r = term(j)
        if not r:
            return None
        sc, d, k = r
        while k < n and s[k] in "*/·":
            op = s[k]
            r2 = term(k + 1)
            if not r2:
                break
            sc2, d2, k2 = r2
            if op == "/":
                sc /= sc2
                d = dsub(d, d2)
            else:
                sc *= sc2
                d = dadd(d, d2)
            k = k2
        return (sc, d, k)

    return expr(i)


def parse_unit(text):
    t = text.strip()
    r = scan_unit(t, 0)
    if not r or r[2] != len(t):
        raise ThrumError("unknown or malformed unit '%s'" % text)
    return r[0], r[1]
