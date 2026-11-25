#!/usr/bin/env python3
"""Thrum — reference interpreter and CLI toolchain. Pure Python 3.9+, stdlib only.
Usage: python thrum.py run prog.thrum | repl | check prog.thrum | fmt | doc | graph | ast | test | units
"""
import sys

from thrum_lang import *  # noqa: F401, F403
from thrum_lang.cli import main

if __name__ == "__main__":
    sys.setrecursionlimit(20000)
    sys.exit(main())
