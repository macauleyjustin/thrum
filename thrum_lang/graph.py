"""Reactive dependency graph: Cell, settle Group, Reaction, and Scope."""


class Cell:
    _n = 0

    def __init__(self, name, kind, value, expr=None, env=None, site=None, src=None, group=None, has_init=False):
        self.id = Cell._n
        Cell._n += 1
        self.name, self.kind, self.value = name, kind, value
        self.expr, self.env, self.site, self.src = expr, env, site, src
        self.deps, self.subs, self.newdeps = set(), set(), set()
        self.height = 0
        self.group = group
        self.version = 0
        self.eval_id = 0
        self.dead = False
        self.has_init = has_init
        self.born = self.id


class Group:
    def __init__(self, maxit, tol, site):
        self.maxit, self.tol, self.site, self.cells = maxit, tol, site, []

    @property
    def height(self):
        return max((c.height for c in self.cells), default=0)


class Reaction:
    def __init__(self, guard, body, env, mode):
        self.guard, self.body, self.env, self.mode = guard, body, env, mode
        self.prev = False
        self.seen = -1


class Scope:
    __slots__ = ("vars", "parent", "rel", "const_names")

    def __init__(self, parent=None, rel=False):
        self.vars, self.parent, self.rel = {}, parent, rel
        self.const_names = set()

    def find(self, name):
        s = self
        while s is not None:
            if name in s.vars:
                return s
            s = s.parent
        return None
