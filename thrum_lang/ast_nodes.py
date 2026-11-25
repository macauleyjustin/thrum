"""AST node representation and AST dumper."""


class Node:
    def __init__(self, kind, site=None, **kw):
        self.kind = kind
        self.site = site
        self.__dict__.update(kw)

    def __repr__(self):
        return "Node(%s)" % self.kind


def site_of(t):
    return (t.line, t.col, t.start, t.end)


def dump_ast(n, ind=0, out=None):
    import sys

    w = (out or sys.stdout).write
    if isinstance(n, Node):
        w(" " * ind + n.kind + "\n")
        for k, v in n.__dict__.items():
            if k in ("kind", "site"):
                continue
            w(" " * (ind + 2) + k + ":\n")
            dump_ast(v, ind + 4, out=out)
    elif isinstance(n, list):
        for x in n:
            dump_ast(x, ind, out=out)
    elif isinstance(n, tuple):
        for x in n:
            dump_ast(x, ind, out=out)
    else:
        w(" " * ind + repr(n)[:120] + "\n")
