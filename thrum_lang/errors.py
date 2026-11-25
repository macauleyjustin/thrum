"""Errors, control-flow signals, and source diagnostic reporting."""

import sys

VERSION = "0.2.0"


class ThrumError(Exception):
    def __init__(self, msg, site=None, hint=None):
        super().__init__(msg)
        self.msg = msg
        self.site = site
        self.hint = hint


class ReturnSig(Exception):
    def __init__(self, v):
        self.value = v


class BreakSig(Exception):
    pass


class ContinueSig(Exception):
    pass


def report(e, src, path, out=None):
    err_out = out or sys.stderr
    where = path or "<input>"
    if e.site:
        where += ":%d:%d" % (e.site[0], e.site[1])
    err_out.write("%s: error: %s\n" % (where, e.msg))
    if e.site and src:
        lines = src.split("\n")
        if 1 <= e.site[0] <= len(lines):
            ln = lines[e.site[0] - 1]
            err_out.write("  %5d | %s\n" % (e.site[0], ln))
            err_out.write("        | %s^\n" % (" " * max(0, e.site[1] - 1)))
    if e.hint:
        err_out.write("  hint: %s\n" % e.hint)
