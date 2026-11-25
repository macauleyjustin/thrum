"""CLI entrypoint, bundled self-tests, project test discovery, and enhanced REPL."""

import argparse
import glob
import io as _io
import json
import os
import sys

from .ast_nodes import dump_ast
from .checker import BUILTIN_NAMES, check_program, resolve_module_path
from .errors import ThrumError, VERSION, report
from .fmt import export_graph, extract_docs, format_source
from .graph import Cell
from .interp import Interp
from .caps import Journal
from .parser import parse
from .stdmods import STDMODS
from .units import UNITS, dimstr
from .values import vstr


SELFTESTS = [
    ("units literal", "with io:\n    io.say(5 ft as cm)\n", ["152.4 cm"]),
    ("dimension error", "let x = 1 m + 1 s\n", ["dimension mismatch"]),
    ("uncertainty", "with io:\n    io.say(2.0 ± 0.1 m + 3.0 ± 0.2 m)\n", ["5.00 ± 0.22 m"]),
    ("live relation", "var c = 20\nlet f = c * 9/5 + 32\nwith io:\n    io.say(f)\n    c = 100\n    io.say(f)\n", ["68", "212"]),
    ("glitch-free diamond", "var a = 1\nlet b = a + 1\nlet c = a * 2\nlet d = b + c\nwith io:\n    io.say(d)\n    a = 10\n    io.say(d)\n", ["4", "31"]),
    ("settle fixpoint", "var n = 2\nsettle max 60 tol 1e-14:\n    let root = (root + n/root)/2 init 1\nwith io:\n    io.say(round(root, 6))\n    n = 9\n    io.say(round(root, 6))\n", ["1.414214", "3"]),
    ("reaction edge", 'var t = 290 K\nwith io:\n    on t > 300 K rise:\n        io.say("hot")\n    t = 305 K\n    t = 295 K\n    t = 310 K\n', ["hot", "hot"]),
    ("purity enforced", "var x = 1\nlet y = x + 1\nfn bad(v: num) -> num:\n    x = 99\n    return v\nlet z = bad(x)\n", ["cannot mutate"]),
    ("dimension types", "fn ke(m: kg, v: m/s) -> J:\n    return 0.5 * m * v^2\nwith io:\n    io.say(ke(2 kg, 3 m/s))\n", ["9 J"]),
    ("contract", 'fn f(x: num) -> num:\n    requires x >= 0, "need non-negative"\n    return x\nwith io:\n    io.say(f(-1))\n', ["contract failed"]),
    ("whatif", "var m = 72 kg\nvar h = 1.8 m\nlet bmi = m / h^2\nwith io:\n    whatif m = 90 kg:\n        io.say(round(bmi, 2))\n    io.say(round(bmi, 2))\n", ["27.78", "22.22"]),
    ("comprehension + pipe", "let xs = [x^2 for x in 1..6 if x % 2 == 0]\nwith io:\n    io.say(xs |> map(\\it * 2) |> sum())\n", ["112"]),
    ("match", 'let s = {kind: "circle", r: 2}\nmatch s:\n    case {kind: "circle", r}:\n        with io:\n            io.say(round(3.14159 * r^2, 3))\n    case _:\n        with io:\n            io.say("no")\n', ["12.566"]),
    ("copy-on-write reactivity", "var xs = [1, 2, 3]\nlet total = sum(xs)\nwith io:\n    io.say(total)\n    xs[0] = 10\n    io.say(total)\n", ["6", "15"]),
    ("capability required", 'io.say("hi")\n', ["undefined name 'io'"]),
    ("codata uncertainty", "use const\nwith io:\n    io.say(const.G)\n", ["6.6743", "±"]),
]


def run_selftests():
    ok = 0
    for name, src, expects in SELFTESTS:
        buf = _io.StringIO()
        errout = ""
        try:
            I = Interp(out=buf)
            I.run(src, "<test>")
        except ThrumError as e:
            errout = e.msg
        except Exception as e:
            errout = "%s: %s" % (type(e).__name__, e)
        got = buf.getvalue() + errout
        if all(x in got for x in expects):
            print("  ok   %s" % name)
            ok += 1
        else:
            print("  FAIL %s\n       expected %s\n       got %r" % (name, expects, got))
    print("\n%d/%d passed" % (ok, len(SELFTESTS)))
    return 0 if ok == len(SELFTESTS) else 1


def run_project_tests(target):
    files = [target] if os.path.isfile(target) else sorted(glob.glob(os.path.join(target, "**", "*_test.thrum"), recursive=True))
    if not files:
        print("no *_test.thrum files found in %s" % target)
        return 1
    ok = 0
    for f in files:
        with open(f, encoding="utf-8") as fh:
            src = fh.read()
        I = Interp(path=f)
        try:
            I.run(src, f)
            print("  ok   %s" % f)
            ok += 1
        except ThrumError as e:
            print("  FAIL %s" % f)
            report(e, I.src, f)
    print("\n%d/%d test files passed" % (ok, len(files)))
    return 0 if ok == len(files) else 1


def repl():
    print("Thrum %s — live relations, quantities, provenance.  Type :help for commands, Ctrl-D to exit." % VERSION)
    I = Interp()
    while True:
        try:
            line = input(">>> ")
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        s = line.strip()
        if not s:
            continue
        if s.startswith(":"):
            parts = s.split()
            cmd = parts[0]
            if cmd in (":q", ":quit", ":exit"):
                return 0
            if cmd == ":help":
                print("  :vars         list current top-level bindings")
                print("  :why <name>   print dependency tree for <name>")
                print("  :budget <n>   print uncertainty budget for <n>")
                print("  :units [q]    search unit registry")
                print("  :reset        clear all state")
                print("  :quit         exit REPL")
                continue
            if cmd == ":reset":
                I = Interp()
                print("(state cleared)")
                continue
            if cmd == ":vars":
                for k in sorted(I.globals.vars):
                    v = I.globals.vars[k]
                    if isinstance(v, Cell):
                        print("  %s %s = %s" % ("var" if v.kind == "source" else "let", k, vstr(v.value, True)))
                    else:
                        print("  pin %s = %s" % (k, vstr(v, True)))
                continue
            if cmd == ":units":
                q = parts[1] if len(parts) > 1 else None
                for k in sorted(UNITS):
                    if q and q not in k:
                        continue
                    sc, d = UNITS[k]
                    print("  %-12s %-22s scale=%g" % (k, dimstr(d) or "1", sc))
                continue
            if cmd in (":why", ":budget") and len(parts) == 2:
                line = "%s %s" % (cmd[1:], parts[1])
            else:
                print("unknown command %r (try :help)" % cmd)
                continue
        if line.rstrip().endswith(":"):
            buf = [line]
            while True:
                try:
                    more = input("... ")
                except (EOFError, KeyboardInterrupt):
                    break
                if more.strip() == "":
                    break
                buf.append(more)
            line = "\n".join(buf)
        try:
            prog = parse(line)
            for st in prog:
                if st.kind == "exprstmt":
                    v = I.eval(st.value, I.globals)
                    if v is not None:
                        print(vstr(v, True))
                else:
                    I.exec(st, I.globals)
            I.src = line
            I.drain()
        except ThrumError as e:
            report(e, line, None)
        except RecursionError:
            print("error: recursion too deep")


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    prog_args = []
    if "--" in argv:
        idx = argv.index("--")
        prog_args = argv[idx + 1 :]
        argv = argv[:idx]

    ap = argparse.ArgumentParser(prog="thrum", description="Thrum %s language toolchain" % VERSION)
    sub = ap.add_subparsers(dest="cmd")

    r = sub.add_parser("run")
    r.add_argument("file")
    r.add_argument("--journal")
    r.add_argument("--replay")
    r.add_argument("--trace", action="store_true")

    c = sub.add_parser("check")
    c.add_argument("file")

    a = sub.add_parser("ast")
    a.add_argument("file")

    f = sub.add_parser("fmt")
    f.add_argument("file")
    f.add_argument("--write", "-w", action="store_true")
    f.add_argument("--check", action="store_true")

    d = sub.add_parser("doc")
    d.add_argument("target")

    g = sub.add_parser("graph")
    g.add_argument("file")
    g.add_argument("symbol", nargs="?")
    g.add_argument("--format", choices=("dot", "mermaid"), default="dot")

    u = sub.add_parser("units")
    u.add_argument("query", nargs="?")

    t = sub.add_parser("test")
    t.add_argument("target", nargs="?")

    sub.add_parser("repl")
    sub.add_parser("version")

    args = ap.parse_args(argv)
    cmd = args.cmd or "repl"

    if cmd == "version":
        print("Thrum " + VERSION)
        return 0
    if cmd == "test":
        if args.target:
            return run_project_tests(args.target)
        return run_selftests()
    if cmd == "repl":
        return repl()
    if cmd == "units":
        q = args.query
        for k in sorted(UNITS):
            if q and q not in k:
                continue
            sc, d_ = UNITS[k]
            print("%-12s %-22s scale=%g" % (k, dimstr(d_) or "1", sc))
        return 0
    if cmd == "doc":
        target = args.target
        if target.startswith("std."):
            class _N:
                modpath = target.split(".")
                path = None
                site = None
            target = resolve_module_path(_N(), None)
        try:
            print(extract_docs(target, STDMODS, Interp()), end="")
            return 0
        except (ThrumError, OSError) as e:
            sys.stderr.write("doc error: %s\n" % e)
            return 1

    with open(args.file, encoding="utf-8") as fh:
        src = fh.read()

    if cmd == "ast":
        try:
            for st in parse(src):
                dump_ast(st)
        except ThrumError as e:
            report(e, src, args.file)
            return 1
        return 0

    if cmd == "check":
        try:
            prog = parse(src)
            Interp()  # ensure BUILTIN_NAMES is populated
            check_program(prog, args.file, stdmods=STDMODS)
            print("%s: ok" % args.file)
            return 0
        except ThrumError as e:
            report(e, src, args.file)
            return 1

    if cmd == "fmt":
        try:
            formatted = format_source(src)
        except ThrumError as e:
            report(e, src, args.file)
            return 1
        if args.check:
            if formatted != src:
                sys.stderr.write("%s: would reformat\n" % args.file)
                return 1
            return 0
        if args.write:
            with open(args.file, "w", encoding="utf-8") as fh:
                fh.write(formatted)
            return 0
        sys.stdout.write(formatted)
        return 0

    if cmd == "graph":
        I = Interp(out=_io.StringIO(), path=args.file)
        try:
            I.run(src, args.file)
        except ThrumError as e:
            report(e, I.src, args.file)
            return 1
        sys.stdout.write(export_graph(I, args.symbol, fmt=args.format))
        return 0

    journal = Journal()
    if args.replay:
        with open(args.replay, encoding="utf-8") as fh:
            journal = Journal(json.load(fh), replay=True)
    I = Interp(journal=journal, trace=args.trace, path=args.file, prog_args=prog_args)
    try:
        I.run(src, args.file)
    except ThrumError as e:
        report(e, I.src, args.file)
        return 1
    except KeyboardInterrupt:
        sys.stderr.write("\ninterrupted\n")
        return 130
    if args.journal:
        with open(args.journal, "w", encoding="utf-8") as fh:
            json.dump(journal.events, fh, indent=1)
        sys.stderr.write("journaled %d events -> %s\n" % (len(journal.events), args.journal))
    if journal.replay and journal.divergences:
        sys.stderr.write("replay: %d output divergence(s)\n" % len(journal.divergences))
        return 2
    return 0
