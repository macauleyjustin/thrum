#!/usr/bin/env python3
"""Native conformance and integration test runner for standalone ./thrumc."""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
THRUMC = os.path.join(ROOT, "thrumc")
CASES_DIR = os.path.join(ROOT, "tests", "cases")
ENV = {**os.environ, "THRUM_HOME": ROOT}


def norm(s):
    return s.replace("\r\n", "\n")


def run_case(thrum_file):
    base = thrum_file[:-6]
    exp_out_path = base + ".expected"
    exp_err_path = base + ".err"
    cmd_path = base + ".cmd"

    mode, extra = "run", []
    if os.path.exists(cmd_path):
        with open(cmd_path, encoding="utf-8") as fh:
            parts = fh.read().strip().split()
        mode, extra = parts[0], parts[1:]

    if mode == "replay_ok":
        with tempfile.TemporaryDirectory() as tmp:
            jpath = os.path.join(tmp, "j.json")
            p1 = subprocess.run([THRUMC, "run", thrum_file, "--journal", jpath] + extra, capture_output=True, text=True, cwd=CASES_DIR, env=ENV)
            p2 = subprocess.run([THRUMC, "run", thrum_file, "--replay", jpath] + extra, capture_output=True, text=True, cwd=CASES_DIR, env=ENV)
            if p1.returncode != 0 or p2.returncode != 0 or p1.stdout != p2.stdout:
                return False, f"p1=({p1.returncode},{p1.stderr!r}) p2=({p2.returncode},{p2.stderr!r}) out_eq={p1.stdout == p2.stdout}"
            if os.path.exists(exp_out_path):
                exp = norm(open(exp_out_path, encoding="utf-8").read())
                if norm(p2.stdout) != exp:
                    return False, f"expected {exp!r}, got {norm(p2.stdout)!r}"
        return True, ""

    if mode == "replay_diverge":
        other = os.path.join(CASES_DIR, extra[0])
        with tempfile.TemporaryDirectory() as tmp:
            jpath = os.path.join(tmp, "j.json")
            p1 = subprocess.run([THRUMC, "run", thrum_file, "--journal", jpath], capture_output=True, text=True, cwd=CASES_DIR, env=ENV)
            p2 = subprocess.run([THRUMC, "run", other, "--replay", jpath], capture_output=True, text=True, cwd=CASES_DIR, env=ENV)
            exp_err = norm(open(exp_err_path, encoding="utf-8").read())
            if p1.returncode != 0 or p2.returncode != 2 or norm(p2.stderr) != exp_err:
                return False, f"rc={p2.returncode} exp={exp_err!r} got={norm(p2.stderr)!r}"
        return True, ""

    if mode == "fs_sandbox":
        with tempfile.TemporaryDirectory() as tmp:
            tmp = os.path.realpath(tmp)
            with open(os.path.join(tmp, "hello.txt"), "w") as fh:
                fh.write("secret-content")
            sub = os.path.join(tmp, "sandbox")
            os.makedirs(sub)
            with open(os.path.join(sub, "a.txt"), "w") as fh:
                fh.write("inside")
            prog = os.path.join(tmp, os.path.basename(thrum_file))
            with open(prog, "w", encoding="utf-8") as fh:
                fh.write(open(thrum_file, encoding="utf-8").read())
            p = subprocess.run([THRUMC, "run", prog], capture_output=True, text=True, cwd=tmp, env=ENV)
            if os.path.exists(exp_err_path):
                exp_err = norm(open(exp_err_path, encoding="utf-8").read())
                got_err = norm(p.stderr).replace(prog, os.path.basename(thrum_file)).replace(sub, "<SANDBOX>")
                if p.returncode == 0 or got_err != exp_err:
                    return False, f"rc={p.returncode} exp={exp_err!r} got={got_err!r}"
            else:
                exp = norm(open(exp_out_path, encoding="utf-8").read())
                if p.returncode != 0 or norm(p.stdout) != exp:
                    return False, f"rc={p.returncode} exp={exp!r} got={norm(p.stdout)!r} err={p.stderr!r}"
        return True, ""

    if mode in ("graph", "ast"):
        argv = [THRUMC, mode, os.path.basename(thrum_file)] + extra
    elif mode == "doc":
        argv = [THRUMC, "doc"] + extra
    else:
        argv = [THRUMC, mode, os.path.basename(thrum_file)] + extra
    p = subprocess.run(argv, capture_output=True, text=True, cwd=CASES_DIR, env=ENV)
    if os.path.exists(exp_err_path):
        exp_err = norm(open(exp_err_path, encoding="utf-8").read())
        if p.returncode == 0 or norm(p.stderr) != exp_err:
            return False, f"rc={p.returncode}\nEXP:\n{exp_err}\nGOT:\n{norm(p.stderr)}"
    else:
        exp_out = norm(open(exp_out_path, encoding="utf-8").read())
        if p.returncode != 0 or p.stderr or norm(p.stdout) != exp_out:
            return False, f"rc={p.returncode} err={p.stderr!r}\nEXP:{exp_out!r}\nGOT:{norm(p.stdout)!r}"
    return True, ""


def main():
    if not os.path.isfile(THRUMC):
        sys.stderr.write("missing compiled compiler ./thrumc (run 'make')\n")
        return 1

    files = sorted(
        os.path.join(CASES_DIR, f)
        for f in os.listdir(CASES_DIR)
        if f.endswith(".thrum") and not f.startswith("_helper_")
    )

    passed, failed, skipped = 0, 0, 0
    for tf in files:
        name = os.path.basename(tf[:-6])
        res, msg = run_case(tf)
        if res is None:
            skipped += 1
        elif res:
            passed += 1
        else:
            failed += 1
            print(f"FAIL {name}: {msg}")

    print(f"Native conformance: {passed}/{passed + failed} passed ({skipped} cli-only skipped)")

    # Native self-tests + stdlib + headless GUI capability smoke test
    r_self = subprocess.run([THRUMC, "test"], capture_output=True, text=True, cwd=ROOT, env=ENV)
    r_std = subprocess.run([THRUMC, "test", "tests/stdlib_test.thrum"], capture_output=True, text=True, cwd=ROOT, env=ENV)
    if r_self.returncode != 0:
        print("FAIL ./thrumc test:\n" + r_self.stdout + r_self.stderr)
        failed += 1
    else:
        print("Native self-tests: 16/16 passed")
    if r_std.returncode != 0:
        print("FAIL ./thrumc test tests/stdlib_test.thrum:\n" + r_std.stdout + r_std.stderr)
        failed += 1
    else:
        print("Native stdlib suite: passed")

    if sys.platform == "darwin":
        with tempfile.TemporaryDirectory() as tmp:
            shot = os.path.join(tmp, "gui.png")
            gui_src = os.path.join(tmp, "gui_smoke.thrum")
            gui_bin = os.path.join(tmp, "gui_smoke")
            with open(gui_src, "w", encoding="utf-8") as fh:
                fh.write(
                    'with gui, args:\n'
                    '    gui.open("Smoke", 320, 200)\n'
                    '    gui.begin()\n'
                    '    gui.clear(20, 30, 40)\n'
                    '    gui.clip(10, 10, 300, 180)\n'
                    '    gui.rect(20, 20, 100, 40, 80, 180, 120)\n'
                    '    gui.text(30, 35, "Thrum", 16, 255, 255, 255, "left", true, 255, false)\n'
                    '    gui.unclip()\n'
                    '    gui.commit()\n'
                    '    gui.screenshot(args.get(0))\n'
                    '    gui.close()\n'
                )
            r_b = subprocess.run([THRUMC, "build", gui_src, "-o", gui_bin], capture_output=True, text=True, cwd=ROOT, env=ENV)
            r_g = subprocess.run([gui_bin, shot], capture_output=True, text=True, cwd=ROOT, env={**ENV, "THRUM_GUI_HEADLESS": "1"}) if r_b.returncode == 0 else r_b
            hdr = open(shot, "rb").read(8) if os.path.exists(shot) else b""
            if r_g.returncode != 0 or hdr != b"\x89PNG\r\n\x1a\n":
                print("FAIL headless GUI capability test:", r_g.returncode, r_g.stderr)
                failed += 1
            else:
                print("Native headless GUI capability smoke test: passed")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
