"""Object capabilities (io, clock, rand, fs, env, args) and replay journaling."""

from fractions import Fraction
import os
import random
import select
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

try:
    import termios
    import tty as _py_tty
except ImportError:
    termios = None
    _py_tty = None

from .errors import ThrumError
from .units import _d, dimlabel
from .values import Builtin, Cap, Q, mkq, vstr


def enc(v):
    if isinstance(v, Q):
        return {"$q": [v.v, [str(x) for x in v.d], v.u, list(v.disp) if v.disp else None]}
    if isinstance(v, list):
        return [enc(x) for x in v]
    if isinstance(v, dict):
        return {k: enc(x) for k, x in v.items()}
    return v


def dec(v):
    if isinstance(v, dict) and "$q" in v:
        a = v["$q"]
        return Q(a[0], tuple(Fraction(x) for x in a[1]), a[2], tuple(a[3]) if a[3] else None)
    if isinstance(v, list):
        return [dec(x) for x in v]
    if isinstance(v, dict):
        return {k: dec(x) for k, x in v.items()}
    return v


class Journal:
    def __init__(self, events=None, replay=False):
        self.events = events or []
        self.i = 0
        self.replay = replay
        self.divergences = []

    def get(self, kind, key, produce):
        if self.replay:
            if self.i >= len(self.events):
                raise ThrumError("replay journal exhausted at '%s'" % kind)
            e = self.events[self.i]
            self.i += 1
            if e["kind"] != kind:
                raise ThrumError("replay divergence: expected '%s', journal has '%s'" % (kind, e["kind"]))
            return dec(e["value"])
        v = produce()
        self.events.append({"kind": kind, "key": key, "value": enc(v)})
        return v


def _mkcap(name, table):
    return Cap(name, {k: Builtin(name + "." + k, f, ar) for k, (ar, f) in table.items()})


def cap_io(I, args, site):
    def say(I, a, s):
        txt = " ".join(vstr(x) for x in a)
        rec = I.journal.get("out", "", lambda: txt)
        if I.journal.replay and rec != txt:
            I.journal.divergences.append((I.journal.i, rec, txt))
        I.out.write(txt + "\n")
        return None

    def prt(I, a, s):
        txt = " ".join(vstr(x) for x in a)
        rec = I.journal.get("out", "", lambda: txt)
        if I.journal.replay and rec != txt:
            I.journal.divergences.append((I.journal.i, rec, txt))
        I.out.write(txt)
        return None

    def ask(I, a, s):
        prompt = vstr(a[0]) if a else ""
        return I.journal.get("io.ask", prompt, lambda: input(prompt))

    def warn(I, a, s):
        I.out.write("! " + " ".join(vstr(x) for x in a) + "\n")
        return None

    def err(I, a, s):
        import sys as _sys
        _sys.stderr.write(" ".join(vstr(x) for x in a) + "\n")
        return None

    def table(I, a, s):
        rows = a[0]
        if not isinstance(rows, list):
            raise ThrumError("io.table expects a list of rows", s)
        headers = a[1] if len(a) > 1 else None
        if headers is not None and not isinstance(headers, list):
            raise ThrumError("io.table headers must be a list", s)
        if not rows and not headers:
            return None
        if rows and isinstance(rows[0], dict):
            cols = headers if headers is not None else list(rows[0].keys())
            str_rows = [[vstr(r.get(c)) for c in cols] for r in rows]
        else:
            str_rows = [[vstr(c) for c in (r if isinstance(r, list) else [r])] for r in rows]
            cols = [vstr(h) for h in headers] if headers is not None else None
        all_rows = ([cols] if cols else []) + str_rows
        if not all_rows:
            return None
        ncols = max(len(r) for r in all_rows)
        widths = [0] * ncols
        for r in all_rows:
            for j, cell in enumerate(r):
                widths[j] = max(widths[j], len(cell))
        lines = []
        if cols:
            lines.append("  ".join(cols[j].ljust(widths[j]) if j < len(cols) else "" for j in range(ncols)).rstrip())
            lines.append("  ".join("-" * widths[j] for j in range(ncols)))
        for r in str_rows:
            lines.append("  ".join((r[j] if j < len(r) else "").ljust(widths[j]) for j in range(ncols)).rstrip())
        txt = "\n".join(lines)
        rec = I.journal.get("out", "", lambda: txt)
        if I.journal.replay and rec != txt:
            I.journal.divergences.append((I.journal.i, rec, txt))
        I.out.write(txt + "\n")
        return None

    return _mkcap(
        "io",
        {
            "say": (None, say),
            "print": (None, prt),
            "ask": ((0, 1), ask),
            "eof": ((0, 0), lambda I, a, s: False),
            "warn": (None, warn),
            "err": (None, err),
            "table": ((1, 2), table),
        },
    )


def cap_clock(I, args, site):
    def now(I, a, s):
        return mkq(I.journal.get("clock.now", "", time.time), _d(s=1), 0.0, (1.0, "s"))

    def mono(I, a, s):
        return mkq(I.journal.get("clock.mono", "", time.monotonic), _d(s=1), 0.0, (1.0, "s"))

    def sleep(I, a, s):
        sec = I.needq(a[0], s)
        if sec.d != _d(s=1):
            raise ThrumError("sleep expects a duration (s)", s)
        I.journal.get("clock.sleep", "", lambda: sec.v)
        if not I.journal.replay:
            time.sleep(max(0.0, sec.v))
        return None

    return _mkcap("clock", {"now": ((0,), now), "mono": ((0,), mono), "sleep": ((1,), sleep)})


def cap_rand(I, args, site):
    seed = I.needint(args[0], site) if args else None
    rng = random.Random(seed)

    def fl(I, a, s):
        return mkq(I.journal.get("rand.float", "", rng.random))

    def ri(I, a, s):
        lo, hi = I.needint(a[0], s), I.needint(a[1], s)
        return mkq(float(I.journal.get("rand.int", "%d,%d" % (lo, hi), lambda: rng.randint(lo, hi))))

    def pick(I, a, s):
        xs = a[0]
        if not isinstance(xs, list) or not xs:
            raise ThrumError("pick needs a non-empty list", s)
        i = I.journal.get("rand.pick", "", lambda: rng.randrange(len(xs)))
        return xs[i]

    def shuffle(I, a, s):
        xs = a[0]
        if not isinstance(xs, list):
            raise ThrumError("shuffle needs a list", s)
        order = I.journal.get("rand.shuffle", str(len(xs)), lambda: rng.sample(range(len(xs)), len(xs)))
        return [xs[idx] for idx in order]

    def sample(I, a, s):
        xs = a[0]
        if not isinstance(xs, list):
            raise ThrumError("sample needs a list", s)
        k = I.needint(a[1], s)
        if k < 0 or k > len(xs):
            raise ThrumError("sample count %d out of range (len %d)" % (k, len(xs)), s)
        order = I.journal.get("rand.sample", "%d,%d" % (len(xs), k), lambda: rng.sample(range(len(xs)), k))
        return [xs[idx] for idx in order]

    def gauss(I, a, s):
        mu = I.needq(a[0], s)
        sig = I.needq(a[1], s)
        if mu.d != sig.d:
            raise ThrumError("rand.gauss mean and stdev dimensions must match: %s vs %s" % (dimlabel(mu), dimlabel(sig)), s)
        val = I.journal.get("rand.gauss", "", lambda: rng.gauss(mu.v, sig.v))
        return mkq(val, mu.d, mu.u, mu.disp)

    return _mkcap(
        "rand",
        {
            "float": ((0,), fl),
            "int": ((2,), ri),
            "pick": ((1,), pick),
            "shuffle": ((1,), shuffle),
            "sample": ((2,), sample),
            "gauss": ((2,), gauss),
        },
    )


def cap_fs(I, args, site):
    if args and not isinstance(args[0], str):
        raise ThrumError("fs root path must be text", site)
    root = os.path.abspath(args[0] if args else ".")

    def resolve(p, s):
        if not isinstance(p, str):
            raise ThrumError("fs path must be text", s)
        q = os.path.abspath(os.path.join(root, p))
        prefix = os.sep if root == os.sep else root + os.sep
        if not (q == root or q.startswith(prefix)):
            raise ThrumError("path escapes the granted root '%s'" % root, s)
        return q

    def read(I, a, s):
        p = resolve(a[0], s)

        def _do():
            with open(p, encoding="utf-8") as fh:
                return fh.read()

        return I.journal.get("fs.read", p, _do)

    def read_lines(I, a, s):
        txt = read(I, a, s)
        return txt.splitlines()

    def write(I, a, s):
        p = resolve(a[0], s)
        if not isinstance(a[1], str):
            raise ThrumError("fs.write content must be text", s)

        def _do():
            parent = os.path.dirname(p)
            if parent and not os.path.exists(parent):
                os.makedirs(parent, exist_ok=True)
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(a[1])
            return a[1]

        rec = I.journal.get("fs.write", p, _do)
        if I.journal.replay and rec != a[1]:
            I.journal.divergences.append((I.journal.i, rec, a[1]))
        return None

    def append(I, a, s):
        p = resolve(a[0], s)
        if not isinstance(a[1], str):
            raise ThrumError("fs.append content must be text", s)

        def _do():
            parent = os.path.dirname(p)
            if parent and not os.path.exists(parent):
                os.makedirs(parent, exist_ok=True)
            with open(p, "a", encoding="utf-8") as fh:
                fh.write(a[1])
            return a[1]

        rec = I.journal.get("fs.append", p, _do)
        if I.journal.replay and rec != a[1]:
            I.journal.divergences.append((I.journal.i, rec, a[1]))
        return None

    def write_lines(I, a, s):
        if not isinstance(a[1], list):
            raise ThrumError("fs.write_lines needs a list", s)
        content = "\n".join(vstr(x) for x in a[1]) + ("\n" if a[1] else "")
        return write(I, [a[0], content], s)

    def exists(I, a, s):
        p = resolve(a[0], s)
        return I.journal.get("fs.exists", p, lambda: os.path.exists(p))

    def is_file(I, a, s):
        p = resolve(a[0], s)
        return I.journal.get("fs.is_file", p, lambda: os.path.isfile(p))

    def is_dir(I, a, s):
        p = resolve(a[0], s)
        return I.journal.get("fs.is_dir", p, lambda: os.path.isdir(p))

    def listing(I, a, s):
        p = resolve(a[0] if a else ".", s)
        return I.journal.get("fs.list", p, lambda: sorted(os.listdir(p)))

    def mkdir(I, a, s):
        p = resolve(a[0], s)

        def _do():
            os.makedirs(p, exist_ok=True)
            return True

        I.journal.get("fs.mkdir", p, _do)
        return None

    def remove(I, a, s):
        p = resolve(a[0], s)

        def _do():
            if os.path.isfile(p):
                os.remove(p)
            elif os.path.isdir(p):
                os.rmdir(p)
            else:
                raise ThrumError("cannot remove missing path '%s'" % a[0], s)
            return True

        I.journal.get("fs.remove", p, _do)
        return None

    return _mkcap(
        "fs",
        {
            "read": ((1,), read),
            "read_lines": ((1,), read_lines),
            "write": ((2,), write),
            "write_lines": ((2,), write_lines),
            "append": ((2,), append),
            "exists": ((1,), exists),
            "is_file": ((1,), is_file),
            "is_dir": ((1,), is_dir),
            "list": ((0, 1), listing),
            "mkdir": ((1,), mkdir),
            "remove": ((1,), remove),
        },
    )


def cap_env(I, args, site):
    def get(I, a, s):
        if not isinstance(a[0], str):
            raise ThrumError("env.get name must be text", s)
        default = a[1] if len(a) > 1 else ""
        return I.journal.get("env.get", a[0], lambda: os.environ.get(a[0], default))

    def has(I, a, s):
        if not isinstance(a[0], str):
            raise ThrumError("env.has name must be text", s)
        return I.journal.get("env.has", a[0], lambda: a[0] in os.environ)

    def keys(I, a, s):
        return I.journal.get("env.keys", "", lambda: sorted(os.environ.keys()))

    return _mkcap("env", {"get": ((1, 2), get), "has": ((1,), has), "keys": ((0,), keys)})


def cap_args(I, args, site):
    def all_args(I, a, s):
        return I.journal.get("args.all", "", lambda: list(I.prog_args))

    def get_arg(I, a, s):
        idx = I.needint(a[0], s)
        default = a[1] if len(a) > 1 else None
        xs = I.journal.get("args.all", "", lambda: list(I.prog_args))
        return xs[idx] if 0 <= idx < len(xs) else default

    def count_args(I, a, s):
        xs = I.journal.get("args.all", "", lambda: list(I.prog_args))
        return mkq(float(len(xs)))

    return _mkcap("args", {"all": ((0,), all_args), "get": ((1, 2), get_arg), "len": ((0,), count_args)})


def cap_tty(I, args, site):
    state = {"fd": None, "saved": None, "cursor_hidden": False}

    def _live_tty():
        return (
            sys.stdin is not None
            and hasattr(sys.stdin, "isatty")
            and sys.stdin.isatty()
            and hasattr(I.out, "isatty")
            and I.out.isatty()
        )

    def is_tty(I, a, s):
        return I.journal.get("tty.is_tty", "", _live_tty)

    def enter_raw(I, a, s):
        if I.journal.replay or not _live_tty() or termios is None or _py_tty is None:
            return False
        if state["saved"] is None:
            fd = sys.stdin.fileno()
            state["fd"] = fd
            state["saved"] = termios.tcgetattr(fd)
            _py_tty.setcbreak(fd)
        return True

    def exit_raw(I=None, a=None, s=None):
        if state["saved"] is not None and termios is not None:
            try:
                termios.tcsetattr(state["fd"], termios.TCSADRAIN, state["saved"])
            except Exception:
                pass
            state["saved"] = None
            state["fd"] = None
        return None

    def hide_cursor(I, a, s):
        if not I.journal.replay and _live_tty():
            I.out.write("\x1b[?25l")
            I.out.flush()
            state["cursor_hidden"] = True
        return None

    def show_cursor(I=None, a=None, s=None):
        if state["cursor_hidden"]:
            try:
                I.out.write("\x1b[?25h")
                I.out.flush()
            except Exception:
                pass
            state["cursor_hidden"] = False
        return None

    def clear(I, a, s):
        I.journal.get("tty.clear", "", lambda: True)
        I.out.write("\x1b[2J\x1b[H")
        if hasattr(I.out, "flush"):
            I.out.flush()
        return None

    def write(I, a, s):
        txt = "".join(vstr(x) for x in a)
        rec = I.journal.get("tty.write", "", lambda: txt)
        if I.journal.replay and rec != txt:
            I.journal.divergences.append((I.journal.i, rec, txt))
        I.out.write(txt)
        if hasattr(I.out, "flush"):
            I.out.flush()
        return None

    def size(I, a, s):
        def _sz():
            sz = shutil.get_terminal_size((80, 24))
            return {"cols": float(sz.columns), "rows": float(sz.lines)}

        raw = I.journal.get("tty.size", "", _sz)
        return {"cols": mkq(raw["cols"]), "rows": mkq(raw["rows"])}

    def _read_key(timeout_sec):
        if not _live_tty():
            return None
        fd = sys.stdin.fileno()
        r, _, _ = select.select([fd], [], [], max(0.0, timeout_sec))
        if not r:
            return None
        ch = os.read(fd, 1)
        if not ch:
            return None
        if ch == b"\x1b":
            r2, _, _ = select.select([fd], [], [], 0.015)
            if not r2:
                return "esc"
            b2 = os.read(fd, 1)
            if b2 == b"[":
                r3, _, _ = select.select([fd], [], [], 0.015)
                if not r3:
                    return "esc"
                b3 = os.read(fd, 1)
                return {b"A": "up", b"B": "down", b"C": "right", b"D": "left"}.get(b3, "esc")
            return "esc"
        if ch == b" ":
            return "space"
        if ch in (b"\r", b"\n"):
            return "enter"
        if ch in (b"\x7f", b"\x08"):
            return "backspace"
        if ch == b"\t":
            return "tab"
        try:
            return ch.decode("utf-8", errors="replace")
        except Exception:
            return None

    def poll_key(I, a, s):
        timeout_sec = 0.05
        if a:
            sec = I.needq(a[0], s)
            if sec.d != _d(s=1):
                raise ThrumError("tty.poll_key timeout expects a duration (s)", s)
            timeout_sec = sec.v
        return I.journal.get("tty.poll_key", "", lambda: _read_key(timeout_sec))

    def cleanup():
        exit_raw()
        show_cursor()

    methods = {
        "is_tty": ((0,), is_tty),
        "enter_raw": ((0,), enter_raw),
        "exit_raw": ((0,), exit_raw),
        "hide_cursor": ((0,), hide_cursor),
        "show_cursor": ((0,), show_cursor),
        "clear": ((0,), clear),
        "write": (None, write),
        "size": ((0,), size),
        "poll_key": ((0, 1), poll_key),
    }
    return Cap(
        "tty",
        {k: Builtin("tty." + k, f, ar) for k, (ar, f) in methods.items()},
        cleanup=cleanup,
    )


def cap_proc(I, args, site):
    def run_proc(I, a, s):
        cmd = a[0]
        if not isinstance(cmd, list) or not cmd or not all(isinstance(x, str) for x in cmd):
            raise ThrumError("proc.run expects a non-empty list of text arguments", s)
        cwd = a[1] if len(a) > 1 else None
        if cwd is not None and not isinstance(cwd, str):
            raise ThrumError("proc.run cwd must be text", s)

        def _do():
            p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
            return {
                "code": float(p.returncode),
                "stdout": p.stdout,
                "stderr": p.stderr,
            }

        raw = I.journal.get("proc.run", " ".join(cmd), _do)
        return {
            "code": mkq(raw["code"]),
            "stdout": raw["stdout"],
            "stderr": raw["stderr"],
        }

    def exe_path(I, a, s):
        return os.path.abspath(sys.argv[0])

    def cwd_path(I, a, s):
        return os.getcwd()

    def exit_proc(I, a, s):
        code = int(I.needint(a[0], s)) if a else 0
        sys.exit(code)

    return _mkcap("proc", {"run": ((1, 2), run_proc), "exe": ((0,), exe_path), "cwd": ((0,), cwd_path), "exit": ((0, 1), exit_proc)})


def cap_gui(I, a, s):
    raise ThrumError("gui capability requires compiled native binary (./thrumc build ...)", s)


def cap_net(I, a, s):
    def _do_req(method, url, body=None, headers=None, call_site=None):
        if not isinstance(url, str) or not (url.startswith("http://") or url.startswith("https://")):
            return {
                "ok": False,
                "status": mkq(0),
                "url": url if isinstance(url, str) else "",
                "content_type": "",
                "headers": {},
                "body": "",
                "error": "only http:// and https:// URLs are supported",
            }
        req_hdrs = {
            "User-Agent": "Mozilla/5.0 (Macintosh; Apple Silicon Mac OS X) ThrumBrowser/0.2",
            "Accept": "text/html,application/xhtml+xml,text/plain,application/json;q=0.9,*/*;q=0.8",
        }
        if isinstance(headers, dict):
            for k, v in headers.items():
                if isinstance(k, str) and isinstance(v, str):
                    req_hdrs[k] = v
        data = body.encode("utf-8") if isinstance(body, str) else None
        req = urllib.request.Request(url, data=data, headers=req_hdrs, method=method)

        def _do():
            try:
                with urllib.request.urlopen(req, timeout=12.0) as resp:
                    raw = resp.read()
                    try:
                        text = raw.decode("utf-8")
                    except UnicodeDecodeError:
                        text = raw.decode("latin-1", "replace")
                    hdrs = {str(k).lower(): str(v) for k, v in resp.headers.items()}
                    return {
                        "ok": 200 <= resp.status < 400,
                        "status": int(resp.status),
                        "url": resp.geturl(),
                        "content_type": resp.headers.get_content_type(),
                        "headers": hdrs,
                        "body": text,
                        "error": "",
                    }
            except urllib.error.HTTPError as he:
                raw = he.read()
                text = raw.decode("utf-8", "replace")
                hdrs = {str(k).lower(): str(v) for k, v in he.headers.items()} if he.headers else {}
                return {
                    "ok": False,
                    "status": int(he.code),
                    "url": he.geturl() or url,
                    "content_type": he.headers.get_content_type() if he.headers else "",
                    "headers": hdrs,
                    "body": text,
                    "error": str(he),
                }
            except Exception as ex:
                return {
                    "ok": False,
                    "status": 0,
                    "url": url,
                    "content_type": "",
                    "headers": {},
                    "body": "",
                    "error": str(ex),
                }

        raw = I.journal.get("net." + method.lower(), url, _do)
        return {
            "ok": raw["ok"],
            "status": mkq(int(raw["status"])),
            "url": raw["url"],
            "content_type": raw["content_type"],
            "headers": raw["headers"],
            "body": raw["body"],
            "error": raw["error"],
        }

    def get_req(I, a, s):
        if not isinstance(a[0], str):
            raise ThrumError("net.get needs text url", s)
        return _do_req("GET", a[0], None, a[1] if len(a) > 1 else None, s)

    def post_req(I, a, s):
        if not isinstance(a[0], str) or not isinstance(a[1], str):
            raise ThrumError("net.post needs text url and body", s)
        return _do_req("POST", a[0], a[1], a[2] if len(a) > 2 else None, s)

    return _mkcap("net", {"get": ((1, 2), get_req), "post": ((2, 3), post_req)})


CAPS = {
    "io": cap_io,
    "clock": cap_clock,
    "rand": cap_rand,
    "fs": cap_fs,
    "gui": cap_gui,
    "net": cap_net,
    "env": cap_env,
    "args": cap_args,
    "tty": cap_tty,
    "proc": cap_proc,
}
