[&larr; Tutorial](tutorial.md) | [Documentation Index](index.md) | [Next: Standard Library &rarr;](library.md)

# The Thrum Language Reference

This reference manual specifies the syntax and exact runtime/static semantics of **Thrum v0.2**. Together with [grammar.ebnf](../grammar.ebnf), [SPEC.md](../SPEC.md), and the conformance tests in `tests/cases/`, it defines normative language behavior.

---

## 1. Introduction

Thrum combines eager lexical scoping with lazy/topological reactive dataflow, dimensional quantities as its core numeric model, and explicit capability-based effect control.

---

## 2. Lexical Analysis

### 2.1 Encoding and Line Structure

- Source files must be valid UTF-8 (`.thrum`).
- Comments begin with `#` and run to the end of the line.
- Statements terminate at newlines or semicolons (`;`). Implicit line joining occurs inside matched `()`, `[]`, and `{}`.

### 2.2 Indentation

Indentation uses ASCII spaces (`' '`) only. Leading tab characters (`\t`) raise a lexical error. Inconsistent dedentation widths relative to open block levels raise an indentation error.

### 2.3 Keywords

- **Hard keywords (35):** `var`, `let`, `pin`, `fn`, `return`, `if`, `elif`, `else`, `for`, `while`, `repeat`, `in`, `match`, `case`, `try`, `catch`, `fail`, `assert`, `with`, `on`, `settle`, `live`, `whatif`, `why`, `budget`, `use`, `as`, `not`, `and`, `or`, `break`, `continue`, `true`, `false`, `nil`.
- **Soft / contextual keywords:** `init`, `max`, `tol`, `rise`, `fall`, `always`, `requires`, `ensures`, `record` (treated as record declaration only in `record NAME:` header form; usable as identifier names elsewhere).

### 2.4 Text Literals

- Delimiters: `'...'`, `"..."`, `'''...'''`, `"""..."""`.
- Raw prefixes: `r"..."`, `r'...'`, `r"""..."""`, `r'''...'''` disable escape sequences and interpolation.
- Supported escapes in non-raw literals: `\n`, `\t`, `\r`, `\\`, `\"`, `\'`, `\{`, `\}`, `\0`.
- Any unescaped `{expr}` inside a non-raw text literal evaluates `expr` (arbitrary expressions, nested braces, and string literals allowed) and converts it via `str(val)`.

### 2.5 Numeric Quantities & Unit Disambiguation

```text
QTY       := NUMBER [ ( "±" | "+/-" ) NUMBER ] [ UNIT_EXPR ]
UNIT_EXPR := unit_term { ( "*" | "/" | "·" ) unit_term }   (* no internal spaces *)
```

- Decimal, underscores (`1_000_000`), scientific (`6.02e23`), hex (`0xFF`), and binary (`0b1010`) integer forms are supported for `NUMBER`.
- A unit expression attaches immediately after `NUMBER` on the same line when **no spaces** separate terms (`5 m/s^2`, `10 kg*m^2/s^2`). `5 * m` multiplies `5` by variable `m`.
- Hard keywords immediately after a number (`5 in xs`, `5 as ft`) are **never** scanned as units. Use `5 inch` or `qty(5, "in")` for inches. Bare `1` is not consumed as a unit unless written as a reciprocal such as `5 1/s`.

---

## 3. Data Model

Every runtime value has one of seven distinct types reported by `type(v)`: `"nil"`, `"bool"`, `"num"`, `"text"`, `"list"`, `"map"`, `"fn"` (or `"module"`, `"cap"`, `"record"` / `<RecordName>`).

### 3.1 Physical Quantities (`"num"`)

Numbers are immutable tuples $(v, u, \mathbf{d}, \text{disp})$ stored in IEEE-754 `double` plus exact rationals:

- **$v$ (`bare`):** Magnitude converted into SI base units.
- **$u$ (`unc`):** Non-negative absolute standard uncertainty $\sigma$ in SI base units ($0$ for exact quantities).
- **$\mathbf{d}$ (`dim`):** Seven exact rational exponents $(e_{\text{m}}, e_{\text{kg}}, e_{\text{s}}, e_{\text{A}}, e_{\text{K}}, e_{\text{mol}}, e_{\text{cd}})$.
- **$\text{disp}$:** Optional `(scale, symbol)` hint preserving preferred display unit (`as km/h`).

#### Dimensional Rules & Gaussian Uncertainty Formulas

- **`a + b`, `a - b`, `a % b`, ordering `< <= > >=`:** Require identical $\mathbf{d}$; mismatch is an error (`dimension mismatch`). Uncertainty: $\sigma(a \pm b) = \sqrt{\sigma_a^2 + \sigma_b^2}$.
- **`a == b`, `a != b`:** Mismatched dimensions compare unequal (`false` / `true`) without error.
- **`a * b`, `a / b`:** Dimension exponents add / subtract. Uncertainty:
  $$\sigma(a \cdot b) = \sqrt{(\sigma_a b)^2 + (\sigma_b a)^2}, \qquad \sigma(a / b) = \sqrt{(\sigma_a / b)^2 + (\sigma_b a / b^2)^2}$$
- **`a ^ b`:** Exponent $b$ must be dimensionless. Dimensioned $a$ requires rational exponent (integer or half-integer / exact fraction). Uncertainty uses $|b a^{b-1}|\sigma_a$ (plus $|\ln(a) a^b|\sigma_b$ when $a > 0$ and $\sigma_b > 0$).
- **`a ~ b` (approximate equality):** Requires identical dimensions; true iff $|a - b| \le \max(3\sqrt{\sigma_a^2 + \sigma_b^2},\; 10^{-9}\max(|a|, |b|))$.
- **Formatting:** Quantities with $\sigma > 0$ round uncertainty to two significant figures (`5.00 ± 0.22 m`), switching to `(v ± u)e+NN` outside $[10^{-4}, 10^4]$; exact zero or non-finite values print `0`, `inf`, `-inf`, or `nan`.

### 3.2 Truthiness (`nil` / `"bool"`)

Thrum preserves strict equality (`nil == 0`, `nil == false`, and `nil == ""` are all `false`): for boolean conditions (`if`, `while`, `on`, `not`, `and`, `or`), `false`, `nil`, numeric `v == 0.0`, empty text `""`, empty list `[]`, and empty map `{}` are falsy, while all other values (non-zero numbers, non-empty collections, records, functions, modules, capabilities) are truthy. Use `isnil(v)` or `v ?? default` when testing specifically for `nil`.

### 3.3 UTF-8 Text and Collections (Copy-on-Write)

- Texts index and slice by Unicode **codepoints** (`len`, `s[i]`, `s[a:b:step]`). Negative indices index from the end. Zero slice step raises `slice step cannot be 0`.
- Lists and maps use Copy-on-Write value semantics on indexed/dot mutation (`xs[i] = v`, `m.k = v` or `m[k] = v`): mutation allocates/updates the target variable's value and notifies reactive subscribers without aliasing bugs.
- Map keys may be `"text"`, `"num"`, `"bool"`, or `"nil"`; attempting to use a `"list"` or `"map"` as a map key raises a runtime error. Unquoted keys in `{k: v}` literals are treated as string keys (unless `[expr]: v` computed-key syntax is used; reserved hard keywords must be quoted).

### 3.4 Records

```thrum
record Force:
    val: N
    label: text = "default"
```

- Positional construction `Force(5 N)` checks arity and field types/dimensions. Map construction `Force{val: 5 N}` fills defaulted fields automatically.
- Reading non-existent fields or assigning incompatible types/dimensions (`f.val = 1 s`) raises an error.
- Field update (`var f = Force(5 N); f.val = 10 N`) performs a Copy-on-Write rebuild and notifies dependent reactive cells.

### 3.5 Compound Containers & Union/Optional Annotations

Parameter, return, and record-field annotations also support typed collections and unions enforced statically by `thrumc check` / `--strict` and dynamically at call/construction boundaries:

- **Typed Lists & Maps:** `list[m]`, `list[Pt2]`, `map[text, kg]` check element/key/value types recursively.
- **Unions & Optionals:** `m | text`, `num | nil`, and shorthand `T?` (`kg?` is `kg | nil`).

---

## 4. Execution Model and Reactivity

### 4.1 Scopes: Relation vs. Imperative

- **Relation scopes:** Module top level, top-level `with` blocks, `live:` blocks, `settle:` blocks.
- **Imperative scopes:** Function bodies (`fn`), loops (`for`, `while`, `repeat`), `on` bodies, `whatif` blocks, `match` case arms, `try`/`catch` handlers.

`var` always creates a mutable cell. `pin` always evaluates once and binds immutably. `let` creates a **live reactive cell** inside relation scopes, and an **immutable local binding** inside imperative scopes.

### 4.2 Glitch-Free Topological Propagation & Purity Theorem

1. Every cell has topological height `0` (source `var` or literal `let`) or `1 + max(dep.height)`.
2. When one or more `var` inputs change cleanly inside a reactive wave, reachable dependents are queued in a min-priority order by `height`. Each affected cell recomputes at most once after all lower-height dependencies have settled (**zero glitches**).
3. Evaluating any reactive cell (or `whatif` block) enforces **runtime purity**: assigning to any cell created prior to the current evaluation (`cell.born < eval_id`) raises `cannot mutate '...' inside a relation`.
4. Unannotated cyclic cell dependencies raise `cyclic relation involving '...'`.

### 4.3 `settle` Fixpoint Solver

```thrum
settle max 50 tol 1e-10:
    let x = 0.5 * (x + y) init 1
    let y = (a + x) / 2 init 1
```

Cells inside `settle` belong to a shared `Group` scheduled atomically by collective group height. On change, cells evaluate iteratively in declaration order until $\max_c \text{relchg}(c_{\text{old}}, c_{\text{new}}) \le \text{tol}$ or `max` rounds exhaust (`settle did not converge`). Default bounds: `max 100`, `tol 1e-12`. Only `var`/`let`/`pin` declarations are valid inside `settle`; `init` is illegal outside `settle`.

### 4.4 `on` Reactions and Cascades

```thrum
on CONDITION [rise | fall | always]:
    ...
```

- **`rise` (default):** Fires when `truthy(CONDITION)` transitions `false -> true` across a settled wave.
- **`fall`:** Fires on `true -> false`.
- **`always`:** Fires after every propagation wave in which `CONDITION` evaluates truthy.

Reaction bodies may mutate `var`s, scheduling a fresh propagation wave, capped at `1000` cascade rounds (`reaction cascade exceeded 1000 rounds`).

---

## 5. Expressions and Operator Precedence

Listed from **lowest** to **highest** precedence:

| Level | Operators | Notes |
| :---: | :--- | :--- |
| 1 | `or` | Short-circuit logical OR |
| 2 | `and` | Short-circuit logical AND |
| 3 | `not` | Prefix logical negation |
| 4 | `==` `!=` `<` `<=` `>` `>=` `~` `in` `not in` | Comparison, uncertainty approx `~`, membership |
| 5 | `??` | Nil-coalescing (`a ?? b`: returns `b` iff `a` is `nil`) |
| 6 | `\|>` | Pipeline (`x \|> f` -> `f(x)`; `x \|> f(a)` -> `f(x, a)`) |
| 7 | `..` `..<` | Inclusive and half-open integer ranges |
| 8 | `+` `-` | Numeric dimension-checked add/sub, text concatenation |
| 9 | `*` `/` `%` | Multiply, divide, dimension-checked modulo |
| 10 | unary `-` unary `+` | Numeric sign |
| 11 | `^` | Right-associative exponentiation |
| 12 | `f(args)` `x[i]` `x[a:b:s]` `Rec{...}` `x.attr` `x as unit` | Calls, indexing, slicing, record literals, member access, unit cast |

---

## 6. Capabilities and Sandboxing

Pure code has no ambient I/O. External interaction requires entering `with CAP1, CAP2(arg), ...:`:

- Capability bindings (`io`, `fs`, etc.) are pinned cleanly in that scope.
- Exit from `with` (whether normal return, `break`, or caught `fail`) runs resource cleanups automatically in LIFO order (e.g. `tty` restores terminal canonical mode and cursor visibility; `gui` closes active Cocoa windows).

---

## 7. Type System, Static Checker, and Contracts

Annotations (`: num`, `: text`, `: kg`, `: m/s^2`, `: Vec2`, `-> J`) are verified dynamically on parameter entry, field write, and function return, and verified statically ahead of time by `thrumc check` / `thrumc build --strict` / `thrum.py check`.

Functions and lambdas support runtime-checked `requires cond[, msg]` preconditions and `ensures cond[, msg]` postconditions (where `result` binds the candidate return value before return type verification).

---

## 8. Grammar

The complete machine-readable grammar lives in [grammar.ebnf](../grammar.ebnf).

---

[&larr; Tutorial](tutorial.md) | [Documentation Index](index.md) | [Next: Standard Library &rarr;](library.md)
