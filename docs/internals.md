[&larr; Standard Library](library.md) | [Documentation Index](index.md) | [Next: CLI &amp; Packaging &rarr;](cli.md)

# How Thrum Works: Compiler & Runtime Internals

This guide explains the internal architecture of Thrum: how source code is parsed, type-checked, lowered to C11, bootstrapped to a self-hosting fixed point, and executed by the garbage-collected reactive runtime and native macOS Cocoa backend.

---

## 1. Dual Implementation & Architecture Overview

Thrum has two aligned implementations governed by [SPEC.md](../SPEC.md) and the executable conformance suite in [`tests/cases/`](../tests/run_tests.py):

1. **Self-Hosted Native Toolchain (`selfhost/` + `runtime/`):** Written in 3,803 lines of Thrum (`selfhost/*.thrum`), producing portable C11 compiled against 10,179 lines of C11/Objective-C (`runtime/*`). Zero Python required at compile or run time once bootstrapped.
2. **Pure Python Reference Toolchain (`thrum_lang/`):** Direct tree-walking interpreter, static checker, formatter, and REPL in zero-dependency Python 3.9+, used as Stage 0 during cold bootstrap from source.

Compilation & execution pipeline in `thrumc`:

```text
  .thrum source
       |
       v
  [ lexer.thrum ]       Indentation (INDENT/DEDENT), unit disambiguation, string interpolation
       |
       v
  [ parser.thrum ]      Recursive-descent + Pratt expression parser -> CoW AST maps
       |
       v
  [ checker.thrum ]     (a) Static check (`check`, `--strict`): types, 7-SI dims, purity, scope
       |                (b) Module resolver (`build`/`run`): collects transitive `use` files
       v
  [ codegen_c.thrum ]   Lowers functions/closures, records, reactive thunks, modules -> C11
       |
       +--> emits `/* THRUM_CAPS: ... */` on line 1 when `gui` or `net` is requested
       |
       v
  [ host cc / clang ]   Links C11 object + `runtime/libthrumrt.a` + Cocoa/Foundation (`gui`/`net`) or stubs
       |
       v
  Standalone Mach-O / native executable
```

---

## 2. Compiler Frontend and C11 Code Generation (`selfhost/`)

### Lexer & Text Interpolation (`selfhost/lexer.thrum`, `selfhost/parser.thrum`)

- **Significant indentation:** Emits explicit `INDENT`/`DEDENT` tokens using spaces only; tabs in leading indentation fail immediately. Newlines inside `()`, `[]`, and `{}` are suppressed.
- **Chunked O(1) append buffers (`util.thrum`):** Because Thrum lists use Copy-on-Write semantics, building large compiler lists (tokens, C output lines, strings) by repeated `push()` would be quadratic ($O(N^2)$). `util.thrum` implements a 3-tier chunked buffer (`buf_new`, `buf_push`, `buf_join`) keeping compilation linear.
- **Sub-lexed interpolation:** Inside non-raw string literals, `{expr}` spans (supporting nested braces and quoted strings) are isolated by the parser and re-lexed/parsed via `lx.lex(inner)` + `_parse_tokens(..., true)`, lowering directly to `thrum_interp_text(rt, nparts, parts)`.

### C11 Lowering (`selfhost/codegen_c.thrum`)

Generated C targets C11 cleanly with `-Wall -Wextra -Werror` and maps Thrum constructs to runtime helpers:

- **Functions & closures:** Every `fn` and `\it` lambda compiles to a C function taking `(TRuntime* rt, TEnv* env, int argc, TVal* argv)` bundled inside a `TFn` closure object capturing its lexical `TEnv*`. Runtime calls go through `thrum_call()` which enforces arity, parameter types/dimensions, `requires` contracts, recursion depth (`<= 400`), `ensures` postconditions (binding `result`), and return dimensions.
- **Reactive thunks:** Every top-level / `live` / `settle` `let` expression—and every `on` guard/body—is lifted into a C helper thunk `(TRuntime* rt, TEnv* env) -> TVal` plus its source string for `why` provenance.
- **Structured temps & GC safety:** Every intermediate expression temporary binds `T_VAR(rt, _t_N, ...)` which pushes its address onto `rt->root_ptrs` so allocations occurring mid-expression can never lose live intermediates to a GC cycle. Frame roots restore automatically via `T_FRAME(rt)` / `T_END_FRAME(rt)` (and `try` stack unwind restoration).
- **Bundled file modules:** Transitive `use "./foo.thrum"` / `use std.physics` files are compiled into `_thrum_init_N(rt)` functions inside the same single `.c` output. Before running an initializer, `thrum_reg_loading()` stores a sentinel in `rt->modules`; re-entering a module currently loading raises `circular module import` at runtime.

---

## 3. Three-Stage Fixed-Point Bootstrap

Bootstrapping proves that `thrumc` compiles itself faithfully without divergence:

1. **Stage 0 (`python3 thrum.py run selfhost/thrumc.thrum`):** Runs `selfhost/` in the Python reference interpreter to compile `selfhost/thrumc.thrum` into C (`s1.c`), compiled by `cc` into `thrumc1`. *(Note: minor formatting differences such as Python float repr vs C `printf("%.15g")` can exist between `s1.c` and `s2.c`.)*
2. **Stage 1 (`thrumc1 emit-c selfhost/thrumc.thrum`):** The native binary compiles itself to `b2.c` (`47,507+` lines), built by `cc` into `thrumc` (`Stage 2`).
3. **Stage 2 (`./thrumc emit-c selfhost/thrumc.thrum`):** Compiles itself again to `b3.c`.
4. **Fixed-point check (`make bootstrap`):** `diff -u b2.c b3.c` exits `0` (**byte-identical**).

---

## 4. Runtime Value Representation & Memory Management (`runtime/rt_core.c`)

### Uniform Tagged Value (`TVal`)

Every value in Thrum is a stack-passed 32-byte value struct `TVal`:

| `TVKind` | Payload (`union`) | Semantics |
| :--- | :--- | :--- |
| `TV_NIL` | &mdash; | Singleton `nil` |
| `TV_BOOL` | `bool b` | `true` / `false` |
| `TV_NUM` | `TQ q` | `double v`, `double u` (`σ`), `TDim d` (7 rational `num/den` exponents), `disp_sc`, `disp_sym` |
| `TV_OBJ` | `TObj* obj` | GC-managed heap object (`OBJ_STR`, `OBJ_LIST`, `OBJ_MAP`, `OBJ_FN`, `OBJ_BUILTIN`, `OBJ_ENV`, `OBJ_CELL`, `OBJ_RECTYPE`, `OBJ_RECVAL`, `OBJ_MOD`, `OBJ_CAP`) |

### Garbage Collector & UTF-8 Strings

- **Mark-Sweep GC:** Allocations increment `rt->num_objs`; when hitting `rt->gc_threshold`, `thrum_gc()` marks reachable graphs from `rt->root_ptrs`, `rt->builtins`, `rt->globals`, `rt->modules`, reactive cells/groups/reactions, and journal entries, sweeps unreachable objects (running `free_state` destructors on closed capabilities), and doubles/adapts threshold dynamically.
- **UTF-8 Codepoints:** `TStr` stores both byte length `len` and cached Unicode codepoint count `ulen`, so `len(s)`, `s[i]`, slices, and regex offsets match Python character indexing while keeping C string interoperability (`\0`-terminated).
- **Exceptions & Stack Unwinding:** `try` pushes a `TTryFrame` recording `jmp_buf`, `root_top`, `track_top`, `frames_top`, `cleanup_top`, `call_depth`, `draining`, `overrides`, `cap_overrides`, and `curgroup`. On `thrum_error()`, if inside `try`, cleanups (`with tty:`, `with gui:`) unwind in LIFO order, reactive/override state is restored atomically, and `longjmp` transfers control to `catch e:`.

---

## 5. Reactive Graph and Fixpoint Engine (`runtime/rt_graph.c`)

- **Dependency Tracking:** Evaluating any `let` thunk pushes a dependency set onto `rt->frames`. Whenever `thrum_read()` touches an upstream `TCell`, that cell is appended to the active frame's tracked dependencies and height (`1 + max(dep->height)`).
- **Purity Enforcement:** Evaluating a reactive cell snapshots `cur_eval_id`. `thrum_assign()` checks if any mutated `TCell` existed before the active relation evaluation began (`cell->born < eval_born`) and aborts with `cannot mutate '...' inside a relation`.
- **Glitch-Free Propagation (`thrum_drain`):** Source mutations (`var` or CoW path updates on lists/maps/records) enqueue direct subscribers sorted by topological `height`. Because `thrum_drain()` always pops the **lowest-height** dirty cell/group next, a diamond node $D = f(B, C)$ where $B, C$ both depend on $A$ never evaluates with stale $C$ and fresh $B$.
- **`settle` Relaxation:** Mutually cyclic declarations inside `settle` share a `TGroup` with a collective topological height. When dirtied, all group cells iterate up to `maxit` rounds until every member satisfies `thrum_relchg(old, new) <= tol`.
- **`on` Cascades:** After propagation settles, registered `TReaction` guards evaluate; matching edge (`rise`/`fall`/`always`) bodies run and may mutate `var`s, re-triggering propagation up to the 1000-round cascade guard.

---

## 6. Capabilities, Journaling, Networking & Cocoa GUI (`runtime/rt_caps.c`, `rt_gui_cocoa.m`, `rt_net_apple.m`)

- **Deterministic Journal:** Every capability input read (`rand.*`, `clock.now/mono`, `fs.read`, `env.get`, `args.all`, `io.ask`, `tty.poll_key/size`, `proc.run`, `net.get/post`) routes through `thrum_journal_get(rt, kind, key, produce, ctx)`. In `--journal` mode, serialized `$q` JSON records `(kind, key, value)`; in `--replay` mode, saved values replay in sequence and verify both capability key/kind parity and identical output streams (`io.say`, `tty.write`, etc.).
- **MT19937 Parity:** `cap_rand` embeds a C11 Mersenne Twister (`init_by_array` seeding + `mt_randbelow` bitmask rejection matching Python's `random.Random` bit-for-bit).
- **Native Cocoa / AppKit 2D Engine:** `runtime/rt_gui_cocoa.m` exposes `ThrumGuiView` (overriding `isFlipped` to `YES` for top-left coordinates matching mouse events), buffers drawing commands (`rect`, `round_rect`, `stroke_rect`, `circle`, `line`, `text` with proportional/monospace fonts, `clip`/`unclip` graphics-state stack) in staging/display `TGuiCmdBuf` arrays inside `gui.begin()`...`gui.commit()`, dispatches mouse, keyboard, and `scrollWheel:` events (`wheel`), provides `CoreText`/AppKit string width metrics via `gui.measure()` and pasteboard access via `gui.clipboard()`/`set_clipboard()`, routes `Cmd+Q`/close cleanly through `ThrumWinDelegate`, and renders 2x Retina PNGs via offscreen `NSBitmapImageRep` (used both interactively and under `THRUM_GUI_HEADLESS=1`).
- **Explicit HTTP/HTTPS Client:** `runtime/rt_net_apple.m` backs `with net:` via ephemeral `NSURLSession` requests (enforcing explicit `http://`/`https://` schemes, returning structured status/headers/body/error records, and linking only when `net` is requested while CLI binaries keep the lightweight `rt_net_stub.o`).

---

[&larr; Standard Library](library.md) | [Documentation Index](index.md) | [Next: CLI &amp; Packaging &rarr;](cli.md)
