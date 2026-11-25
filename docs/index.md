# Thrum 0.2.0 Documentation

> *A language where values stay in tune.*

Welcome! This is the official documentation for **Thrum 0.2.0**.

---

## Parts of the Documentation

| Guide | Who it is for |
| :--- | :--- |
| **[What's New in Thrum 0.2](whatsnew.md)** | Important new features, the self-hosted C11 compiler, and native Cocoa GUI support |
| **[The Thrum Tutorial](tutorial.md)** | *Start here:* step-by-step introduction to quantities, reactivity, contracts, and GUI programming |
| **[The Thrum Language Reference](reference.md)** | Exact specification of syntax, data model, propagation semantics, types, and grammar |
| **[The Thrum Standard Library](library.md)** | Built-in functions, `std.*` modules, capability interfaces, and the SI unit registry |
| **[Compiler & Runtime Internals](internals.md)** | How `thrumc`, the 3-stage bootstrap, GC, DAG solver, and Cocoa backend work |
| **[Command Line, Makefile & Packaging](cli.md)** | Using `thrumc`, `thrum.py`, `--journal`/`--replay`, `Makefile`, and macOS `.app` bundles |

---

## Design & Specification Documents

- **[Normative Specification (`SPEC.md`)](../SPEC.md)** — The five non-negotiable invariants of Thrum 0.2.
- **[Deliberate Non-Goals (`NON_GOALS.md`)](../NON_GOALS.md)** — Features intentionally excluded from Thrum.
- **[Formal Grammar (`grammar.ebnf`)](../grammar.ebnf)** — Frozen EBNF grammar definition.

---

## Showcase Programs

- **[Tetris (`examples/tetris.thrum`)](../examples/tetris.thrum)** — Live reactive ANSI terminal game with AI demo mode and raw TTY input.
- **[N-Body Simulation (`examples/nbody_sim.thrum`)](../examples/nbody_sim.thrum)** — Dimension-checked gravitational integration using `std.vec` and `std.physics`.
- **[Lab Data Pipeline (`examples/lab_pipeline.thrum`)](../examples/lab_pipeline.thrum)** — Experimental uncertainty propagation, regression (`stat.linfit`), and error budgets.
- **[Thermostat Controller (`examples/thermostat.thrum`)](../examples/thermostat.thrum)** — Feedback control with `settle` fixpoint relaxation and `on rise` reactions.

---

## Quick Navigation by Topic

- **Want to learn the syntax?** Read [The Thrum Tutorial](tutorial.md).
- **Need to look up a function or unit?** Check [The Thrum Standard Library](library.md).
- **Debugging a reactive cycle or purity error?** See [Execution Model](reference.md#4-execution-model-and-reactivity) and [`why` / `whatif` Internals](internals.md#5-reactive-graph-and-fixpoint-engine).
- **Interested in how the compiler compiles itself?** Read [3-Stage Bootstrap](internals.md#3-three-stage-fixed-point-bootstrap).
