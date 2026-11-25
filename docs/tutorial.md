[&larr; What's New](whatsnew.md) | [Documentation Index](index.md) | [Next: Language Reference &rarr;](reference.md)

# The Thrum Tutorial

Thrum is an intuitive yet rigorous programming language built around **live reactive relations**, **physical quantities with units and uncertainties**, **provenance inspection**, and **explicit object capabilities**.

This tutorial introduces Thrum systematically through runnable examples.

---

## 1. Whetting Your Appetite

In conventional languages, spreadsheets feel magical until you try to write real programs in them, and physics/engineering code silently drops units (`meters` added to `feet` or `seconds`), ignores measurement error bars, and hides where every side effect comes from.

Thrum unifies these ideas in a clean indentation-based syntax:

- Values declared with `let` in a relation scope **stay synchronized automatically** when `var` inputs change.
- Every number carries an exact **SI dimension** and an optional **uncertainty (`±`)**.
- Any expression or module function cannot print, read files, inspect the clock, open a window, or generate random numbers unless granted an explicit **capability** (`with io, fs, rand, gui:`).
- Compiled binaries produced by `thrumc` run natively with zero interpreter overhead.

---

## 2. Building and Using the Toolchain

Build the runtime library, self-hosted native compiler (`thrumc`), and sample apps:

```bash
make
```

Verify a script statically, run it, or compile it into a standalone executable:

```bash
./thrumc check examples/bmi.thrum
./thrumc run examples/bmi.thrum
./thrumc build examples/tetris.thrum -o tetris
```

For interactive exploration, launch the native compiler REPL (or Python reference REPL):

```bash
./thrumc repl
```

---

## 3. An Informal Introduction to Thrum

### 3.1 Using Numbers, Units, and Uncertainties

Physical quantities are Thrum's **only** numeric type. A bare number (`42`, `3.14`) is simply a dimensionless quantity (`dim = 1`). Attaching a unit immediately after a literal creates a dimensioned quantity:

```thrum
let distance = 42.195 km
let time     = 2 h + 1 min + 9 s
let pace     = distance / time

with io:
    io.say("Distance in miles:", round(distance as mi, 2))
    io.say("Average speed:", round(pace as km/h, 2))
```

Attempting to combine incompatible dimensions immediately raises a dimensional error instead of silently returning nonsense:

```thrum
let bad = 10 m + 5 s   # Error: dimension mismatch: m + s
```

#### Measurement Uncertainties (`±` / `+/-`)

Attach first-order uncertainties directly on literals (`72 ± 0.5 kg`, or ASCII `72 +/- 0.5 kg`) or dynamically via `with_unc(val, sigma)`. Thrum propagates independent Gaussian errors automatically through `+`, `-`, `*`, `/`, `^`, and `sqrt`:

```thrum
let mass   = 2.0 ± 0.1 kg
let speed  = 3.0 ± 0.2 m/s
let energy = 0.5 * mass * speed^2

with io:
    io.say(energy)             # prints: 9.0 ± 1.3 J
    io.say(energy ~ 9.5 J)     # approximate uncertainty-aware equality: true
```

### 3.2 Text, Interpolation, and Indexing

Texts are UTF-8 strings delimited by `'...'`, `"..."`, triple quotes `"""..."""`, or raw quotes `r"..."`. Non-raw texts interpolate any expression inside `{...}`:

```thrum
let name = "Thrum"
let g    = 9.81 m/s^2
with io:
    io.say("Welcome to {upper(name)}! g = {fix(g, 2)}")
    io.say(r"Raw strings keep {braces} and \n literal.")
```

Indexing and slicing work cleanly by Unicode codepoint:

```thrum
let word = "café"
with io:
    io.say(len(word), word[-1], word[1:3])   # 4 é af
```

### 3.3 Lists and Maps

Lists (`[...]`) and maps (`{...}`) support comprehensions, slices, and copy-on-write updates:

```thrum
let squares = [x^2 for x in 1..6 if x % 2 == 0]   # [4, 16, 36]
var config  = {host: "localhost", port: 8080}
config.port = 9000
```

Inclusive ranges use `a..b` (`1..4` -> `[1, 2, 3, 4]`), and half-open ranges use `a..<b` (`0..<4` -> `[0, 1, 2, 3]`).

---

## 4. Reactive Bindings (`var`, `let`, `pin`) and Glitch-Freedom

In module top-level scopes, `with` blocks, and `live:` blocks, declarations create cells in a **reactive dependency graph**:

- **`var name = expr`:** Mutable source cell.
- **`let name = expr`:** Live derived relation. It tracks every cell read during evaluation and automatically recomputes whenever any upstream dependency changes.
- **`pin name = expr`:** Constant snapshot evaluated once; never tracks or re-evaluates.

```thrum
var a = 1
let b = a + 1       # depends on a
let c = a * 2       # depends on a
let d = b + c       # diamond join: depends on b and c

with io:
    io.say(d)       # 4
    a = 10          # updates b -> 11, c -> 20, then d -> 31 in one consistent wave
    io.say(d)       # 31
```

Notice there is no temporary "glitch" where `d` briefly evaluates with new `b = 11` and stale `c = 2`: propagation runs strictly in ascending topological **height** order.

### Runtime Purity

Inside any `let` relation, calling a function that tries to mutate a pre-existing source `var` raises `cannot mutate '...' inside a relation`. Local `var` loop counters created *inside* a function called by a relation are allowed cleanly.

---

## 5. Control Flow, Pattern Matching, and Error Handling

```thrum
fn classify(v) -> text:
    match v:
        case 0:
            return "zero"
        case x if type(x) == "num" and x < 0:
            return "negative"
        case [head, *tail]:
            return "list starting with {head}, rest len={len(tail)}"
        case {kind: "circle", r}:
            return "circle r={r}"
        case _:
            return "other"
```

Loops cover `for item in xs:`, `for k, v in items(mp):`, `while cond:`, and `repeat N:`. Errors throw via `fail("reason")` or failed contracts/operations and can be handled with `try: ... catch e:`:

```thrum
try:
    let bad = 1 m + 1 s
catch e:
    with io:
        io.say("Caught:", e.message)
```

---

## 6. Functions, Closures, Pipelines, Contracts, and Records

### Functions, Lambdas, and Pipelines (`|>`)

```thrum
fn scale(x: num, factor: num = 2) -> num:
    return x * factor

let total = (
    [1, 2, 3, 4]
        |> filter(\it % 2 == 0)
        |> map(\it^2)
        |> sum()
)   # 20
```

Short anonymous lambdas use `\expr` with `it` bound to the single parameter. Full multi-parameter/typed anonymous functions use `fn(x, y): ...`.

### Preconditions and Postconditions (`requires` / `ensures`)

```thrum
fn kinetic_energy(m: kg, v: m/s) -> J:
    requires m >= 0 kg, "mass cannot be negative"
    ensures result >= 0 J
    return 0.5 * m * v^2
```

### Typed Immutable `record`s

```thrum
record Particle:
    pos: m
    vel: m/s = 0 m/s

var p = Particle(10 m, 2 m/s)
let future_x = p.pos + p.vel * 5 s   # 20 m

with io:
    io.say(future_x)
    p.pos = 100 m                    # CoW update triggers `future_x` -> 110 m
    io.say(future_x)
```

Match patterns can destructure records directly by name: `case Particle{pos, vel}:`.

---

## 7. Fixpoints (`settle`) and Edge Reactions (`on`)

Cyclic equations are errors in general graphs unless placed in a `settle` fixpoint block, where declarations provide `init` seeds and relax iteratively until relative change `<= tol`:

```thrum
var n = 2
settle max 60 tol 1e-12:
    let sqrt_n = (sqrt_n + n / sqrt_n) / 2 init 1

with io:
    io.say(round(sqrt_n, 6))    # 1.414214
    n = 9
    io.say(round(sqrt_n, 6))    # 3
```

Side-effecting triggers run **after** propagation waves finish via `on COND [rise | fall | always]:`:

```thrum
var temp = 290 K
with io:
    on temp > 300 K rise:
        io.say("Overheat alert:", temp)
    temp = 305 K    # fires
    temp = 310 K    # already > 300 K: rise does NOT re-fire
    temp = 295 K
    temp = 302 K    # fires again
```

---

## 8. Explainability and Counterfactuals (`why`, `budget`, `whatif`)

Thrum computes lineage and sensitivity natively:

```thrum
var v = 12 ± 0.3 m/s
var t = 4 ± 0.1 s
let dist = v * t

with io:
    whatif v = 15 m/s:
        io.say("What-if distance:", dist)   # 60 m
    io.say("Actual distance:", dist)        # still 48 ± 1.7 m!
    why dist
    budget dist
```

- `why` prints the live derivation tree with formulas and cached values.
- `budget` ranks each uncertain input's percentage variance contribution and partial derivative sensitivity.
- `whatif` temporarily shadows source cells inside an isolated block without leaving side effects behind.

---

## 9. Modules, Capabilities, and Deterministic Replay

Use built-in modules (`use math`), standard library modules (`use std.physics`), or local files (`use "./helpers.thrum" as h`).

Capabilities are granted explicitly by `with`:

- `io` — console text, tables, prompts
- `fs([root])` — sandboxed file reads/writes (cannot escape `root`)
- `clock` — wall clock, monotonic timer, sleep
- `rand([seed])` — Mersenne Twister PRNG
- `env`, `args`, `proc`, `tty`
- `gui` — native macOS Cocoa window & 2D drawing primitives

Any run recording capabilities with `--journal out.json` can be replayed deterministically offline with `--replay out.json`, even for interactive random/clock/stdin workflows.

---

## 10. Writing a Native Desktop GUI App

Here is a minimal animated bouncing-orb GUI program compiled with `./thrumc build`:

```thrum
var x = 120
var dx = 4

with gui:
    gui.open("Thrum Pulse", 640, 420)
    while gui.is_open():
        for ev in gui.poll():
            if ev.kind == "quit" or (ev.kind == "key" and ev.key == "esc"):
                gui.close()
        x += dx
        if x > 580 or x < 60:
            dx = -dx
        gui.begin()
        gui.clear(14, 52, 34)
        gui.circle(x, 210, 36, 245, 206, 78)
        gui.text(320, 40, "Press ESC or close window to exit", 15, 225, 240, 232, "mid", true)
        gui.commit()
        gui.wait(16)
```

Run `THRUM_HOME=. ./thrumc build my_gui.thrum -o my_gui && ./my_gui` on macOS to open a live Cocoa desktop window directly from native code.

---

[&larr; What's New](whatsnew.md) | [Documentation Index](index.md) | [Next: Language Reference &rarr;](reference.md)
