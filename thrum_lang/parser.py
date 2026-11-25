"""Recursive-descent parser for Thrum."""

from .ast_nodes import Node, site_of
from .errors import ThrumError
from .lexer import lex
from .units import D0, parse_unit, scan_unit
from .values import _ESC, mkq, unescape_text

PREC = {
    "or": 1, "and": 2,
    "==": 4, "!=": 4, "<": 4, ">": 4, "<=": 4, ">=": 4, "~": 4, "in": 4, "notin": 4,
    "??": 5, "|>": 6, "..": 7, "..<": 7, "+": 8, "-": 8, "*": 9, "/": 9, "%": 9,
}
KINDS = ("num", "text", "bool", "list", "map", "fn", "any", "module", "cap", "record")
_NON_CONTRACT_OPS = set("= += -= *= /= ^= %= == != < <= > >= + - * / % ^ ?? |> .. ..< ) , . [ : (".split())


class P:
    def __init__(self, toks, src):
        self.t, self.i, self.src, self.rel, self.in_fn = toks, 0, src, True, False

    def peek(self, k=0):
        return self.t[min(self.i + k, len(self.t) - 1)]

    def at(self, kind, val=None):
        t = self.peek()
        return t.kind == kind and (val is None or t.val == val)

    def op(self, v):
        t = self.peek()
        return t.kind in ("OP", "KW") and t.val == v

    def take(self, kind=None, val=None):
        if kind and not self.at(kind, val):
            t = self.peek()
            raise ThrumError(
                "expected %s%s but found %s"
                % (kind, " '%s'" % val if val else "", t.val if t.val is not None else t.kind),
                site_of(t),
            )
        t = self.peek()
        self.i += 1
        return t

    def soft(self, w):
        t = self.peek()
        if t.kind == "NAME" and t.val == w:
            self.i += 1
            return True
        return False

    def srcslice(self, a, b):
        return self.src[a:b]

    def program(self):
        out = []
        while not self.at("EOF"):
            if self.at("NEWLINE"):
                self.take()
                continue
            out.append(self.stmt())
        return out

    def block(self, rel=None, in_fn=None):
        old_rel, old_fn = self.rel, self.in_fn
        if rel is not None:
            self.rel = rel
        if in_fn is not None:
            self.in_fn = in_fn
        if self.at("NEWLINE"):
            self.take()
            self.take("INDENT")
            body = []
            while not self.at("DEDENT") and not self.at("EOF"):
                if self.at("NEWLINE"):
                    self.take()
                    continue
                body.append(self.stmt())
            if self.at("DEDENT"):
                self.take()
        else:
            body = [self.stmt()]
        self.rel, self.in_fn = old_rel, old_fn
        return body

    def stmt(self):
        t = self.peek()
        s = site_of(t)
        k = t.val if t.kind == "KW" else None
        n = None
        if k in ("var", "let", "pin"):
            n = self.decl()
        elif k == "fn" and self.peek(1).kind == "NAME":
            n = self.fndecl()
        elif (
            t.kind == "NAME"
            and t.val == "record"
            and self.peek(1).kind == "NAME"
            and self.peek(2).kind == "OP"
            and self.peek(2).val == ":"
        ):
            n = self.recorddecl()
        elif k == "if":
            n = self.ifstmt()
        elif k == "for":
            self.take()
            a = self.take("NAME").val
            b = None
            if self.op(","):
                self.take()
                b = self.take("NAME").val
            self.take("KW", "in")
            it = self.expr()
            self.take("OP", ":")
            n = Node("for", s, a=a, b=b, iter=it, body=self.block())
        elif k == "while":
            self.take()
            c = self.expr()
            self.take("OP", ":")
            n = Node("while", s, cond=c, body=self.block())
        elif k == "repeat":
            self.take()
            c = self.expr()
            self.take("OP", ":")
            n = Node("repeat", s, count=c, body=self.block())
        elif k == "match":
            n = self.matchstmt()
        elif k == "try":
            self.take()
            self.take("OP", ":")
            body = self.block()
            self.take("KW", "catch")
            nm = self.take("NAME").val
            self.take("OP", ":")
            n = Node("try", s, body=body, name=nm, handler=self.block())
        elif k == "with":
            self.take()
            caps = []
            while True:
                nm = self.take("NAME").val
                args = []
                if self.op("("):
                    self.take()
                    if not self.op(")"):
                        args.append(self.expr())
                        while self.op(","):
                            self.take()
                            args.append(self.expr())
                    self.take("OP", ")")
                caps.append((nm, args))
                if not self.op(","):
                    break
                self.take()
            self.take("OP", ":")
            n = Node("with", s, caps=caps, body=self.block())
        elif k == "on":
            self.take()
            st = self.peek().start
            c = self.expr()
            en = self.t[self.i - 1].end
            mode = "rise"
            if self.peek().kind == "NAME" and self.peek().val in ("rise", "fall", "always"):
                mode = self.take().val
            self.take("OP", ":")
            n = Node("on", s, cond=c, mode=mode, body=self.block(rel=False), src=self.srcslice(st, en))
        elif k == "settle":
            self.take()
            mx, tol = 100, None
            if self.soft("max"):
                mx = int(self.take("QTY").val[0])
            if self.soft("tol"):
                tol = self.expr()
            self.take("OP", ":")
            n = Node("settle", s, maxit=mx, tol=tol, body=self.block(rel=True))
        elif k == "live":
            self.take()
            self.take("OP", ":")
            n = Node("live", s, body=self.block(rel=True))
        elif k == "whatif":
            self.take()
            pairs = []
            while True:
                nm = self.take("NAME").val
                self.take("OP", "=")
                pairs.append((nm, self.expr()))
                if not self.op(","):
                    break
                self.take()
            self.take("OP", ":")
            n = Node("whatif", s, pairs=pairs, body=self.block())
        elif k in ("why", "budget"):
            self.take()
            nm = self.take("NAME").val
            n = Node(k, s, name=nm)
        elif k == "use":
            self.take()
            path = None
            nm = None
            modpath = None
            if self.at("TEXT"):
                path = unescape_text(self.take().val)
            elif self.at("RAWTEXT"):
                path = self.take().val
            else:
                parts = [self.take("NAME").val]
                while self.op("."):
                    self.take()
                    parts.append(self.take("NAME").val)
                if len(parts) == 1:
                    nm = parts[0]
                else:
                    modpath = parts
            alias = None
            if self.op("as"):
                self.take()
                alias = self.take("NAME").val
            n = Node("use", s, path=path, name=nm, modpath=modpath, alias=alias)
        elif k == "return":
            self.take()
            v = None if (self.at("NEWLINE") or self.at("DEDENT") or self.at("EOF")) else self.expr()
            n = Node("return", s, value=v)
        elif k == "break":
            self.take()
            n = Node("break", s)
        elif k == "continue":
            self.take()
            n = Node("continue", s)
        elif k == "fail":
            self.take()
            n = Node("fail", s, value=self.expr())
        elif k == "assert":
            self.take()
            c = self.expr()
            m = None
            if self.op(","):
                self.take()
                m = self.expr()
            n = Node("assert", s, cond=c, msg=m)
        elif (
            self.in_fn
            and t.kind == "NAME"
            and t.val in ("requires", "ensures")
            and self.peek(1).kind not in ("NEWLINE", "DEDENT", "EOF")
            and not (self.peek(1).kind == "OP" and self.peek(1).val in _NON_CONTRACT_OPS)
        ):
            w = self.take().val
            st = self.peek().start
            c = self.expr()
            en = self.t[self.i - 1].end
            m = None
            if self.op(","):
                self.take()
                m = self.expr()
            n = Node("contract", s, which=w, cond=c, msg=m, src=self.srcslice(st, en))
        else:
            e = self.expr()
            if self.peek().kind == "OP" and self.peek().val in ("=", "+=", "-=", "*=", "/=", "^=", "%="):
                o = self.take().val
                v = self.expr()
                n = Node("assign", s, target=e, op=o, value=v)
            else:
                n = Node("exprstmt", s, value=e)
        while self.at("NEWLINE"):
            self.take()
        return n

    def decl(self):
        kw = self.take().val
        nt = self.take("NAME")
        s = site_of(nt)
        self.take("OP", "=")
        st = self.peek().start
        e = self.expr()
        en = self.t[self.i - 1].end
        init = None
        if self.soft("init"):
            init = self.expr()
        return Node("decl", s, kw=kw, name=nt.val, value=e, init=init, src=self.srcslice(st, en), live=self.rel)

    def recorddecl(self):
        self.take()  # 'record'
        nt = self.take("NAME")
        s = site_of(nt)
        self.take("OP", ":")
        self.take("NEWLINE")
        self.take("INDENT")
        fields = []
        while not self.at("DEDENT") and not self.at("EOF"):
            if self.at("NEWLINE"):
                self.take()
                continue
            ft = self.take("NAME")
            fs = site_of(ft)
            ty = None
            df = None
            if self.op(":"):
                self.take()
                ty = self.typeann()
            if self.op("="):
                self.take()
                df = self.expr()
            fields.append(Node("field", fs, name=ft.val, type=ty, default=df))
            while self.at("NEWLINE"):
                self.take()
        if self.at("DEDENT"):
            self.take()
        if not fields:
            raise ThrumError("record '%s' must declare at least one field" % nt.val, s)
        return Node("record", s, name=nt.val, fields=fields)

    def params(self):
        self.take("OP", "(")
        ps = []
        while not self.op(")"):
            nt = self.take("NAME")
            ty = None
            df = None
            if self.op(":"):
                self.take()
                ty = self.typeann()
            if self.op("="):
                self.take()
                df = self.expr()
            ps.append(Node("param", site_of(nt), name=nt.val, type=ty, default=df))
            if not self.op(","):
                break
            self.take()
        self.take("OP", ")")
        return ps

    def typeann(self):
        t = self.peek()
        if t.kind == "NAME" and t.val in KINDS:
            self.take()
            return ("kind", t.val)
        if t.kind == "KW" and t.val == "nil":
            self.take()
            return ("kind", "nil")
        r = scan_unit(self.src, t.start)
        if r:
            sc, d, end = r
            while self.peek().kind != "EOF" and self.peek().start < end:
                self.i += 1
            return ("dim", d, self.src[t.start:end])
        if t.kind == "NAME":
            parts = [self.take("NAME").val]
            while self.op("."):
                self.take()
                parts.append(self.take("NAME").val)
            return ("record", parts[-1], ".".join(parts))
        raise ThrumError("expected a type or unit here", site_of(t))

    def take_unit(self):
        t = self.peek()
        r = scan_unit(self.src, t.start)
        if not r:
            raise ThrumError("expected a unit here", site_of(t))
        sc, d, end = r
        while self.peek().kind != "EOF" and self.peek().start < end:
            self.i += 1
        return sc, d, self.src[t.start:end]

    def fndecl(self):
        self.take()
        nt = self.take("NAME")
        s = site_of(nt)
        ps = self.params()
        ret = None
        if self.op("->"):
            self.take()
            ret = self.typeann()
        self.take("OP", ":")
        body = self.block(rel=False, in_fn=True)
        req = [x for x in body if x.kind == "contract" and x.which == "requires"]
        ens = [x for x in body if x.kind == "contract" and x.which == "ensures"]
        body = [x for x in body if x.kind != "contract"]
        return Node("fndecl", s, name=nt.val, params=ps, ret=ret, body=body, requires=req, ensures=ens)

    def ifstmt(self):
        s = site_of(self.take())
        c = self.expr()
        self.take("OP", ":")
        b = self.block()
        arms = [(c, b)]
        els = None
        while self.at("KW", "elif"):
            self.take()
            c2 = self.expr()
            self.take("OP", ":")
            arms.append((c2, self.block()))
        if self.at("KW", "else"):
            self.take()
            self.take("OP", ":")
            els = self.block()
        return Node("if", s, arms=arms, els=els)

    def matchstmt(self):
        s = site_of(self.take())
        subj = self.expr()
        self.take("OP", ":")
        self.take("NEWLINE")
        self.take("INDENT")
        cases = []
        while not self.at("DEDENT") and not self.at("EOF"):
            if self.at("NEWLINE"):
                self.take()
                continue
            self.take("KW", "case")
            pat = self.pattern()
            guard = None
            if self.at("KW", "if"):
                self.take()
                guard = self.expr()
            self.take("OP", ":")
            cases.append((pat, guard, self.block(rel=False)))
        if self.at("DEDENT"):
            self.take()
        return Node("match", s, subject=subj, cases=cases)

    def _map_pat_items(self):
        self.take("OP", "{")
        items = []
        while not self.op("}"):
            if self.at("TEXT"):
                key = unescape_text(self.take().val)
                self.take("OP", ":")
                items.append((key, self.pattern()))
            elif self.at("RAWTEXT"):
                key = self.take().val
                self.take("OP", ":")
                items.append((key, self.pattern()))
            else:
                kt = self.take("NAME")
                key = kt.val
                if self.op(":"):
                    self.take()
                    items.append((key, self.pattern()))
                else:
                    items.append((key, ("bind", key)))
            if not self.op(","):
                break
            self.take()
        self.take("OP", "}")
        return items

    def pattern(self):
        t = self.peek()
        if t.kind == "NAME" and t.val == "_":
            self.take()
            return ("wild",)
        if self.op("-"):
            self.take()
            qt = self.take("QTY")
            v, u, disp = qt.val
            d = parse_unit(disp[1])[1] if disp else D0
            return ("lit", mkq(-v, d, u, disp))
        if t.kind == "QTY":
            self.take()
            v, u, disp = t.val
            d = parse_unit(disp[1])[1] if disp else D0
            return ("lit", mkq(v, d, u, disp))
        if t.kind == "TEXT":
            self.take()
            return ("lit", unescape_text(t.val))
        if t.kind == "RAWTEXT":
            self.take()
            return ("lit", t.val)
        if t.kind == "KW" and t.val in ("true", "false", "nil"):
            self.take()
            return ("lit", {"true": True, "false": False, "nil": None}[t.val])
        if t.kind == "NAME":
            if self.peek(1).kind == "OP" and self.peek(1).val in ("{", "."):
                parts = [self.take("NAME").val]
                while self.op("."):
                    self.take()
                    parts.append(self.take("NAME").val)
                if self.op("{"):
                    return ("record", parts[-1], self._map_pat_items())
                raise ThrumError("bad pattern", site_of(t))
            self.take()
            return ("bind", t.val)
        if self.op("{"):
            return ("map", self._map_pat_items())
        if self.op("["):
            self.take()
            items = []
            rest = None
            while not self.op("]"):
                if self.op("*"):
                    self.take()
                    rest = self.take("NAME").val
                    break
                items.append(self.pattern())
                if not self.op(","):
                    break
                self.take()
            self.take("OP", "]")
            return ("list", items, rest)
        raise ThrumError("bad pattern", site_of(t))

    # ---- expressions ----
    def expr(self, minp=1):
        left = self.unary()
        while True:
            t = self.peek()
            op = None
            if t.kind in ("OP", "KW"):
                if t.val == "not" and self.peek(1).kind == "KW" and self.peek(1).val == "in":
                    op = "notin"
                elif t.val in PREC:
                    op = t.val
            if op is None or PREC[op] < minp:
                break
            self.take()
            if op == "notin":
                self.take()
            right = self.expr(PREC[op] + 1)
            if op == "|>":
                if right.kind == "call":
                    right.args.insert(0, left)
                elif right.kind in ("name", "dot"):
                    right = Node("call", right.site, fn=right, args=[left])
                else:
                    raise ThrumError("right side of '|>' must be a call", right.site)
                left = right
            else:
                left = Node("bin", left.site, op=op, left=left, right=right)
        return left

    def unary(self):
        t = self.peek()
        if self.op("-"):
            self.take()
            return Node("un", site_of(t), op="-", operand=self.unary())
        if self.op("+"):
            self.take()
            return self.unary()
        if self.op("not"):
            self.take()
            return Node("un", site_of(t), op="not", operand=self.expr(3))
        return self.power()

    def power(self):
        b = self.postfix()
        if self.op("^"):
            self.take()
            return Node("bin", b.site, op="^", left=b, right=self.unary())
        return b

    def _map_entries(self):
        self.take("OP", "{")
        entries = []
        while not self.op("}"):
            if self.op("["):
                self.take()
                k = self.expr()
                self.take("OP", "]")
            elif self.at("TEXT"):
                tk = self.take()
                ks = site_of(tk)
                k = Node("text", ks, parts=self.template(tk.val, ks))
            elif self.at("RAWTEXT"):
                tk = self.take()
                k = Node("q", site_of(tk), value=tk.val)
            else:
                nt = self.take("NAME")
                ns = site_of(nt)
                k = Node("q", ns, value=nt.val)
                if not self.op(":"):
                    entries.append((k, Node("name", ns, name=nt.val)))
                    if not self.op(","):
                        break
                    self.take()
                    continue
            self.take("OP", ":")
            entries.append((k, self.expr()))
            if not self.op(","):
                break
            self.take()
        self.take("OP", "}")
        return entries

    def postfix(self):
        e = self.atom()
        while True:
            if self.op("("):
                self.take()
                args = []
                while not self.op(")"):
                    args.append(self.expr())
                    if not self.op(","):
                        break
                    self.take()
                self.take("OP", ")")
                e = Node("call", e.site, fn=e, args=args)
            elif self.op("["):
                self.take()
                start_ex = None if self.op(":") else self.expr()
                if self.op(":"):
                    self.take()
                    stop_ex = None if (self.op(":") or self.op("]")) else self.expr()
                    step_ex = None
                    if self.op(":"):
                        self.take()
                        if not self.op("]"):
                            step_ex = self.expr()
                    self.take("OP", "]")
                    e = Node("slice", e.site, obj=e, start=start_ex, stop=stop_ex, step=step_ex)
                else:
                    self.take("OP", "]")
                    e = Node("index", e.site, obj=e, idx=start_ex)
            elif self.op("{"):
                entries = self._map_entries()
                e = Node("rec_lit", e.site, target=e, entries=entries)
            elif self.op("."):
                self.take()
                t_mem = self.peek()
                if t_mem.kind in ("NAME", "KW"):
                    nt = self.take()
                else:
                    nt = self.take("NAME")
                e = Node("dot", e.site, obj=e, name=nt.val)
            elif self.op("as"):
                self.take()
                sc, d, sym = self.take_unit()
                e = Node("convert", e.site, obj=e, unit=sym)
            else:
                break
        return e

    def atom(self):
        t = self.peek()
        s = site_of(t)
        if t.kind == "QTY":
            self.take()
            v, u, disp = t.val
            d = parse_unit(disp[1])[1] if disp else D0
            return Node("q", s, value=mkq(v, d, u, disp))
        if t.kind == "TEXT":
            self.take()
            return Node("text", s, parts=self.template(t.val, s))
        if t.kind == "RAWTEXT":
            self.take()
            return Node("q", s, value=t.val)
        if t.kind == "KW" and t.val in ("true", "false", "nil"):
            self.take()
            return Node("q", s, value={"true": True, "false": False, "nil": None}[t.val])
        if t.kind == "KW" and t.val == "fn":
            self.take()
            ps = self.params()
            ret = None
            if self.op("->"):
                self.take()
                ret = self.typeann()
            self.take("OP", ":")
            body = self.block(rel=False, in_fn=True)
            req = [x for x in body if x.kind == "contract" and x.which == "requires"]
            ens = [x for x in body if x.kind == "contract" and x.which == "ensures"]
            body = [x for x in body if x.kind != "contract"]
            return Node("fnexpr", s, params=ps, ret=ret, body=body, requires=req, ensures=ens)
        if t.kind == "NAME":
            self.take()
            return Node("name", s, name=t.val)
        if self.op("("):
            self.take()
            e = self.expr()
            self.take("OP", ")")
            return e
        if self.op("\\"):
            self.take()
            return Node("lambda", s, body=self.expr())
        if self.op("["):
            self.take()
            if self.op("]"):
                self.take()
                return Node("list", s, items=[])
            first = self.expr()
            if self.at("KW", "for"):
                self.take()
                nm = self.take("NAME").val
                self.take("KW", "in")
                it = self.expr()
                cond = None
                if self.at("KW", "if"):
                    self.take()
                    cond = self.expr()
                self.take("OP", "]")
                return Node("comp", s, elem=first, var=nm, iter=it, cond=cond)
            items = [first]
            while self.op(","):
                self.take()
                if self.op("]"):
                    break
                items.append(self.expr())
            self.take("OP", "]")
            return Node("list", s, items=items)
        if self.op("{"):
            return Node("map", s, entries=self._map_entries())
        raise ThrumError("unexpected %s" % (t.val if t.val is not None else t.kind), s)

    def template(self, raw, site):
        parts, i, buf, n = [], 0, [], len(raw)
        while i < n:
            c = raw[i]
            if c == "\\" and i + 1 < n:
                buf.append(_ESC.get(raw[i + 1], raw[i + 1]))
                i += 2
                continue
            if c == "{":
                j, depth, q = i + 1, 1, None
                while j < n:
                    ch = raw[j]
                    if q:
                        if ch == "\\":
                            j += 2
                            continue
                        if ch == q:
                            q = None
                    elif ch in "\"'":
                        q = ch
                    elif ch == "{":
                        depth += 1
                    elif ch == "}":
                        depth -= 1
                        if depth == 0:
                            break
                    j += 1
                if j >= n:
                    raise ThrumError("unterminated '{' in text", site)
                if buf:
                    parts.append(("str", "".join(buf)))
                    buf = []
                inner = raw[i + 1 : j]
                sub = P(lex(inner), inner)
                ex = sub.expr()
                if not sub.at("EOF") and not sub.at("NEWLINE"):
                    raise ThrumError("unexpected trailing token in interpolation", site_of(sub.peek()))
                parts.append(("expr", ex))
                i = j + 1
                continue
            buf.append(c)
            i += 1
        if buf:
            parts.append(("str", "".join(buf)))
        return parts


def parse(src):
    return P(lex(src), src).program()
