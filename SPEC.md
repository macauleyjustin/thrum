# Thrum Language Specification (v0.2)

> **A language where values stay in tune.**

This document is the normative specification for **Thrum 0.2**. Together with [grammar.ebnf](grammar.ebnf) and the executable conformance suite in `tests/`, it defines the required semantics of any Thrum implementation.

---

## 1. Core Semantics & Invariants

Thrum is built around five non-negotiable invariants:

1. **Live (`var` / `let` / `pin` / `settle`):** `let` declares a standing relation in reactive scopes rather than a one-time assignment. Derived values stay consistent via glitch-free, height-ordered topological propagation.
2. **Quantified (`72 ± 0.5 kg`, `as ft`, `-> J`):** Physical quantities are the *only* numeric type. Every number carries an SI base value, an exact rational 7-dimensional vector `(m, kg, s, A, K, mol, cd)`, an absolute uncertainty `σ`, and an optional display unit.
3. **Explainable (`why`, `budget`, `whatif`):** Every reactive cell carries lineage and source text; `why` prints its derivation tree, `budget` computes an uncertainty sensitivity table, and `whatif` evaluates counterfactual inputs without mutating program state.
4. **Sandboxed (`with io, clock, rand(7), fs("./data"), env, args`):** There is **no ambient authority**. Pure code and reactive relations cannot perform I/O or observe nondeterminism without an explicit capability value.
5. **Replayable (`--journal` / `--replay`):** Capability interactions are recorded to a JSON journal and can be replayed deterministically for bit-exact reruns.

---

## 2. Lexical Structure

- Source files are UTF-8 encoded with extension `.thrum`.
- **Indentation:** Spaces only. Tabs (`\t`) in leading indentation are a lexical error. Inconsistent dedentation is an error.
- **Comments:** `#` begins a single-line comment extending to the end of the line.
- **Statement terminators:** Newlines terminate statements; `;` is an equivalent statement separator. Newlines are ignored inside `()`, `[]`, and `{}`.
- **Hard keywords:**
  `var`, `let`, `pin`, `fn`, `return`, `if`, `elif`, `else`, `for`, `while`, `repeat`, `in`, `match`, `case`, `try`, `catch`, `fail`, `assert`, `with`, `on`, `settle`, `live`, `whatif`, `why`, `budget`, `use`, `as`, `not`, `and`, `or`, `break`, `continue`, `true`, `false`, `nil`.
- **Soft / contextual keywords:**
  `init`, `max`, `tol`, `rise`, `fall`, `always`, `requires`, `ensures`, `record` (recognized as record declaration only in `record NAME:` header form).
- **Text literals:** Single (`'...'`), double (`"..."`), or triple-quoted (`'''...'''`, `"""..."""`), plus raw text literals (`r"..."`, `r'...'`, `r"""..."""`). Non-raw strings support escapes `\n`, `\t`, `\r`, `\\`, `\"`, `\'`, `\{`, `\}`, `\0` and `{expr}` interpolation (with nested braces and quoted strings permitted inside `{...}`). Raw strings disable escapes and interpolation (ideal for regex, JSON, and format templates).

---

## 3. Quantities, Units, and Uncertainty

### 3.1 Numeric Literal Syntax & Disambiguation
A numeric literal has the form:
```
NUMBER [ ( "±" | "+/-" ) NUMBER ] [ UNIT ]
```
- Numbers support decimal, scientific (`1.5e-15`), underscore separators (`1_000`), hex (`0xFF`), and binary (`0b1010`).
- **Disambiguation rule:** A unit expression immediately following a numeric literal on the same line binds to that literal (`5 m` is five meters; `5 * m` multiplies `5` by variable `m`).
- Inside a unit run, `*`, `/`, `·`, `^`, and `()` must have **no spaces**: `5 m/s^2` is a unit; `5 m / s^2` is `(5 m) / s^2`.
- **Keyword protection:** A hard keyword (such as `in` or `as`) immediately following a numeric literal is **never** consumed as a unit suffix (`5 in xs` parses `in` as membership; `5 as ft` parses `as` as unit conversion; use `5 inch` or `qty(5, "in")` for inches in literals). Bare `1` after a number is not consumed as a unit unless followed by `/` (`5 1/s`).

### 3.2 Dimensional Analysis & Arithmetic
- Dimension vectors are 7-tuples of `Fraction` over `(m, kg, s, A, K, mol, cd)`.
- `+` and `-` require identical dimensions; mismatch is an error (`dimension mismatch`), never implicit coercion.
- `==` and `!=` return `false` / `true` respectively on mismatched dimensions.
- `<`, `<=`, `>`, `>=` require identical dimensions; mismatch is an error.
- `*` adds dimension vectors; `/` subtracts dimension vectors; `%` requires identical dimensions.
- `^` requires a dimensionless exponent. Dimensioned bases require a finite rational exponent; dimensionless bases accept any finite float exponent when non-negative.
- `as UNIT` converts to a dimensionally compatible display unit; incompatible dimensions raise an error.
- Affine temperatures (`°C`, `°F`) are intentionally omitted as units; use `K` with `c2k`/`k2c` (or `units.f2k`/`units.k2f`).

### 3.3 Uncertainty Propagation & Formatting
First-order independent Gaussian propagation:
- $\sigma(a \pm b) = \sqrt{\sigma_a^2 + \sigma_b^2}$
- $\sigma(a \cdot b) = \sqrt{(\sigma_a b)^2 + (\sigma_b a)^2}$
- $\sigma(a / b) = \sqrt{(\sigma_a / b)^2 + (\sigma_b a / b^2)^2}$
- $\sigma(a^n) = |n \cdot a^{n-1}| \sigma_a$ (plus $|\ln(a) a^n| \sigma_b$ if exponent $b$ has uncertainty and $a > 0$).
- `a ~ b` tests uncertainty-aware approximate equality: $|a - b| \le \max(3\sqrt{\sigma_a^2 + \sigma_b^2},\; 10^{-9}\max(|a|, |b|))$ with identical dimensions.
- Values with `u > 0` format with two significant figures on `u`; magnitudes outside $[10^{-4}, 10^4]$ use scientific `(v ± u)e+NN` notation. Non-finite values print `inf`, `-inf`, or `nan`.

---

## 4. Bindings, Reactivity, Purity, and `settle`

### 4.1 Bindings
- `var x = e`: Mutable **source cell**. The only binding kind that may be assigned to (`=`, `+=`, `-=`, `*=`, `/=`, `^=`, `%=`).
- `let y = e`:
  - In a **relation scope** (module top-level, top-level `with` blocks, `live:` blocks, `settle:` blocks): creates a **derived reactive cell** that automatically tracks cells read during evaluation and recomputes whenever an upstream dependency changes.
  - In an **imperative scope** (`fn`, `for`, `on`, `whatif`, `match` case bodies): creates an **immutable local**. Assigning to any `let` binding is an error.
- `pin z = e`: Snapshot evaluated once; never tracked and never assignable.

### 4.2 Purity & Glitch-Free Propagation
- **Purity theorem (runtime enforced):** While evaluating a reactive relation (or `whatif` sandbox), mutating any cell born before that evaluation began raises an error (`cannot mutate '...' inside a relation`). Local `var` mutations inside functions called by a relation are permitted.
- **Glitch-free topological order:** When source cells change, transitive dependents are recomputed in ascending `height` order (including `settle` fixpoint groups by group height), ensuring each cell evaluates at most once per propagation wave with consistent inputs.
- **Cycles:** Unannotated cycles raise `cyclic relation involving '...'` unless inside a `settle` block.

### 4.3 `settle` Fixpoint Solver & `on` Reactions
- `settle [max N] [tol E]:` declares a mutually-recursive relation group (with `init` seeds) relaxed iteratively to a fixpoint (`max 100`, `tol 1e-12` defaults). Re-relaxes automatically when upstream inputs change.
- `on COND [rise | fall | always]:` runs side-effecting reactions after propagation settles, guarded by a 1000-round cascade limit.

---

## 5. Records, Collections, Types, and Contracts

### 5.1 Records
```thrum
record Vec2:
    x: m
    y: m = 0 m
```
- Immutable product types constructed positionally `Vec2(3 m, 4 m)` or by field map `Vec2{x: 3 m, y: 4 m}`.
- Field types (kinds, dimensions, or nested records) are checked on construction and on copy-on-write field update (`var p = Vec2(1 m, 2 m); p.x = 5 m`).
- Matched in `match` via `case Vec2{x, y}:` or `case mod.Vec2{x, y}:`.

### 5.2 Collections & Slices
- Lists and maps use copy-on-write value semantics (`xs[0] = v`, `m.k = v` rebuild and notify reactive subscribers).
- Slices `xs[start:stop]` / `xs[start:stop:step]` work on lists and texts.
- Map keys can be `text`, `num`, `bool`, or `nil` with uniform lookup across indexing, literals, and `in`.

### 5.3 Types, Contracts, and Static Checker (`thrum check`)
- Parameter, field, and return annotations accept shape kinds (`num`, `text`, `bool`, `list`, `map`, `fn`, `any`, `module`, `cap`, `record`, `nil`), dimensions (`kg`, `m/s^2`, `J`, `1`), record names (`Vec2`, `vec.Vec2`), parameterized containers (`list[T]`, `map[K, V]`), and union/optional types (`T | U`, `T?`).
- `requires` / `ensures` contracts (with `result` bound in `ensures`) are enforced at runtime on named and anonymous functions.
- `thrum check` statically verifies syntax, name/capability/module resolution, `settle`/`init` structure, assignment immutability, and conservative dimension/type inference prior to execution.

---

## 6. Provenance, Capabilities, and Standard Library

- **Provenance:** `why NAME`, `budget NAME`, `whatif var = expr: ...`
- **Capabilities:**
  - `io`: `say`, `print`, `ask`, `eof`, `warn`, `err`, `table`
  - `clock`: `now`, `mono`, `sleep`
  - `rand([seed])`: `float`, `int`, `pick`, `shuffle`, `sample`, `gauss`
  - `fs([root])`: `read`, `read_lines`, `write`, `write_lines`, `append`, `exists`, `is_file`, `is_dir`, `list`, `mkdir`, `remove`
  - `env`: `get`, `has`, `keys`
  - `args`: `all`, `get`, `len`
  - `tty`: `is_tty`, `enter_raw`, `exit_raw`, `hide_cursor`, `show_cursor`, `clear`, `write`, `size`, `poll_key` (automatically restores raw mode and cursor visibility when exiting `with tty:`)
  - `proc`: `run`, `exe`, `cwd`, `exit`
  - `gui`: `open`, `is_open`, `close`, `size`, `poll`, `wait`, `begin`, `clear`, `rect`, `round_rect`, `stroke_rect`, `circle`, `line`, `text`, `clip`, `unclip`, `measure`, `clipboard`, `set_clipboard`, `commit`, `screenshot`
  - `net`: `get`, `post`
- **Built-in Standard Modules (`use <name>`):**
  `math`, `const`, `stat`, `units`, `text`, `regex`, `json`, `csv`, `time`, `path`, `list`, `map`, `test`.
- **Standard Library (`use std.<name>`):**
  `std.vec`, `std.physics`, `std.signal`, `std.finance`, `std.matrix`, `std.algo`, `std.num`, `std.geom`, `std.prob`, `std.complex`.

---

## 7. CLI Toolchain

- `thrum run FILE [--journal J] [--replay R] [--trace] [-- ...]`
- `thrum check FILE` — syntax, scope, and static dimension/type check
- `thrum fmt FILE [--write | --check]` — source code formatter
- `thrum doc TARGET` — documentation extractor for built-in modules, `std.*`, or `.thrum` files
- `thrum graph FILE [SYMBOL] [--format dot|mermaid]` — dependency graph exporter
- `thrum ast FILE` — AST dump
- `thrum units [QUERY]` — unit registry browser
- `thrum test [DIR|FILE]` — bundled self-tests or project `*_test.thrum` runner
- `thrum repl` — interactive REPL (`:help`, `:vars`, `:why`, `:budget`, `:units`, `:reset`)
