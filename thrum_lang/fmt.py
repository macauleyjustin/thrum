"""Tooling utilities: source formatter (thrum fmt), doc extractor (thrum doc), graph export."""

from .graph import Cell
from .parser import parse
from .values import vstr


def format_source(src):
    """Normalize indentation (4 spaces), trailing whitespace, and final newline."""
    # Validate syntax first so malformed code is never mangled
    parse(src)
    lines = src.replace("\r\n", "\n").split("\n")
    # Remove trailing blank lines, preserve internal blank lines cleanly
    while lines and not lines[-1].strip():
        lines.pop()
    if not lines:
        return ""
    out = []
    # Re-normalize indentation depth in 4-space steps while preserving comments & unit runs verbatim
    indent_stack = [0]
    for raw in lines:
        stripped = raw.lstrip(" ")
        if not stripped:
            out.append("")
            continue
        if stripped.startswith("#"):
            # Align comment with current block level or own indent
            w = len(raw) - len(stripped)
            level = len(indent_stack) - 1
            for idx, iw in enumerate(indent_stack):
                if w <= iw:
                    level = idx
                    break
            out.append(("    " * level) + stripped.rstrip())
            continue
        w = len(raw) - len(stripped)
        if w > indent_stack[-1]:
            indent_stack.append(w)
        else:
            while w < indent_stack[-1]:
                indent_stack.pop()
        level = len(indent_stack) - 1
        out.append(("    " * level) + stripped.rstrip())
    return "\n".join(out) + "\n"


def extract_docs_from_source(src, title="<file>"):
    """Extract documentation entries for top-level records, functions, and bindings."""
    prog = parse(src)
    lines = src.replace("\r\n", "\n").split("\n")
    entries = []
    for st in prog:
        if st.kind not in ("fndecl", "record", "decl") or not st.site:
            continue
        ln = st.site[0] - 1  # 0-based
        doc_lines = []
        i = ln - 1
        while i >= 0 and lines[i].strip().startswith("#"):
            doc_lines.append(lines[i].strip().lstrip("#").strip())
            i -= 1
        doc_lines.reverse()
        doc = " ".join(doc_lines)
        if st.kind == "fndecl":
            params = ", ".join(
                p.name + ((": " + _ann_str(p.type)) if p.type else "") + (" = ..." if p.default else "")
                for p in st.params
            )
            ret = (" -> " + _ann_str(st.ret)) if st.ret else ""
            sig = "fn %s(%s)%s" % (st.name, params, ret)
        elif st.kind == "record":
            flds = ", ".join(f.name + ((": " + _ann_str(f.type)) if f.type else "") for f in st.fields)
            sig = "record %s { %s }" % (st.name, flds)
        else:
            sig = "%s %s = %s" % (st.kw, st.name, (st.src or "").strip())
        entries.append((sig, doc))
    out = [title]
    out.append("=" * len(title))
    for sig, doc in entries:
        out.append("  " + sig)
        if doc:
            out.append("      " + doc)
    return "\n".join(out) + "\n"


def _ann_str(ann):
    if ann is None:
        return ""
    if ann[0] == "kind":
        return ann[1]
    if ann[0] == "dim":
        return ann[2]
    if ann[0] == "record":
        return ann[2]
    return ""


def extract_docs(target, stdmods_factory, interp):
    """Return formatted docs for a built-in module or a .thrum file."""
    if target in stdmods_factory:
        mod = stdmods_factory[target](interp)
        lines = ["module " + target, "=" * (7 + len(target))]
        for k in sorted(mod.scope.vars):
            v = mod.scope.vars[k]
            lines.append("  %-18s %s" % (k, vstr(v)))
        return "\n".join(lines) + "\n"
    with open(target, encoding="utf-8") as fh:
        return extract_docs_from_source(fh.read(), title=target)


def export_graph(interp, target_name=None, fmt="dot"):
    """Export the reactive cell dependency graph as Graphviz DOT or Mermaid."""
    cells = []
    seen = set()

    def collect(c):
        if not isinstance(c, Cell) or c in seen or c.dead:
            return
        seen.add(c)
        cells.append(c)
        for d in c.deps:
            collect(d)

    if target_name:
        s = interp.globals.find(target_name)
        if s is None or not isinstance(s.vars[target_name], Cell):
            return ""
        collect(s.vars[target_name])
    else:
        for v in interp.globals.vars.values():
            if isinstance(v, Cell):
                collect(v)

    if fmt == "mermaid":
        lines = ["flowchart LR"]
        for c in cells:
            shape = '(["%s = %s"])' % (c.name, vstr(c.value).replace('"', "'")) if c.kind == "source" else '["%s = %s"]' % (c.name, vstr(c.value).replace('"', "'"))
            lines.append("    c%d%s" % (c.id, shape))
        for c in cells:
            for d in sorted(c.deps, key=lambda x: x.id):
                lines.append("    c%d --> c%d" % (d.id, c.id))
        return "\n".join(lines) + "\n"

    lines = ["digraph thrum {", '  rankdir=LR;']
    for c in cells:
        shape = "ellipse" if c.kind == "source" else "box"
        lbl = "%s\\n%s" % (c.name, vstr(c.value).replace('"', '\\"'))
        lines.append('  c%d [label="%s", shape=%s];' % (c.id, lbl, shape))
    for c in cells:
        for d in sorted(c.deps, key=lambda x: x.id):
            lines.append("  c%d -> c%d;" % (d.id, c.id))
    lines.append("}")
    return "\n".join(lines) + "\n"
