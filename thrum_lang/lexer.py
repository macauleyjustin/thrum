"""Indentation-aware lexer with keyword-safe unit scanning and interpolated text."""

import re

from .errors import ThrumError
from .units import _URE, scan_unit

KEYWORDS = set(
    "var let pin fn return if elif else for while repeat in match case try catch "
    "fail assert with on settle live whatif why budget use as not and or break "
    "continue true false nil".split()
)
OPS = sorted(
    [
        "+/-", "|>", "??", "->", "..<", "..", "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "^=", "%=",
        "±", "+", "-", "*", "/", "^", "%", "<", ">", "=", "(", ")", "[", "]", "{", "}", ",", ":", ".", "~", "\\", ";",
    ],
    key=len,
    reverse=True,
)
NUMRE = re.compile(r"0[xX][0-9a-fA-F_]+|0[bB][01_]+|\d[\d_]*(?:\.\d[\d_]*)?(?:[eE][+-]?\d+)?")
NAMERE = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")
_UNCRE = re.compile(r"[ \t]*(?:±|\+/-)[ \t]*")


class Tok:
    __slots__ = ("kind", "val", "line", "col", "start", "end")

    def __init__(self, kind, val, line, col, start, end):
        self.kind, self.val, self.line, self.col, self.start, self.end = kind, val, line, col, start, end

    def __repr__(self):
        return "%s(%r)" % (self.kind, self.val)


def lex(src):
    toks, indents, i, line, col, depth, n = [], [0], 0, 1, 1, 0, len(src)
    ls = True

    def T(k, v, l, c, s, e):
        toks.append(Tok(k, v, l, c, s, e))

    while i < n:
        if ls and depth == 0:
            j, w = i, 0
            while j < n and src[j] in " \t":
                if src[j] == "\t":
                    raise ThrumError("tabs are not allowed for indentation", (line, 1, i, j), "use spaces")
                w += 1
                j += 1
            if j < n and src[j] == "#":
                while j < n and src[j] != "\n":
                    j += 1
            if j >= n or src[j] == "\n":
                if j < n:
                    j += 1
                    line += 1
                    col = 1
                i = j
                continue
            if w > indents[-1]:
                indents.append(w)
                T("INDENT", w, line, 1, i, j)
            else:
                while w < indents[-1]:
                    indents.pop()
                    T("DEDENT", w, line, 1, i, j)
                if w != indents[-1]:
                    raise ThrumError("inconsistent indentation", (line, 1, i, j))
            i, col, ls = j, w + 1, False
            continue
        c = src[i]
        if c == "\n":
            if depth == 0:
                T("NEWLINE", None, line, col, i, i + 1)
                ls = True
            i += 1
            line += 1
            col = 1
            continue
        if c in " \t\r":
            i += 1
            col += 1
            continue
        if c == "#":
            while i < n and src[i] != "\n":
                i += 1
            continue
        start, sl, sc = i, line, col
        if c in ("r", "R") and i + 1 < n and src[i + 1] in ('"', "'"):
            q = src[i + 1]
            triple = src.startswith(q * 3, i + 1)
            term = q * 3 if triple else q
            j = i + 1 + len(term)
            buf = []
            while j < n and not src.startswith(term, j):
                if src[j] == "\n":
                    line += 1
                buf.append(src[j])
                j += 1
            if j >= n:
                raise ThrumError("unterminated raw text literal", (sl, sc, start, j))
            j += len(term)
            T("RAWTEXT", "".join(buf), sl, sc, start, j)
            col += j - i
            i = j
            continue
        if c == '"' or c == "'":
            q = c
            triple = src.startswith(q * 3, i)
            term = q * 3 if triple else q
            j = i + len(term)
            buf = []
            while j < n and not src.startswith(term, j):
                if src[j] == "\\" and j + 1 < n:
                    if src[j + 1] == "\n":
                        line += 1
                    buf.append(src[j : j + 2])
                    j += 2
                    continue
                if src[j] == "{":
                    buf.append("{")
                    j += 1
                    bdepth = 1
                    iq = None
                    while j < n and bdepth > 0:
                        ch = src[j]
                        if iq:
                            if ch == "\\" and j + 1 < n:
                                if src[j + 1] == "\n":
                                    line += 1
                                buf.append(src[j : j + 2])
                                j += 2
                                continue
                            if ch == iq:
                                iq = None
                        elif ch in "\"'":
                            iq = ch
                        elif ch == "{":
                            bdepth += 1
                        elif ch == "}":
                            bdepth -= 1
                        if ch == "\n":
                            line += 1
                        buf.append(ch)
                        j += 1
                    if bdepth > 0:
                        raise ThrumError("unterminated '{' in text", (sl, sc, start, j))
                    continue
                if src[j] == "\n":
                    line += 1
                buf.append(src[j])
                j += 1
            if j >= n:
                raise ThrumError("unterminated text literal", (sl, sc, start, j))
            j += len(term)
            T("TEXT", "".join(buf), sl, sc, start, j)
            col += j - i
            i = j
            continue
        m = NUMRE.match(src, i)
        if m:
            raw = m.group(0).replace("_", "")
            val = (
                float(int(raw, 16))
                if raw[:2].lower() == "0x"
                else (float(int(raw, 2)) if raw[:2].lower() == "0b" else float(raw))
            )
            j = m.end()
            unc = 0.0
            mm = _UNCRE.match(src, j)
            if mm:
                m2 = NUMRE.match(src, mm.end())
                if m2:
                    unc = float(m2.group(0).replace("_", ""))
                    j = m2.end()
            k = j
            while k < n and src[k] in " \t":
                k += 1
            disp = None
            r = None
            if k < n and src[k] != "\n":
                if src[k] == "1":
                    if src[k:].startswith("1/"):
                        r = scan_unit(src, k)
                elif src[k] != "(":
                    r = scan_unit(src, k)
                if r:
                    m0 = _URE.match(src, k)
                    if m0 and m0.group(0) in KEYWORDS:
                        r = None
            if r:
                s_, d_, e_ = r
                disp = (s_, src[k:e_])
                val = val * s_
                unc = unc * s_
                j = e_
            T("QTY", (val, unc, disp), line, sc, start, j)
            col += j - i
            i = j
            continue
        m = NAMERE.match(src, i)
        if m:
            w = m.group(0)
            T("KW" if w in KEYWORDS else "NAME", w, line, sc, start, m.end())
            col += m.end() - i
            i = m.end()
            continue
        if c == ";":
            T("NEWLINE", None, line, sc, i, i + 1)
            i += 1
            col += 1
            continue
        for op in OPS:
            if src.startswith(op, i):
                T("OP", op, line, sc, start, i + len(op))
                if op in "([{":
                    depth += 1
                elif op in ")]}":
                    depth = max(0, depth - 1)
                i += len(op)
                col += len(op)
                break
        else:
            raise ThrumError("unexpected character %r" % c, (line, sc, start, start + 1))
    if toks and toks[-1].kind != "NEWLINE":
        T("NEWLINE", None, line, col, n, n)
    while len(indents) > 1:
        indents.pop()
        T("DEDENT", 0, line, col, n, n)
    T("EOF", None, line, col, n, n)
    return toks
