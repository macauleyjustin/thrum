[Documentation Index](index.md) | [Next: Tutorial &rarr;](tutorial.md)

# What's New In Thrum 0.2

This document explains the major additions in **Thrum 0.2** compared to the initial Thrum 0.1 prototype.

---

## Summary — Release Highlights

Thrum 0.2 transforms Thrum from an interpreted research language into a **self-hosted, natively compiled language** capable of producing standalone command-line tools, interactive terminal apps, and native macOS GUI software:

1. **Self-Hosted Compiler (`thrumc`):** Complete compiler written in pure Thrum under [`selfhost/`](../selfhost/thrumc.thrum), generating portable C11 and verified to a byte-identical 3-stage fixed point with full native CLI parity (`build`, `run`, `emit-c`, `check`, `ast`, `graph`, `fmt`, `doc`, `units`, `test`, `repl`, `version`).
2. **Zero-Dependency C11 Native Runtime (`runtime/`):** Full mark-sweep garbage collector, O(1) hash-table string interning, topological reactive DAG solver (`thrum_export_graph`), SI dimensional & Gaussian uncertainty arithmetic, UTF-8 text engine, MT19937 deterministic PRNG, replay journaling, and native macOS Cocoa/Foundation backends.
3. **Native macOS Desktop GUI (`gui`) & Explicit HTTPS (`net`):** Explicit `with gui:` Cocoa/AppKit 2D canvas (shapes, clipping, font metrics, scroll wheel, clipboard) and `with net:` HTTPS client (`NSURLSession`), cleanly sandboxed behind `with` capability blocks while CLI programs link only lightweight stubs.
4. **Records, Parameterized Containers, Unions & Contracts:** Immutable product `record`s, `list[T]` and `map[K, V]` structural containers, `T | U` unions / `T?` optionals, Copy-on-Write updates, `match` destructuring, and `requires`/`ensures` contracts.
5. **23 Standard Modules:** 13 built-in modules (`math`, `const`, `stat`, `units`, `text`, `regex`, `json`, `csv`, `time`, `path`, `list`, `map`, `test`) plus 10 pure-Thrum modules under [`stdlib/`](../stdlib/physics.thrum) (`std.vec`, `std.physics`, `std.signal`, `std.finance`, `std.matrix`, `std.algo`, `std.num`, `std.geom`, `std.prob`, `std.complex`).

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
