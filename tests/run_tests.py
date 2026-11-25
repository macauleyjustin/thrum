#!/usr/bin/env python3
"""Thrum conformance test runner.
Executes every case in tests/cases/ against thrum.py via subprocess and verifies
exact stdout, stderr, exit codes, and journal/replay determinism.
"""
import os
import sys
import json
import tempfile
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
THRUM = os.path.join(ROOT, "thrum.py")
CASES_DIR = os.path.join(ROOT, "tests", "cases")


def norm(s):
    return s.replace("\r\n", "\n")


def run_case(thrum_file):
    base = thrum_file[:-6]  # strip .thrum
    name = os.path.basename(base)
    exp_out_path = base + ".expected"
    exp_err_path = base + ".err"
    cmd_path = base + ".cmd"

    mode = "run"
    extra = []
    if os.path.exists(cmd_path):
        with open(cmd_path, encoding="utf-8") as fh:
            parts = fh.read().strip().split()
        mode = parts[0]
        extra = parts[1:]

    if mode == "replay_ok":
        with tempfile.TemporaryDirectory() as tmp:
            jpath = os.path.join(tmp, "journal.json")
            p1 = subprocess.run(
                [sys.executable, THRUM, "run", thrum_file, "--journal", jpath] + extra,
                capture_output=True, text=True, cwd=CASES_DIR,
            )
            if p1.returncode != 0:
                return False, "journal run failed (%d): %s" % (p1.returncode, p1.stderr)
            p2 = subprocess.run(
                [sys.executable, THRUM, "run", thrum_file, "--replay", jpath] + extra,
                capture_output=True, text=True, cwd=CASES_DIR,
            )
            if p2.returncode != 0:
                return False, "replay run failed (%d): %s" % (p2.returncode, p2.stderr)
            if p1.stdout != p2.stdout:
                return False, "replay stdout mismatch:\n--- run1 ---\n%r\n--- replay ---\n%r" % (p1.stdout, p2.stdout)
            if os.path.exists(exp_out_path):
                with open(exp_out_path, encoding="utf-8") as fh:
                    exp = norm(fh.read())
                if norm(p2.stdout) != exp:
                    return False, "stdout mismatch:\nexpected: %r\ngot:      %r" % (exp, norm(p2.stdout))
            return True, ""

    if mode == "replay_diverge":
        other = os.path.join(CASES_DIR, extra[0])
        with tempfile.TemporaryDirectory() as tmp:
            jpath = os.path.join(tmp, "journal.json")
            p1 = subprocess.run(
                [sys.executable, THRUM, "run", thrum_file, "--journal", jpath],
                capture_output=True, text=True, cwd=CASES_DIR,
            )
            if p1.returncode != 0:
                return False, "initial journal run failed: %s" % p1.stderr
            p2 = subprocess.run(
                [sys.executable, THRUM, "run", other, "--replay", jpath],
                capture_output=True, text=True, cwd=CASES_DIR,
            )
            if p2.returncode != 2:
                return False, "expected exit code 2 on replay divergence, got %d (stderr=%r)" % (p2.returncode, p2.stderr)
            with open(exp_err_path, encoding="utf-8") as fh:
                exp_err = norm(fh.read())
            if norm(p2.stderr) != exp_err:
                return False, "stderr mismatch:\nexpected: %r\ngot:      %r" % (exp_err, norm(p2.stderr))
            return True, ""

    if mode == "fs_sandbox":
        with tempfile.TemporaryDirectory() as tmp:
            tmp = os.path.realpath(tmp)
            with open(os.path.join(tmp, "hello.txt"), "w", encoding="utf-8") as fh:
                fh.write("secret-content")
            # Copy test into temp parent so relative ./sandbox works cleanly
            sub = os.path.join(tmp, "sandbox")
            os.makedirs(sub)
            with open(os.path.join(sub, "a.txt"), "w", encoding="utf-8") as fh:
                fh.write("inside")
            with open(thrum_file, encoding="utf-8") as fh:
                src = fh.read()
            prog = os.path.join(tmp, os.path.basename(thrum_file))
            with open(prog, "w", encoding="utf-8") as fh:
                fh.write(src)
            p = subprocess.run(
                [sys.executable, THRUM, "run", prog],
                capture_output=True, text=True, cwd=tmp,
            )
            if os.path.exists(exp_err_path):
                with open(exp_err_path, encoding="utf-8") as fh:
                    exp_err = norm(fh.read())
                got_err = norm(p.stderr).replace(prog, os.path.basename(thrum_file)).replace(sub, "<SANDBOX>")
                if p.returncode == 0 or got_err != exp_err:
                    return False, "expected error:\n%s\ngot rc=%d stderr:\n%s" % (exp_err, p.returncode, got_err)
                return True, ""
            with open(exp_out_path, encoding="utf-8") as fh:
                exp = norm(fh.read())
            if p.returncode != 0 or norm(p.stdout) != exp:
                return False, "rc=%d expected=%r got=%r stderr=%r" % (p.returncode, exp, norm(p.stdout), p.stderr)
            return True, ""

    if mode == "doc":
        argv = [sys.executable, THRUM, "doc"] + extra
    else:
        argv = [sys.executable, THRUM, mode, os.path.basename(thrum_file)] + extra
    p = subprocess.run(argv, capture_output=True, text=True, cwd=CASES_DIR)

    if os.path.exists(exp_err_path):
        with open(exp_err_path, encoding="utf-8") as fh:
            exp_err = norm(fh.read())
        if p.returncode == 0:
            return False, "expected non-zero exit code, got 0 with stdout=%r" % p.stdout
        got_err = norm(p.stderr)
        if got_err != exp_err:
            return False, "stderr mismatch:\nexpected:\n%s\ngot:\n%s" % (exp_err, got_err)
        return True, ""

    if not os.path.exists(exp_out_path):
        return False, "missing .expected or .err file for %s" % name

    with open(exp_out_path, encoding="utf-8") as fh:
        exp_out = norm(fh.read())
    if p.returncode != 0:
        return False, "unexpected exit code %d; stderr:\n%s" % (p.returncode, p.stderr)
    if p.stderr:
        return False, "unexpected stderr on success:\n%s" % p.stderr
    got_out = norm(p.stdout)
    if got_out != exp_out:
        return False, "stdout mismatch:\nexpected:\n%r\ngot:\n%r" % (exp_out, got_out)
    return True, ""


def main():
    files = sorted(
        os.path.join(CASES_DIR, f)
        for f in os.listdir(CASES_DIR)
        if f.endswith(".thrum") and not f.startswith("_helper_")
    )
    if not files:
        print("No test cases found in", CASES_DIR)
        return 1
    passed = 0
    failed = 0
    for f in files:
        name = os.path.basename(f)[:-6]
        ok, msg = run_case(f)
        if ok:
            print("  ok   %s" % name)
            passed += 1
        else:
            print("  FAIL %s\n       %s" % (name, msg.replace("\n", "\n       ")))
            failed += 1
    print("\n%d/%d conformance tests passed" % (passed, passed + failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
