[&larr; Internals](internals.md) | [Documentation Index](index.md)

# Command Line, Makefile, and Packaging Guide

Thrum provides two command-line interfaces and a unified `Makefile`:

1. **`thrumc`** — The standalone native compiled CLI written in Thrum (`selfhost/thrumc.thrum`). It emits C11, links against `runtime/libthrumrt.a`, and runs natively with zero Python dependencies.
2. **`thrum.py`** — The pure-Python 3.9+ reference interpreter CLI (`thrum_lang/cli.py`), useful for REPL sessions, dependency graph export (`dot`/`mermaid`), and AST inspection.

---

## 1. Native Compiler CLI (`thrumc`)

General syntax:

```text
thrumc <subcommand> [arguments...]
```

When invoked outside the repository root, `thrumc` searches `$THRUM_HOME`, its own executable directory ancestors, and the current working directory to locate `runtime/` and `stdlib/`.

### `build`

Compile a `.thrum` file (and all transitively `use`d modules) into a standalone native binary:

```bash
./thrumc build examples/solitaire.thrum -o solitaire
```

| Flag | Description |
| :--- | :--- |
| `-o <out>` | Output executable path (defaults to input stem). |
| `--keep-c` | Preserve the intermediate `<out>.thrumc.c` generated C file. |
| `--strict` | Run full static type/dimension checking (`checker.thrum`) before emitting C. |

> **Note:** By default (`--strict` omitted), `build` and `run` validate syntax and module resolution while deferring dynamic/negative runtime checks so constructs like `test.fails(fn(): return 1 m + 1 s, ...)` execute at runtime cleanly. Run `./thrumc check` or pass `--strict` to enforce static rejection at compile time.

### `run`

Compile a `.thrum` program to a temporary binary, execute it, forward CLI arguments/flags, and clean up temporary files:

```bash
./thrumc run examples/bmi.thrum
./thrumc run examples/thermostat.thrum --journal run.json
./thrumc run examples/thermostat.thrum --replay run.json
./thrumc run prog.thrum --strict -- --flag arg1
```

### `emit-c`

Generate portable C11 source without invoking the C compiler:

```bash
./thrumc emit-c examples/bmi.thrum -o bmi.c
./thrumc emit-c examples/bmi.thrum -o -        # write to stdout
```

### `check`

Run static syntax, scope, capability, module, and dimensional/type analysis without executing the program:

```bash
./thrumc check examples/solitaire.thrum
```

Exits `0` printing `<file>: ok`, or exits `1` with `file:line:col: error:` diagnostics and carets.

### `fmt`

Format `.thrum` source code to normalized 4-space indentation, trimmed trailing spaces, and single trailing newline (after syntax validation):

```bash
./thrumc fmt file.thrum          # print formatted source to stdout
./thrumc fmt file.thrum --write  # rewrite file in place
./thrumc fmt file.thrum --check  # exit 1 if formatting changes are needed
```

### `doc`

Extract documentation comments and top-level signatures (`fn`, `record`, `var`, `let`, `pin`) from `.thrum` files, `std.*` modules, or list exports of built-in modules:

```bash
./thrumc doc std.physics
./thrumc doc path
./thrumc doc examples/bmi.thrum
```

### `units`

List registered units in the runtime registry with dimensions and SI scale factors, optionally filtering by substring:

```bash
./thrumc units
./thrumc units ohm
```

### `test`

Run bundled language self-tests (`16` core semantics tests) or recursively discover and execute `*_test.thrum` suites natively:

```bash
./thrumc test
./thrumc test tests/stdlib_test.thrum
./thrumc test tests/
```

### `version`

```bash
./thrumc version
```

---

## 2. Standalone Runtime Flags (Compiled Binaries)

Every binary produced by `thrumc build` accepts Thrum runtime flags before user arguments (or separated by `--`):

| Flag | Description |
| :--- | :--- |
| `--journal <file>` | Record capability results (`io`, `clock`, `rand`, `fs`, `env`, `args`, `tty`, `proc`) and output stream divergence checks to JSON. |
| `--replay <file>` | Replay execution deterministically from `<file>`, failing with exit code `2` on capability divergence or unexpected `stdout`/`stderr`. |
| `--trace` | Print reactive propagation traces (`var` changes, cell updates, reactions). |
| `--` | End Thrum runtime flags; pass all subsequent arguments verbatim to `args.all()`. |

Example:

```bash
./tetris --journal game.json -- --demo 15 --plain
./tetris --replay game.json  -- --demo 15 --plain
```

---

## 3. Python Reference CLI (`thrum.py`)

When running via Python 3.9+, `thrum.py` provides additional interactive and inspection commands:

| Command | Purpose |
| :--- | :--- |
| `python3 thrum.py repl` | Interactive REPL supporting `:vars`, `:why <name>`, `:budget <name>`, `:units [q]`, `:reset`. |
| `python3 thrum.py ast FILE` | Pretty-print the parsed AST. |
| `python3 thrum.py graph FILE [SYM] --format mermaid\|dot` | Export the runtime reactive cell graph as Mermaid or Graphviz DOT. |

---

## 4. `Makefile` Targets & macOS `.app` Packaging

| Target | Action |
| :--- | :--- |
| `make` / `make all` | Build C/Cocoa runtime (`libthrumrt.a`, `rt_gui_*`, `rt_net_*`), `./thrumc`, `./tetris`, `./solitaire`, and `./browser`. |
| `make runtime` | Compile the C11 runtime archive, GUI backends, and `net` backends with `-Wall -Wextra -Werror`. |
| `make thrumc` | Compile Stage 1 & Stage 2 `./thrumc` from `selfhost/*.thrum`. |
| `make bootstrap` | Emit Stage 2 and Stage 3 C outputs and verify `diff -u` is byte-identical. |
| `make tetris` | Compile standalone `./tetris` binary. |
| `make solitaire` | Compile standalone `./solitaire` binary with Cocoa framework. |
| `make browser` | Compile standalone `./browser` web browser binary with Cocoa + Foundation HTTPS networking. |
| `make app` | Package `ThrumSolitaire.app` and `ThrumBrowser.app` with `Info.plist` and `AppIcon.icns` from `assets/thrum_logo.png`. |
| `make native-test` | Run `tests/run_native_tests.py` (conformance cases, native self-tests, stdlib & browser suites, headless GUI/browser checks). |
| `make test` | Run `native-test` plus all Python reference test suites. |
| `make clean` | Remove build artifacts, libraries, binaries, and `.app` bundles. |

---

[&larr; Internals](internals.md) | [Documentation Index](index.md)
