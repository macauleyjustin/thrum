"""Runtime value types, formatting, equality, and quantity arithmetic."""

import math
from fractions import Fraction

from .errors import ThrumError
from .units import D0, DIMNAME, dadd, dimlabel, dimless, dimstr, dscale, dsub, expstr, parse_unit


class Q:
    __slots__ = ("v", "d", "u", "disp")

    def __init__(self, v, d=D0, u=0.0, disp=None):
        fv = float(v)
        self.v = 0.0 if fv == 0.0 else fv
        self.d = d
        self.u = float(u)
        self.disp = disp

    def __eq__(self, other):
        return isinstance(other, Q) and self.d == other.d and self.v == other.v

    def __hash__(self):
        return hash((self.v, self.d))

    def __repr__(self):
        return "Q(%s)" % qfmt(self)


class RecordField:
    __slots__ = ("name", "type", "default", "site")

    def __init__(self, name, type=None, default=None, site=None):
        self.name = name
        self.type = type
        self.default = default
        self.site = site


class RecordType:
    __slots__ = ("name", "fields", "field_names", "field_map", "env", "site")

    def __init__(self, name, fields, env=None, site=None):
        self.name = name
        self.fields = fields
        self.field_names = [f.name for f in fields]
        self.field_map = {f.name: f for f in fields}
        self.env = env
        self.site = site


class RecordVal:
    __slots__ = ("rtype", "fields")

    def __init__(self, rtype, fields):
        self.rtype = rtype
        self.fields = fields

    def __repr__(self):
        return vstr(self)


class Fn:
    __slots__ = ("name", "params", "body", "env", "ret", "requires", "ensures", "site")


class Builtin:
    __slots__ = ("name", "f", "arity")

    def __init__(self, name, f, arity):
        self.name, self.f, self.arity = name, f, arity


class Module:
    __slots__ = ("name", "scope")

    def __init__(self, name, scope):
        self.name, self.scope = name, scope


class Cap:
    __slots__ = ("name", "methods", "cleanup")

    def __init__(self, name, methods, cleanup=None):
        self.name, self.methods, self.cleanup = name, methods, cleanup


def wrap(s):
    return "(" + s + ")" if ("*" in s or "/" in s) else s


def wrap_pow(s):
    return "(" + s + ")" if ("*" in s or "/" in s or "^" in s) else s


def mkq(v, d=D0, u=0.0, disp=None):
    if disp is not None:
        sc, sym = disp
        if sc == 1.0 and d in DIMNAME:
            disp = (1.0, DIMNAME[d])
        elif sc == 0 or len(sym) > 20:
            disp = None
    if disp is None and d in DIMNAME:
        disp = (1.0, DIMNAME[d])
    return Q(v, d, u, disp)


def _fix(v, nd):
    if nd >= 0:
        return "%.*f" % (min(nd, 16), v)
    return "%.0f" % round(v, nd)


def qfmt(x):
    sc, sym = x.disp if x.disp else (1.0, dimstr(x.d))
    if sc == 0:
        sc = 1.0
    v, u = x.v / sc, x.u / sc
    if v == 0.0:
        v = 0.0
    tail = (" " + sym) if sym else ""
    if not math.isfinite(v):
        return ("inf" if v > 0 else "-inf" if v < 0 else "nan") + tail
    if u > 0 and math.isfinite(u):
        e = math.floor(math.log10(abs(u)))
        ve = math.floor(math.log10(abs(v))) if v else e
        if max(abs(e), abs(ve)) > 4:
            k = min(max(ve - e + 1, 0), 16)
            scale10 = 10.0 ** ve
            if scale10 == 0.0 or not math.isfinite(scale10):
                return "%.12g ± %.12g%s" % (v, u, tail)
            return "(%.*f ± %.*f)e%+d%s" % (k, v / scale10, k, u / scale10, ve, tail)
        nd = 1 - e
        return "%s ± %s%s" % (_fix(v, nd), _fix(u, nd), tail)
    if v == int(v) and abs(v) < 1e15:
        return "%d%s" % (int(v), tail)
    return "%.12g%s" % (v, tail)


def tname(v):
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, Q):
        return "num"
    if isinstance(v, str):
        return "text"
    if isinstance(v, list):
        return "list"
    if isinstance(v, dict):
        return "map"
    if isinstance(v, RecordVal):
        return v.rtype.name
    if isinstance(v, RecordType):
        return "record"
    if isinstance(v, (Fn, Builtin)):
        return "fn"
    if isinstance(v, Module):
        return "module"
    if isinstance(v, Cap):
        return "cap"
    return "?"


def vstr(v, quote=False):
    if v is None:
        return "nil"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, Q):
        return qfmt(v)
    if isinstance(v, str):
        return '"%s"' % v if quote else v
    if isinstance(v, list):
        return "[" + ", ".join(vstr(x, True) for x in v) + "]"
    if isinstance(v, dict):
        items = []
        for k, x in v.items():
            ks = vstr(k, isinstance(k, str) and not k.isidentifier())
            items.append("%s: %s" % (ks, vstr(x, True)))
        return "{" + ", ".join(items) + "}"
    if isinstance(v, RecordVal):
        items = ["%s: %s" % (k, vstr(v.fields[k], True)) for k in v.rtype.field_names]
        return "%s{%s}" % (v.rtype.name, ", ".join(items))
    if isinstance(v, RecordType):
        return "<record %s>" % v.name
    if isinstance(v, Fn):
        return "<fn %s>" % (v.name or "anonymous")
    if isinstance(v, Builtin):
        return "<builtin %s>" % v.name
    if isinstance(v, Module):
        return "<module %s>" % v.name
    if isinstance(v, Cap):
        return "<cap %s>" % v.name
    return str(v)


def veq(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return a is b
    if a is None or b is None:
        return a is None and b is None
    if isinstance(a, Q) and isinstance(b, Q):
        return a.d == b.d and a.v == b.v
    if isinstance(a, Q) or isinstance(b, Q):
        return False
    if isinstance(a, RecordVal) and isinstance(b, RecordVal):
        return a.rtype is b.rtype and all(veq(a.fields[k], b.fields[k]) for k in a.rtype.field_names)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(veq(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(veq(a[k], b[k]) for k in a)
    return type(a) is type(b) and a == b


def truthy(v):
    if v is None or v is False:
        return False
    if v is True:
        return True
    if isinstance(v, Q):
        return v.v != 0
    if isinstance(v, (str, list, dict)):
        return len(v) > 0
    return True


def relchg(a, b):
    if isinstance(a, Q) and isinstance(b, Q) and a.d == b.d:
        return abs(a.v - b.v) / max(abs(a.v), abs(b.v), 1e-12)
    if isinstance(a, RecordVal) and isinstance(b, RecordVal) and a.rtype is b.rtype:
        return max((relchg(a.fields[k], b.fields[k]) for k in a.rtype.field_names), default=0.0)
    return 0.0 if veq(a, b) else 1.0


# ============================== quantity arithmetic ==============================
def qadd(a, b, sign, site=None):
    if a.d != b.d:
        raise ThrumError(
            "dimension mismatch: %s %s %s" % (dimlabel(a), "+" if sign > 0 else "-", dimlabel(b)),
            site,
            "convert with 'as', or check your units — Thrum never guesses",
        )
    return mkq(a.v + sign * b.v, a.d, math.hypot(a.u, b.u), a.disp or b.disp)


def _dcomb(a, b, op, res_dim):
    if dimless(res_dim) and not (dimless(a.d) and dimless(b.d)):
        return None
    da, db = a.disp, b.disp
    if da is None and dimless(a.d):
        if op == "*":
            return db
        return (1.0 / db[0], "1/" + wrap(db[1])) if db else None
    if db is None and dimless(b.d):
        return da
    if da and dimless(a.d) and da[0] == 1.0:
        if op == "*":
            return db
        return (1.0 / db[0], "1/" + wrap(db[1])) if db else None
    if db and dimless(b.d) and db[0] == 1.0:
        return da
    if not da and not db:
        return None
    cancelled = any((x * y < 0) if op == "*" else (x * y > 0) for x, y in zip(a.d, b.d))
    if cancelled:
        return None
    sa, sca = (da[1], da[0]) if da else (dimstr(a.d) or "1", 1.0)
    sb, scb = (db[1], db[0]) if db else (dimstr(b.d) or "1", 1.0)
    if op == "*":
        if sa == sb and sca == scb:
            return (sca * scb, wrap_pow(sa) + "^2")
        return (sca * scb, wrap(sa) + "*" + wrap(sb))
    return (sca / scb, wrap(sa) + "/" + wrap(sb))


def qmul(a, b):
    v = a.v * b.v
    rd = dadd(a.d, b.d)
    return mkq(v, rd, math.hypot(a.u * b.v, b.u * a.v), _dcomb(a, b, "*", rd))


def qdiv(a, b, site=None):
    if b.v == 0:
        raise ThrumError("division by zero", site)
    v = a.v / b.v
    rd = dsub(a.d, b.d)
    return mkq(v, rd, math.hypot(a.u / b.v, b.u * a.v / (b.v * b.v)), _dcomb(a, b, "/", rd))


def qpow(a, b, site=None):
    if not dimless(b.d):
        raise ThrumError("exponent must be dimensionless", site)
    if dimless(a.d):
        ef = b.v
        if a.v < 0 and (not math.isfinite(ef) or ef != int(ef)):
            raise ThrumError("negative base with fractional exponent", site)
        if a.v == 0 and ef < 0:
            raise ThrumError("division by zero", site)
        v = a.v ** ef
        if ef == 1.0:
            u = a.u
        elif ef == 0.0:
            u = 0.0
        else:
            u = abs(ef * v / a.v) * a.u if a.v else 0.0
        if b.u and a.v > 0:
            u = math.hypot(u, abs(v * math.log(a.v)) * b.u)
        return mkq(v, D0, u, a.disp if (a.disp and a.disp[0] != 1.0 and ef == 1.0) else None)
    if not math.isfinite(b.v):
        raise ThrumError("exponent on dimensioned quantity must be finite rational", site)
    e = Fraction(b.v).limit_denominator(1000)
    if abs(float(e) - b.v) > 1e-9:
        raise ThrumError("exponent on dimensioned quantity must be rational", site)
    if a.v < 0 and e.denominator != 1:
        raise ThrumError("negative base with fractional exponent", site)
    if a.v == 0 and e < 0:
        raise ThrumError("division by zero", site)
    ef = float(e)
    v = a.v ** ef
    if e == 1:
        u = a.u
    elif e == 0:
        u = 0.0
    else:
        u = abs(ef * v / a.v) * a.u if a.v else 0.0
    if b.u and a.v > 0:
        u = math.hypot(u, abs(v * math.log(a.v)) * b.u)
    disp = None
    if a.disp and not (dimless(a.d) and a.disp[0] == 1.0):
        disp = (a.disp[0] ** ef, wrap_pow(a.disp[1]) + "^" + expstr(e))
    return mkq(v, dscale(a.d, e), u, disp)


def qmod(a, b, site=None):
    if a.d != b.d:
        raise ThrumError("dimension mismatch in '%'", site)
    if b.v == 0:
        raise ThrumError("modulo by zero", site)
    return mkq(math.fmod(a.v, b.v), a.d, a.u, a.disp)


def qcmp(a, b, op, site=None):
    if a.d != b.d:
        raise ThrumError("cannot compare %s with %s" % (dimlabel(a), dimlabel(b)), site)
    x, y = a.v, b.v
    return {"<": x < y, ">": x > y, "<=": x <= y, ">=": x >= y}[op]


def qapprox(a, b, k=3.0):
    if a.d != b.d:
        return False
    tol = max(k * math.hypot(a.u, b.u), 1e-9 * max(abs(a.v), abs(b.v)))
    return abs(a.v - b.v) <= tol


def convert(x, unit, site=None):
    sc, d = parse_unit(unit)
    if d != x.d:
        raise ThrumError("cannot convert %s to %s" % (dimlabel(x), unit), site)
    return Q(x.v, x.d, x.u, (sc, unit))


# ============================== text escapes ==============================
_ESC = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "{": "{", "}": "}", "0": "\0"}


def unescape_text(raw):
    out, i, n = [], 0, len(raw)
    while i < n:
        if raw[i] == "\\" and i + 1 < n:
            out.append(_ESC.get(raw[i + 1], raw[i + 1]))
            i += 2
        else:
            out.append(raw[i])
            i += 1
    return "".join(out)
