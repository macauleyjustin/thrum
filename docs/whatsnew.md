[Documentation Index](index.md) | [Next: Tutorial &rarr;](tutorial.md)

# What's New In Thrum 0.2

This document explains the major additions in **Thrum 0.2** compared to the initial Thrum 0.1 prototype.

---

## Summary — Release Highlights

Thrum 0.2 transforms Thrum from an interpreted research language into a **self-hosted, natively compiled language** capable of producing standalone command-line tools, interactive terminal apps, and native macOS GUI software:

1. **Self-Hosted Compiler (`thrumc`):** Complete compiler written in pure Thrum under [`selfhost/`](../selfhost/thrumc.thrum), generating portable C11 and verified to a byte-identical 3-stage fixed point.
2. **Zero-Dependency C11 Native Runtime (`runtime/`):** Full mark-sweep garbage collector, topological reactive DAG solver, SI dimensional & Gaussian uncertainty arithmetic, UTF-8 text engine, MT19937 deterministic PRNG, replay journaling, and native Cocoa/Foundation backends.
3. **Native macOS Desktop GUI (`gui`) & Explicit HTTPS (`net`):** Explicit `with gui:` Cocoa/AppKit 2D canvas (shapes, clipping, font metrics, scroll wheel, clipboard) and `with net:` HTTPS client (`NSURLSession`), showcased by a full desktop web browser (`examples/browser.thrum` / `ThrumBrowser.app`) and Klondike Solitaire (`examples/solitaire.thrum` / `ThrumSolitaire.app`).
4. **Records, Pattern Matching & Contracts:** Immutable product `record`s with dimensional field verification, copy-on-write field mutation, `match` destructuring, and `requires`/`ensures` function contracts.
5. **18 Standard Modules:** 13 built-in modules (`math`, `const`, `stat`, `units`, `text`, `regex`, `json`, `csv`, `time`, `path`, `list`, `map`, `test`) plus 5 pure-Thrum modules under [`stdlib/`](../stdlib/physics.thrum) (`std.vec`, `std.physics`, `std.signal`, `std.finance`, `std.matrix`).

---

## New Language Features

### Typed Immutable `record`s & `match` Destructuring

Records provide nominal, dimension-checked data structures with optional default values and copy-on-write field updates:

```thrum
record Pt:
    x: m
    y: m = 0 m

var p = Pt(3 m)
let dist = sqrt(p.x^2 + p.y^2)
p.y = 4 m   # triggers Copy-on-Write rebuild and updates `dist` to 5 m
```

See [Records](reference.md#34-records) for full details.

### Enhanced Text & Raw Strings

- Raw text literals `r"..."`, `r'...'`, and `r"""..."""` bypass escape sequences and `{expr}` interpolation cleanly.
- Triple-quoted text supports multiline strings while preserving exact UTF-8 codepoint indexing (`len`, indexing, slicing, and regex offsets).

### `thrumc` Unified Native CLI

`thrumc` compiles programs natively (`build`, `run`, `emit-c`) and ships built-in developer workflows (`check`, `fmt`, `doc`, `units`, `test`, `version`, and `--strict` build checking). See [Command Line Reference](cli.md).

---

## New Capabilities: `gui` &amp; `net`

Thrum 0.2 adds `gui` and `net` to the object-capability set (`io`, `clock`, `rand`, `fs`, `env`, `args`, `tty`, `proc`, `gui`, `net`). Native programs using `with gui:` or `with net:` link macOS `Cocoa.framework` (`runtime/rt_gui_cocoa.m`) and `Foundation.framework` (`runtime/rt_net_apple.m`); CLI programs that omit them link only the lightweight stubs (`rt_gui_stub.o`, `rt_net_stub.o`) with zero AppKit/Foundation overhead.

An offscreen headless rendering mode (`THRUM_GUI_HEADLESS=1`) allows automated tests and CI pipelines to exercise frame rendering and `gui.screenshot()` PNG generation without an active desktop focus window.

---

[Documentation Index](index.md) | [Next: Tutorial &rarr;](tutorial.md)
