# Thrum Deliberate Non-Goals

These features are intentionally excluded from Thrum 0.1 to preserve its core invariants (live relations, dimensional safety, purity, and object capabilities). Do **not** add them to pass tests or for convenience:

1. **Class inheritance / OOP hierarchies** — Data uses maps, pattern matching, and functions.
2. **Operator overloading** — `+`, `-`, `*`, `/`, `^`, `~` have fixed, dimension-checked meanings.
3. **Implicit numeric/unit coercion** — Adding `1 m + 1 s` is always a dimensional error; adding `1 m + 100 cm` works because both have dimension length, never via silent guessing.
4. **Ambient I/O or ambient nondeterminism** — Pure code and reactive relations cannot print, read files, read the clock, inspect environment variables, or draw random numbers without an explicit capability (`with io, fs, clock, rand, env`).
5. **Null puns / silent nil arithmetic** — `nil` is distinct from `0`, `""`, and `[]`. Use explicit `??` coalescing or `isnil(...)`.
6. **Affine temperature units (°C / °F) as unit suffixes** — Temperatures use Kelvin (`K`) plus explicit `c2k` / `k2c` conversions to avoid interval-vs-offset bugs.
