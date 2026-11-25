[&larr; Language Reference](reference.md) | [Documentation Index](index.md) | [Next: Internals &rarr;](internals.md)

# The Thrum Standard Library

This manual documents every built-in function, built-in module (`use <name>`), pure-Thrum library (`use std.<name>`), capability interface (`with <cap>:`), and registered physical unit in **Thrum v0.2**.

---

## 1. Built-in Functions

Installed unconditionally in every Thrum scope:

### 1.1 Types, Formatting & Errors

- **`type(x) -> text`**: Returns runtime type name (`"nil"`, `"bool"`, `"num"`, `"text"`, `"list"`, `"map"`, `"fn"`, `"record"`, `"module"`, `"cap"`, or record name like `"Card"`).
- **`str(x) -> text`**, **`fmt(x) -> text`**: Human-readable representation of `x`.
- **`repr(x) -> text`**: Quoted/unambiguous representation (`"hi"` -> `'"hi"'`).
- **`bool(x) -> bool`**, **`isnil(x) -> bool`**, **`len(x) -> 1`**: Truthiness, `nil` test, and codepoint/element length of texts, lists, and maps.
- **`num(x[, unit]) -> num`**: Converts numeric text (`"42.5"`) or quantity to `num`; with `unit`, converts compatible units (same as `convert`).
- **`fix(x, nd) -> text`**: Formats quantity `x` with fixed decimal places `nd >= 0` in its display unit (`fix(9.80665 m/s^2, 2)` -> `"9.81 m/s^2"`).
- **`fail(msg)`**: Raises a catchable runtime `ThrumError` with message `str(msg)`.

### 1.2 Quantities, Units & Uncertainty

- **`qty(val, unit[, sigma]) -> num`**: Constructs a quantity from scalar magnitude, unit string, and optional uncertainty (`qty(5, "km", 0.1)` -> `5.0 ± 0.1 km`).
- **`convert(x, unit) -> num`**: Converts `x` to compatible unit string (equivalent to `x as unit`).
- **`bare(x) -> 1`**: Strips unit and uncertainty, returning the raw SI base value (`bare(2 km)` -> `2000`).
- **`unit(x) -> text`**: Returns display unit symbol (`"km"`) or canonical dimension string (`"m"` / `"1"`).
- **`dim(x) -> text`**: Returns canonical SI dimension string (`"kg*m^2/s^2"`, or `"1"` if dimensionless).
- **`unc(x) -> num`**: Returns uncertainty $\sigma$ as an exact quantity in `x`'s dimension and display unit.
- **`si(x) -> num`**: Strips custom display unit scaling while retaining SI dimension and $\sigma$.
- **`exact(x) -> num`**: Drops uncertainty ($\sigma \leftarrow 0$), retaining value, dimension, and display unit.
- **`with_unc(x, sigma) -> num`**: Attaches uncertainty (`sigma` in `x`'s dimension, or dimensionless interpreted in `x`'s display unit).
- **`c2k(celsius) -> K`**, **`k2c(kelvin) -> 1`**: Explicit affine Celsius $\leftrightarrow$ Kelvin offset conversions.
- **`approx(a, b[, k=3]) -> bool`**: True when dimensions match and $|a - b| \le \max(k\sqrt{\sigma_a^2+\sigma_b^2}, 10^{-9}\max(|a|,|b|))$.

### 1.3 Math & Statistics Builtins

- **`abs(x)`**, **`sqrt(x)`**, **`sign(x)`**, **`floor(x)`**, **`ceil(x)`**, **`round(x[, nd=0])`**: Preserve/scale units accurately (`floor`/`ceil`/`round` operate on display unit magnitude; `sqrt(9 m^2)` -> `3 m`).
- **`clamp(x, lo, hi)`**, **`lerp(a, b, t)`**: Clamp `x` to `[lo, hi]`; linear interpolation `a + (b - a) * t`.
- **`min(xs)`**, **`max(xs)`**: Minimum / maximum of a non-empty list of compatible numbers or texts.
- **`sum(xs)`**, **`product(xs)`**, **`mean(xs)`**, **`median(xs)`**, **`stdev(xs)`**: Dimension-safe aggregations (`stdev` uses sample $N-1$ Bessel correction).

### 1.4 Lists, Maps, Higher-Order & Text Builtins

- **Ranges & Construction:** `range([start=0,] stop[, step=1])`, `list(x)` (chars of text or `[k, v]` pairs of map).
- **Sequences:** `push(xs, v)`, `pop(xs)`, `concat(a, b)`, `slice(xs, start[, stop])`, `reverse(xs)`, `sort(xs[, key_fn])`, `uniq(xs)`, `flatten(xs)`, `zip(a, b)`, `enumerate(xs)`.
- **Functional:** `map(xs, f)`, `filter(xs, f)`, `find(xs, f)`, `count(xs, f)`, `reduce(xs, f, init)`, `any(xs[, f])`, `all(xs[, f])`.
- **Maps:** `keys(m)`, `vals(m)`, `items(m)`.
- **Text:** `split(s[, sep])`, `join(sep, xs)`, `upper(s)`, `lower(s)`, `trim(s)`, `replace(s, old, new)`, `starts(s, pre)`, `ends(s, suf)`.

---

## 2. Built-in Modules (`use <name>`)

### `math`

- **Constants:** `pi`, `tau`, `e`, `phi`, `inf`, `nan`.
- **Trig & Hyperbolic (dimensionless):** `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y, x)`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`, `deg2rad(d)`, `rad2deg(r)`.
- **Exp & Log:** `exp`, `expm1`, `ln`, `log1p`, `log10`, `log2`, `log(x, base)`, `hypot(a, b)` (dimension-safe!).
- **Classification:** `isfinite(x)`, `isnan(x)`, `isinf(x)`.
- **Discrete & Vector:** `gcd(a, b)`, `lcm(a, b)`, `factorial(n)`, `comb(n, k)`, `perm(n, k)`, `is_prime(n)`, `factors(n)`, `linspace(a, b, n)`, `arange(start, stop[, step])`, `dot(a, b)`, `norm(xs)`, `cumsum(xs)`, `diff(xs)`, `clamp(x, lo, hi)`.

### `const` (CODATA Physical & Astronomical Constants)

Exact and uncertain physical constants carrying full SI units and $\sigma$:
`c`, `h`, `hbar`, `kB`, `NA`, `e`, `G`, `eps0`, `mu0`, `me`, `mp`, `mn`, `u`, `alpha`, `Ry`, `F`, `muB`, `R`, `g0`, `atm`, `sigma`, `au`, `ly`, `pc`, `M_sun`, `M_earth`, `R_earth`, `L_sun`, `T_cmb`.

### `stat`

`mean(xs)`, `stdev(xs)`, `median(xs)`, `variance(xs)`, `mode(xs)`, `quantile(xs, q)`, `iqr(xs)`, `cov(xs, ys)`, `corr(xs, ys)`, `linfit(xs, ys)` (returns `{slope, intercept, r2, n}`), `weighted_mean(pairs)` (`[[val, unc], ...]` inverse-variance weighted!), `zscore(xs)`, `normalize(xs)`, `histogram(xs[, bins=10])` (list of `{low, high, count}`).

### `units`

`define(name, scale, base_unit_str)`, `list()`, `dim_of(q)`, `compatible(a, b)`, `convert`, `qty`, `c2k`, `k2c`, `f2k(f)`, `k2f(k)`.

### `text`

`split`, `join`, `upper`, `lower`, `title(s)`, `trim`, `ltrim(s)`, `rtrim(s)`, `replace`, `starts`, `ends`, `contains(s, sub)`, `index_of(s, sub)` (`-1` if missing), `count(s, sub)`, `pad_left(s, w[, ch=" "])`, `pad_right(s, w[, ch=" "])`, `repeat(s, n)`, `chars(s)`, `lines(s)`, `words(s)`, `is_digit(s)`, `is_alpha(s)`, `format(tmpl, args...)` (`{0}`, `{1}` or `{}`).

### `regex`

POSIX Extended Regular Expressions (`\d`, `\w`, `\s` supported cleanly):
`test(pat, s) -> bool`, `match(pat, s) -> map|nil` (anchored start), `search(pat, s) -> map|nil`, `find_all(pat, s) -> list`, `replace(pat, rep, s) -> text`, `split(pat, s) -> list`. Match maps have `{text, start, end, groups}` using codepoint offsets.

### `json`

`parse(s)`, `stringify(v[, indent])`, `parse_qty(s)` / `stringify_qty(v[, indent])` (round-trips physical quantities & uncertainties via `{"$q": v, "d": ..., "u": ..., "unit": ...}` encodings).

### `csv`

`parse(s[, header=true])`, `parse_nums(s[, header=true])` (auto-parses numeric cells), `stringify(rows[, cols])`.

### `time`

`parse_iso(s) -> s` (Unix timestamp in seconds `s`), `format_iso(t_s) -> text` (UTC ISO-8601), `format_dur(dur_s) -> text`, `components(t_s) -> {year, month, day, hour, min, sec, weekday}`, `from_components(y, m, d[, h, min, s]) -> s`.

### `path`

`join(parts...)`, `dirname(p)`, `basename(p)`, `stem(p)`, `ext(p)`, `normalize(p)`, `split(p)`.

### `list` & `map`

- **`list`:** `chunk(xs, n)`, `window(xs, n)`, `take(xs, n)`, `drop(xs, n)`, `partition(xs, pred)`, `group_by(xs, key_fn)`, `sort_by(xs, key_fn)`, `flat_map(xs, fn)`.
- **`map`:** `get(m, k[, default])`, `has(m, k)`, `merge(a, b)`, `pick(m, ks)`, `omit(m, ks)`, `map_vals(m, fn)`, `filter_entries(m, pred)`, `from_entries(pairs)`.

### `test`

`eq(a, b[, msg])`, `neq(a, b[, msg])`, `approx(a, b[, msg])`, `ok(cond[, msg])`, `fails(fn()[, substr])`, `run(name, fn())`, `summary() -> {passed, failed, total, failures}`.

---

## 3. Pure-Thrum Standard Library (`use std.<name>`)

- **`std.vec`:** `record Vec2 {x, y}`, `record Vec3 {x, y, z}`, `vec2`, `vec3`, `add2`, `sub2`, `scale2`, `dot2`, `mag2`, `norm2`, `dist2`, `add3`, `sub3`, `scale3`, `dot3`, `cross3`, `mag3`, `norm3`, `dist3`.
- **`std.physics`:** `ke(m, v)`, `pe_grav(m, h[, g])`, `momentum(m, v)`, `grav_force(m1, m2, r)`, `escape_vel(m, r)`, `orbital_period(m, r)`, `ideal_gas_p(n, T, V)`, `ohms_i(V, R)`, `power_vi(V, I)`, `photon_energy(freq)`, `de_broglie(m, v)`, `pendulum_period(L[, g])`, `lorentz_gamma(v)`.
- **`std.signal`:** `clamp01(x)`, `deadband(x, w)`, `ema(prev, x, alpha)`, `sma(xs)`, `lowpass(prev, target, dt, tau)`, `pid_step(state, sp, meas, dt, gains)`, `rmse(pred, actual)`, `mae(pred, actual)`.
- **`std.finance`:** `compound(p, r, n)`, `continuous(p, r, t)`, `npv(r, cfs)`, `pmt(p, r, n)`, `cagr(v0, v1, n)`, `sharpe(rets[, rf=0])`.
- **`std.matrix`:** `zeros(r, c)`, `eye(n)`, `shape(m)`, `transpose(m)`, `add(a, b)`, `scale(m, s)`, `matmul(a, b)`, `matvec(m, v)`, `trace(m)`, `det2(m)`, `solve2(m, b)`.

---

## 4. Object Capabilities (`with <cap>:`)

| Capability | Methods | Purpose & Sandboxing |
| :--- | :--- | :--- |
| `io` | `say(...)`, `print(...)`, `ask([prompt])`, `warn(...)`, `err(...)`, `table(rows[, headers])` | Standard output/error streams and tabular display |
| `clock` | `now() -> s`, `mono() -> s`, `sleep(dur: s)` | Wall/monotonic clocks and sleeping (journaled) |
| `rand([seed])` | `float()`, `int(lo, hi)`, `pick(xs)`, `shuffle(xs)`, `sample(xs, k)`, `gauss(mu, sig)` | Deterministic MT19937 PRNG |
| `fs([root])` | `read`, `read_lines`, `write`, `write_lines`, `append`, `exists`, `is_file`, `is_dir`, `list([p])`, `mkdir`, `remove` | Strictly jailed inside `root` (defaults to `.`; path traversal errors) |
| `env` | `get(k[, def])`, `has(k)`, `keys()` | Read-only environment variables (journaled) |
| `args` | `all()`, `get(i[, def])`, `len()` | CLI program arguments |
| `tty` | `is_tty()`, `enter_raw()`, `exit_raw()`, `hide_cursor()`, `show_cursor()`, `clear()`, `write(...)`, `size()`, `poll_key([dt])` | Raw non-blocking terminal TUI with automatic restore cleanup |
| `proc` | `run(argv[, cwd])`, `exe()`, `cwd()`, `exit([code])` | Subprocess invocation returning `{code, stdout, stderr}` |
| `gui` | `open(title, w, h)`, `is_open()`, `close()`, `size()`, `poll()`, `wait(ms)`, `begin()`, `clear(r,g,b)`, `rect(...)`, `round_rect(...)`, `stroke_rect(...)`, `circle(...)`, `line(...)`, `text(..., [align, [bold, [a, [mono]]]])`, `clip(x,y,w,h)`, `unclip()`, `measure(txt, sz[, bold[, mono]])`, `clipboard()`, `set_clipboard(s)`, `commit()`, `screenshot(path)` | Native macOS Cocoa/AppKit 2D canvas window (`rt_gui_cocoa.m`) with clipping, monospace metrics, wheel scroll & clipboard |
| `net` | `get(url[, headers])`, `post(url, body[, headers])` | Explicit `http://` and `https://` client (`NSURLSession` / `rt_net_apple.m`), returning `{ok, status, url, content_type, headers, body, error}` with replay journaling |

---

## 5. Built-in Units Registry

Run `./thrumc units` to query exact scales interactively:

- **SI Base (with metric prefixes `y..Y`):** `m`, `g` (`kg`), `s`, `A`, `K`, `mol`, `cd`.
- **SI Derived (with prefixes):** `N`, `Pa`, `J`, `W`, `C`, `V`, `F`, `H`, `S`, `Hz`, `Wb`, `T`, `lm`, `lx`, `Bq`, `Gy`, `Sv`, `ohm` (`Ω`).
- **Time:** `min`, `h`/`hr`, `day`, `yr`.
- **Length, Mass, Volume & Energy:** `angstrom` (`Å`), `au`, `ly`, `pc`, `in`/`inch`, `ft`/`foot`, `yd`, `mi`, `t` (tonne), `lb`, `oz`, `L` (liter), `gal`, `eV`, `cal`, `bar`, `atm`, `psi`, `mph`, `knot`.
- **Dimensionless & Information:** `rad`, `deg` (`°`), `sr`, `pct` (`%` name), `ppm`, `ppb`, `count`, `bit`, `byte`.

---

[&larr; Language Reference](reference.md) | [Documentation Index](index.md) | [Next: Internals &rarr;](internals.md)
