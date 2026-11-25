"""Thrum programming language package."""

from .errors import VERSION, ThrumError
from .units import UNITS, DIMNAME, parse_unit, dimstr
from .values import Q, RecordType, RecordVal, Fn, Builtin, Module, Cap, vstr, veq, qfmt
from .lexer import lex
from .parser import parse
from .checker import check_program
from .interp import Interp
from .caps import Journal
from .fmt import format_source, extract_docs, export_graph

__version__ = VERSION
__all__ = [
    "VERSION",
    "ThrumError",
    "UNITS",
    "DIMNAME",
    "parse_unit",
    "dimstr",
    "Q",
    "RecordType",
    "RecordVal",
    "Fn",
    "Builtin",
    "Module",
    "Cap",
    "vstr",
    "veq",
    "qfmt",
    "lex",
    "parse",
    "check_program",
    "Interp",
    "Journal",
    "format_source",
    "extract_docs",
    "export_graph",
]
