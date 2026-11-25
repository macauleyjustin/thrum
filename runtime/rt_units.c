#include "thrum_rt.h"
#include <ctype.h>

typedef struct {
    const char* sym;
    double scale;
    TDim dim;
} TUnitEntry;

#define MAX_UNITS 2048
static TUnitEntry g_units[MAX_UNITS];
static int g_nunits = 0;
static bool g_units_init = false;

typedef struct {
    TDim dim;
    const char* sym;
} TDimName;

static TDimName g_dimnames[32];
static int g_ndimnames = 0;
static const char* BASE_NAMES[7] = {"m", "kg", "s", "A", "K", "mol", "cd"};

static int64_t i64_gcd(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b != 0) {
        int64_t t = a % b;
        a = b;
        b = t;
    }
    return a ? a : 1;
}

TFrac thrum_frac(int64_t num, int64_t den) {
    if (den == 0) den = 1;
    if (den < 0) {
        num = -num;
        den = -den;
    }
    if (num == 0) return (TFrac){0, 1};
    int64_t g = i64_gcd(num, den);
    return (TFrac){num / g, den / g};
}

static TFrac fadd(TFrac a, TFrac b) {
    return thrum_frac(a.num * b.den + b.num * a.den, a.den * b.den);
}

static TFrac fsub(TFrac a, TFrac b) {
    return thrum_frac(a.num * b.den - b.num * a.den, a.den * b.den);
}

static TFrac fmul(TFrac a, TFrac b) {
    return thrum_frac(a.num * b.num, a.den * b.den);
}

TFrac thrum_frac_approx(double x, int64_t max_den) {
    /* Continued fraction rational approximation matching Python Fraction.limit_denominator */
    if (!isfinite(x)) return (TFrac){0, 1};
    int sign = x < 0 ? -1 : 1;
    double ax = fabs(x);
    int64_t p0 = 0, q0 = 1, p1 = 1, q1 = 0;
    double r = ax;
    for (int iter = 0; iter < 64; iter++) {
        int64_t a = (int64_t)floor(r);
        int64_t q2 = q0 + a * q1;
        if (q2 > max_den) break;
        int64_t p2 = p0 + a * p1;
        p0 = p1; q0 = q1;
        p1 = p2; q1 = q2;
        double frac = r - (double)a;
        if (frac < 1e-14) break;
        r = 1.0 / frac;
        if (r > 1e15) break;
    }
    if (q1 == 0) return thrum_frac(sign * (int64_t)llround(ax), 1);
    /* Check semiconvergent (k * q1 + q0 <= max_den) */
    int64_t k = (max_den - q0) / q1;
    double err1 = fabs(ax - (double)p1 / (double)q1);
    int64_t ps = p0 + k * p1;
    int64_t qs = q0 + k * q1;
    if (qs > 0) {
        double err2 = fabs(ax - (double)ps / (double)qs);
        if (err2 < err1) return thrum_frac(sign * ps, qs);
    }
    return thrum_frac(sign * p1, q1);
}

TDim thrum_d0(void) {
    TDim d;
    for (int i = 0; i < 7; i++) d.e[i] = (TFrac){0, 1};
    return d;
}

TDim thrum_dim(int m, int kg, int s, int A, int K, int mol, int cd) {
    TDim d;
    d.e[0] = (TFrac){m, 1};
    d.e[1] = (TFrac){kg, 1};
    d.e[2] = (TFrac){s, 1};
    d.e[3] = (TFrac){A, 1};
    d.e[4] = (TFrac){K, 1};
    d.e[5] = (TFrac){mol, 1};
    d.e[6] = (TFrac){cd, 1};
    return d;
}

bool thrum_dimless(TDim d) {
    for (int i = 0; i < 7; i++) {
        if (d.e[i].num != 0) return false;
    }
    return true;
}

bool thrum_dim_eq(TDim a, TDim b) {
    for (int i = 0; i < 7; i++) {
        if (a.e[i].num != b.e[i].num || a.e[i].den != b.e[i].den) return false;
    }
    return true;
}

TDim thrum_dadd(TDim a, TDim b) {
    TDim r;
    for (int i = 0; i < 7; i++) r.e[i] = fadd(a.e[i], b.e[i]);
    return r;
}

TDim thrum_dsub(TDim a, TDim b) {
    TDim r;
    for (int i = 0; i < 7; i++) r.e[i] = fsub(a.e[i], b.e[i]);
    return r;
}

TDim thrum_dscale(TDim a, TFrac n) {
    TDim r;
    for (int i = 0; i < 7; i++) r.e[i] = fmul(a.e[i], n);
    return r;
}

static const char* cstr_dup_perm(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static void reg_u(const char* sym, double scale, TDim dim) {
    for (int i = 0; i < g_nunits; i++) {
        if (strcmp(g_units[i].sym, sym) == 0) {
            g_units[i].scale = scale;
            g_units[i].dim = dim;
            return;
        }
    }
    if (g_nunits < MAX_UNITS) {
        g_units[g_nunits].sym = cstr_dup_perm(sym);
        g_units[g_nunits].scale = scale;
        g_units[g_nunits].dim = dim;
        g_nunits++;
    }
}

static const char* g_pref_list[32];
static int g_npref = 0;

static void add_u(const char* sym, double sc, TDim d, bool pref, const char* const* aliases, int naliases) {
    reg_u(sym, sc, d);
    for (int i = 0; i < naliases; i++) reg_u(aliases[i], sc, d);
    if (pref && g_npref < 32) g_pref_list[g_npref++] = sym;
}

static void add_dimname(TDim d, const char* sym) {
    for (int i = 0; i < g_ndimnames; i++) {
        if (thrum_dim_eq(g_dimnames[i].dim, d)) return;
    }
    if (g_ndimnames < 32) {
        g_dimnames[g_ndimnames].dim = d;
        g_dimnames[g_ndimnames].sym = sym;
        g_ndimnames++;
    }
}

static const char* lookup_dimname(TDim d) {
    for (int i = 0; i < g_ndimnames; i++) {
        if (thrum_dim_eq(g_dimnames[i].dim, d)) return g_dimnames[i].sym;
    }
    return NULL;
}

static void init_units(void) {
    if (g_units_init) return;
    g_units_init = true;
    TDim d0 = thrum_d0();
    #define AU(sym, sc, dm, pf, ...) do { \
        const char* _al[] = {__VA_ARGS__}; \
        add_u(sym, sc, dm, pf, _al, (int)(sizeof(_al)/sizeof(_al[0]))); \
    } while(0)
    #define AU0(sym, sc, dm, pf) add_u(sym, sc, dm, pf, NULL, 0)

    AU("m", 1.0, thrum_dim(1,0,0,0,0,0,0), true, "meter", "meters", "metre", "metres");
    AU("g", 1e-3, thrum_dim(0,1,0,0,0,0,0), true, "gram", "grams");
    AU("s", 1.0, thrum_dim(0,0,1,0,0,0,0), true, "sec", "secs", "second", "seconds");
    AU("A", 1.0, thrum_dim(0,0,0,1,0,0,0), true, "amp", "amps", "ampere", "amperes");
    AU("K", 1.0, thrum_dim(0,0,0,0,1,0,0), true, "kelvin");
    AU("mol", 1.0, thrum_dim(0,0,0,0,0,1,0), true, "mole", "moles");
    AU("cd", 1.0, thrum_dim(0,0,0,0,0,0,1), true, "candela");

    AU("N", 1.0, thrum_dim(1,1,-2,0,0,0,0), true, "newton", "newtons");
    AU("Pa", 1.0, thrum_dim(-1,1,-2,0,0,0,0), true, "pascal", "pascals");
    AU("J", 1.0, thrum_dim(2,1,-2,0,0,0,0), true, "joule", "joules");
    AU("W", 1.0, thrum_dim(2,1,-3,0,0,0,0), true, "watt", "watts");
    AU("C", 1.0, thrum_dim(0,0,1,1,0,0,0), true, "coulomb", "coulombs");
    AU("V", 1.0, thrum_dim(2,1,-3,-1,0,0,0), true, "volt", "volts");
    AU("F", 1.0, thrum_dim(-2,-1,4,2,0,0,0), true, "farad", "farads");
    AU("H", 1.0, thrum_dim(2,1,-2,-2,0,0,0), true, "henry", "henries");
    AU("S", 1.0, thrum_dim(-2,-1,3,2,0,0,0), true, "siemens");
    AU("Hz", 1.0, thrum_dim(0,0,-1,0,0,0,0), true, "hertz");
    AU("Wb", 1.0, thrum_dim(2,1,-2,-1,0,0,0), true, "weber", "webers");
    AU("T", 1.0, thrum_dim(0,1,-2,-1,0,0,0), true, "tesla", "teslas");
    AU("lm", 1.0, thrum_dim(0,0,0,0,0,0,1), true, "lumen", "lumens");
    AU("lx", 1.0, thrum_dim(-2,0,0,0,0,0,1), true, "lux");
    AU0("Bq", 1.0, thrum_dim(0,0,-1,0,0,0,0), true);
    AU0("Gy", 1.0, thrum_dim(2,0,-2,0,0,0,0), true);
    AU0("Sv", 1.0, thrum_dim(2,0,-2,0,0,0,0), true);
    AU("ohm", 1.0, thrum_dim(2,1,-3,-2,0,0,0), true, "\xCE\xA9", "ohms");

    AU("min", 60.0, thrum_dim(0,0,1,0,0,0,0), false, "minute", "minutes");
    AU("h", 3600.0, thrum_dim(0,0,1,0,0,0,0), false, "hr", "hrs", "hour", "hours");
    AU("day", 86400.0, thrum_dim(0,0,1,0,0,0,0), false, "days");
    AU("yr", 31557600.0, thrum_dim(0,0,1,0,0,0,0), false, "year", "years");
    AU("L", 1e-3, thrum_dim(3,0,0,0,0,0,0), true, "liter", "liters", "litre", "litres");
    AU("t", 1000.0, thrum_dim(0,1,0,0,0,0,0), true, "tonne", "tonnes");
    AU0("eV", 1.602176634e-19, thrum_dim(2,1,-2,0,0,0,0), true);
    AU0("cal", 4.184, thrum_dim(2,1,-2,0,0,0,0), true);
    AU0("bar", 1e5, thrum_dim(-1,1,-2,0,0,0,0), true);
    AU0("atm", 101325.0, thrum_dim(-1,1,-2,0,0,0,0), false);
    AU("angstrom", 1e-10, thrum_dim(1,0,0,0,0,0,0), false, "\xC3\x85");
    AU0("au", 1.495978707e11, thrum_dim(1,0,0,0,0,0,0), false);
    AU0("ly", 9.4607304725808e15, thrum_dim(1,0,0,0,0,0,0), false);
    AU("pc", 3.085677581491367e16, thrum_dim(1,0,0,0,0,0,0), false, "parsec", "parsecs");
    AU("in", 0.0254, thrum_dim(1,0,0,0,0,0,0), false, "inch", "inches");
    AU("ft", 0.3048, thrum_dim(1,0,0,0,0,0,0), false, "foot", "feet");
    AU("yd", 0.9144, thrum_dim(1,0,0,0,0,0,0), false, "yard", "yards");
    AU("mi", 1609.344, thrum_dim(1,0,0,0,0,0,0), false, "mile", "miles");
    AU("lb", 0.45359237, thrum_dim(0,1,0,0,0,0,0), false, "lbs", "pound", "pounds");
    AU("oz", 0.028349523125, thrum_dim(0,1,0,0,0,0,0), false, "ounce", "ounces");
    AU0("psi", 6894.757293168, thrum_dim(-1,1,-2,0,0,0,0), false);
    AU0("mph", 0.44704, thrum_dim(1,0,-1,0,0,0,0), false);
    AU("knot", 0.5144444444444445, thrum_dim(1,0,-1,0,0,0,0), false, "knots");
    AU("gal", 3.785411784e-3, thrum_dim(3,0,0,0,0,0,0), false, "gallon", "gallons");
    AU("rad", 1.0, d0, true, "radian", "radians");
    AU("deg", M_PI / 180.0, d0, false, "\xC2\xB0", "degree", "degrees");
    AU0("sr", 1.0, d0, true);
    AU("pct", 0.01, d0, false, "percent");
    AU0("ppm", 1e-6, d0, false);
    AU0("ppb", 1e-9, d0, false);
    AU0("count", 1.0, d0, false);
    AU("bit", 1.0, d0, false, "bits");
    AU("byte", 8.0, d0, false, "bytes");
    #undef AU
    #undef AU0

    static const struct { const char* p; double f; } prefs[] = {
        {"Q",1e30},{"R",1e27},{"Y",1e24},{"Z",1e21},{"E",1e18},{"P",1e15},
        {"T",1e12},{"G",1e9},{"M",1e6},{"k",1e3},{"h",1e2},{"da",1e1},
        {"d",1e-1},{"c",1e-2},{"m",1e-3},{"u",1e-6},{"\xC2\xB5",1e-6},{"n",1e-9},
        {"p",1e-12},{"f",1e-15},{"a",1e-18},{"z",1e-21},{"y",1e-24},
        {"r",1e-27},{"q",1e-30}
    };
    int nprefs = (int)(sizeof(prefs) / sizeof(prefs[0]));
    for (int pi = 0; pi < nprefs; pi++) {
        for (int si = 0; si < g_npref; si++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s%s", prefs[pi].p, g_pref_list[si]);
            double sc; TDim dd;
            if (!thrum_unit_lookup(buf, &sc, &dd)) {
                thrum_unit_lookup(g_pref_list[si], &sc, &dd);
                reg_u(buf, sc * prefs[pi].f, dd);
            }
        }
    }

    const char* dn_syms[] = {
        "Hz","N","Pa","J","W","C","V","F","H","S","Wb","T","lm","lx","Bq","Gy","Sv","ohm",
        "m","kg","s","A","K","mol","cd"
    };
    for (int i = 0; i < (int)(sizeof(dn_syms)/sizeof(dn_syms[0])); i++) {
        double sc; TDim dd;
        if (thrum_unit_lookup(dn_syms[i], &sc, &dd) && sc == 1.0 && !thrum_dimless(dd)) {
            add_dimname(dd, dn_syms[i]);
        }
    }
}

bool thrum_unit_lookup(const char* sym, double* out_sc, TDim* out_d) {
    init_units();
    for (int i = 0; i < g_nunits; i++) {
        if (strcmp(g_units[i].sym, sym) == 0) {
            if (out_sc) *out_sc = g_units[i].scale;
            if (out_d) *out_d = g_units[i].dim;
            return true;
        }
    }
    return false;
}

void thrum_unit_define(TRuntime* rt, const char* sym, double sc, TDim d) {
    (void)rt;
    init_units();
    reg_u(sym, sc, d);
}

TVal thrum_units_list(TRuntime* rt) {
    init_units();
    T_FRAME(rt);
    T_VAR(rt, lst, thrum_list(rt, (size_t)g_nunits));
    TList* L = (TList*)lst.as.obj;
    for (int i = 0; i < g_nunits; i++) {
        TVal s = thrum_cstr(rt, g_units[i].sym);
        thrum_list_push(rt, L, s);
    }
    T_END_FRAME(rt);
    return lst;
}

static void fmt_exp(TFrac e, char* buf, size_t sz) {
    if (e.den == 1) snprintf(buf, sz, "%lld", (long long)e.num);
    else snprintf(buf, sz, "%lld/%lld", (long long)e.num, (long long)e.den);
}

void thrum_dimstr(TDim d, char* buf, size_t bufsz) {
    init_units();
    if (thrum_dimless(d)) {
        buf[0] = '\0';
        return;
    }
    const char* dn = lookup_dimname(d);
    if (dn) {
        snprintf(buf, bufsz, "%s", dn);
        return;
    }
    char pos[128] = "";
    char neg[128] = "";
    int nneg = 0;
    for (int i = 0; i < 7; i++) {
        if (d.e[i].num > 0) {
            char part[40], eb[24];
            if (d.e[i].num == 1 && d.e[i].den == 1) {
                snprintf(part, sizeof(part), "%s", BASE_NAMES[i]);
            } else {
                fmt_exp(d.e[i], eb, sizeof(eb));
                snprintf(part, sizeof(part), "%s^%s", BASE_NAMES[i], eb);
            }
            if (pos[0]) strncat(pos, "*", sizeof(pos) - strlen(pos) - 1);
            strncat(pos, part, sizeof(pos) - strlen(pos) - 1);
        } else if (d.e[i].num < 0) {
            char part[40], eb[24];
            TFrac pe = (TFrac){-d.e[i].num, d.e[i].den};
            if (pe.num == 1 && pe.den == 1) {
                snprintf(part, sizeof(part), "%s", BASE_NAMES[i]);
            } else {
                fmt_exp(pe, eb, sizeof(eb));
                snprintf(part, sizeof(part), "%s^%s", BASE_NAMES[i], eb);
            }
            if (neg[0]) strncat(neg, "*", sizeof(neg) - strlen(neg) - 1);
            strncat(neg, part, sizeof(neg) - strlen(neg) - 1);
            nneg++;
        }
    }
    if (!pos[0]) strcpy(pos, "1");
    if (!nneg) snprintf(buf, bufsz, "%s", pos);
    else if (nneg == 1) snprintf(buf, bufsz, "%s/%s", pos, neg);
    else snprintf(buf, bufsz, "%s/(%s)", pos, neg);
}

static bool is_ustart(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
}
static bool is_ucont(unsigned char c) {
    return is_ustart(c) || (c >= '0' && c <= '9');
}

static bool scan_expr(const char* s, int j, int n, double* out_sc, TDim* out_d, int* out_k);

static bool scan_term(const char* s, int j, int n, double* out_sc, TDim* out_d, int* out_k) {
    if (j < n && s[j] == '(') {
        double sc; TDim d; int k;
        if (scan_expr(s, j + 1, n, &sc, &d, &k) && k < n && s[k] == ')') {
            *out_sc = sc; *out_d = d; *out_k = k + 1;
            return true;
        }
        return false;
    }
    if (j < n && s[j] == '1') {
        if (j + 1 >= n || !(isdigit((unsigned char)s[j + 1]) || s[j + 1] == '_' || isalpha((unsigned char)s[j + 1]) || s[j + 1] == '.')) {
            *out_sc = 1.0; *out_d = thrum_d0(); *out_k = j + 1;
            return true;
        }
        return false;
    }
    if (j >= n || !is_ustart((unsigned char)s[j])) return false;
    int k = j;
    while (k < n && is_ucont((unsigned char)s[k])) k++;
    int len = k - j;
    if (len <= 0 || len >= 64) return false;
    char sym[64];
    memcpy(sym, s + j, (size_t)len);
    sym[len] = '\0';
    double sc; TDim d;
    if (!thrum_unit_lookup(sym, &sc, &d)) return false;
    if (k < n && s[k] == '^') {
        int p = k + 1;
        bool parens = false;
        if (p < n && s[p] == '(') { parens = true; p++; }
        int sign = 1;
        if (p < n && (s[p] == '+' || s[p] == '-')) {
            if (s[p] == '-') sign = -1;
            p++;
        }
        if (p < n && isdigit((unsigned char)s[p])) {
            int64_t num = 0, den = 1;
            while (p < n && isdigit((unsigned char)s[p])) { num = num * 10 + (s[p] - '0'); p++; }
            if (p + 1 < n && s[p] == '/' && isdigit((unsigned char)s[p + 1])) {
                p++;
                den = 0;
                while (p < n && isdigit((unsigned char)s[p])) { den = den * 10 + (s[p] - '0'); p++; }
                if (den == 0) den = 1;
            }
            if (!parens || (p < n && s[p] == ')')) {
                if (parens) p++;
                TFrac e = thrum_frac(sign * num, den);
                sc = pow(sc, (double)e.num / (double)e.den);
                d = thrum_dscale(d, e);
                k = p;
            }
        }
    }
    *out_sc = sc; *out_d = d; *out_k = k;
    return true;
}

static bool scan_expr(const char* s, int j, int n, double* out_sc, TDim* out_d, int* out_k) {
    double sc; TDim d; int k;
    if (!scan_term(s, j, n, &sc, &d, &k)) return false;
    while (k < n) {
        char op = 0;
        int next = k + 1;
        if (s[k] == '*' || s[k] == '/') op = s[k];
        else if ((unsigned char)s[k] == 0xC2 && k + 1 < n && (unsigned char)s[k + 1] == 0xB7) {
            op = '*';
            next = k + 2;
        }
        if (!op) break;
        double sc2; TDim d2; int k2;
        if (!scan_term(s, next, n, &sc2, &d2, &k2)) break;
        if (op == '/') { sc /= sc2; d = thrum_dsub(d, d2); }
        else { sc *= sc2; d = thrum_dadd(d, d2); }
        k = k2;
    }
    *out_sc = sc; *out_d = d; *out_k = k;
    return true;
}

bool thrum_scan_unit(const char* s, int i, double* out_sc, TDim* out_d, int* out_end) {
    init_units();
    int n = (int)strlen(s);
    return scan_expr(s, i, n, out_sc, out_d, out_end);
}

void thrum_parse_unit(TRuntime* rt, const char* text, int line, int col, double* out_sc, TDim* out_d) {
    init_units();
    while (*text == ' ' || *text == '\t') text++;
    int n = (int)strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\t')) n--;
    double sc; TDim d; int end;
    if (n <= 0 || !scan_expr(text, 0, n, &sc, &d, &end) || end != n) {
        char msg[160];
        snprintf(msg, sizeof(msg), "unknown or malformed unit '%s'", text);
        thrum_error(rt, line, col, msg, NULL);
    }
    *out_sc = sc; *out_d = d;
}

/* ---- Formatting ---- */
static void fmt_fix_nd(double v, int nd, char* buf, size_t sz) {
    if (nd >= 0) {
        int prec = nd > 16 ? 16 : nd;
        snprintf(buf, sz, "%.*f", prec, v);
        if (strcmp(buf, "-0") == 0 || strncmp(buf, "-0.", 3) == 0) {
            if (fabs(v) < 0.5 * pow(10.0, -prec)) {
                snprintf(buf, sz, "%.*f", prec, 0.0);
            }
        }
    } else {
        double p = pow(10.0, (double)(-nd));
        double rv = round(v / p) * p;
        if (rv == 0.0) rv = 0.0;
        snprintf(buf, sz, "%.0f", rv);
    }
}

void thrum_qfmt(TQ x, char* buf, size_t bufsz) {
    init_units();
    char dstr[128];
    const char* sym = x.disp_sym;
    double sc = x.disp_sym ? x.disp_sc : 1.0;
    if (!sym) {
        thrum_dimstr(x.d, dstr, sizeof(dstr));
        sym = dstr;
    }
    if (sc == 0.0) sc = 1.0;
    double v = x.v / sc;
    double u = x.u / sc;
    if (v == 0.0) v = 0.0;
    char tail[140] = "";
    if (sym && sym[0]) snprintf(tail, sizeof(tail), " %s", sym);

    if (!isfinite(v)) {
        const char* nf = isnan(v) ? "nan" : (v > 0 ? "inf" : "-inf");
        snprintf(buf, bufsz, "%s%s", nf, tail);
        return;
    }
    if (u > 0.0 && isfinite(u)) {
        int e = (int)floor(log10(fabs(u)));
        int ve = (v != 0.0) ? (int)floor(log10(fabs(v))) : e;
        if (abs(e) > 4 || abs(ve) > 4) {
            int k = ve - e + 1;
            if (k < 0) k = 0;
            if (k > 16) k = 16;
            double scale10 = pow(10.0, (double)ve);
            if (scale10 == 0.0 || !isfinite(scale10)) {
                snprintf(buf, bufsz, "%.12g \xC2\xB1 %.12g%s", v, u, tail);
                return;
            }
            snprintf(buf, bufsz, "(%.*f \xC2\xB1 %.*f)e%+d%s", k, v / scale10, k, u / scale10, ve, tail);
            return;
        }
        int nd = 1 - e;
        char vb[64], ub[64];
        fmt_fix_nd(v, nd, vb, sizeof(vb));
        fmt_fix_nd(u, nd, ub, sizeof(ub));
        snprintf(buf, bufsz, "%s \xC2\xB1 %s%s", vb, ub, tail);
        return;
    }
    if (fabs(v) < 1e15 && v == (double)(int64_t)v) {
        snprintf(buf, bufsz, "%lld%s", (long long)(int64_t)v, tail);
        return;
    }
    snprintf(buf, bufsz, "%.12g%s", v, tail);
}

/* ---- Quantity constructors & arithmetic ---- */
static void wrap_sym(const char* s, char* out, size_t sz) {
    if (strchr(s, '*') || strchr(s, '/')) snprintf(out, sz, "(%s)", s);
    else snprintf(out, sz, "%s", s);
}

static void wrap_pow_sym(const char* s, char* out, size_t sz) {
    if (strchr(s, '*') || strchr(s, '/') || strchr(s, '^')) snprintf(out, sz, "(%s)", s);
    else snprintf(out, sz, "%s", s);
}

TVal thrum_num_dimless(double x) {
    return thrum_qty(NULL, x, 0.0, thrum_d0(), 1.0, NULL);
}

TVal thrum_qty(TRuntime* rt, double v, double u, TDim d, double disp_sc, const char* disp_sym) {
    init_units();
    if (v == 0.0) v = 0.0;
    const char* dn = lookup_dimname(d);
    if (disp_sym != NULL) {
        if (disp_sc == 1.0 && dn) {
            disp_sym = dn;
        } else if (disp_sc == 0.0 || strlen(disp_sym) > 20) {
            disp_sym = NULL;
            disp_sc = 1.0;
        } else if (rt) {
            disp_sym = thrum_intern(rt, disp_sym);
        }
    }
    if (disp_sym == NULL && dn) {
        disp_sym = dn;
        disp_sc = 1.0;
    }
    TVal out;
    out.kind = TV_NUM;
    out.as.q.v = v;
    out.as.q.u = u;
    out.as.q.d = d;
    out.as.q.disp_sc = disp_sc;
    out.as.q.disp_sym = disp_sym;
    return out;
}

TVal thrum_qty_lit(TRuntime* rt, double v, double u, const char* unit_str) {
    if (!unit_str || !unit_str[0]) return thrum_qty(rt, v, u, thrum_d0(), 1.0, NULL);
    double sc = 1.0;
    TDim d = thrum_d0();
    thrum_parse_unit(rt, unit_str, 0, 0, &sc, &d);
    return thrum_qty(rt, v * sc, u * sc, d, sc, unit_str);
}

static void dcomb(TRuntime* rt, TQ a, TQ b, char op, TDim res_dim, double* out_sc, const char** out_sym) {
    *out_sc = 1.0; *out_sym = NULL;
    if (thrum_dimless(res_dim) && !(thrum_dimless(a.d) && thrum_dimless(b.d))) return;
    bool a_dl = thrum_dimless(a.d);
    bool b_dl = thrum_dimless(b.d);
    if (!a.disp_sym && a_dl) {
        if (op == '*') { *out_sc = b.disp_sc; *out_sym = b.disp_sym; return; }
        if (b.disp_sym) {
            char wb[64], buf[80];
            wrap_sym(b.disp_sym, wb, sizeof(wb));
            snprintf(buf, sizeof(buf), "1/%s", wb);
            *out_sc = 1.0 / b.disp_sc;
            *out_sym = thrum_intern(rt, buf);
        }
        return;
    }
    if (!b.disp_sym && b_dl) {
        *out_sc = a.disp_sc; *out_sym = a.disp_sym; return;
    }
    if (a.disp_sym && a_dl && a.disp_sc == 1.0) {
        if (op == '*') { *out_sc = b.disp_sc; *out_sym = b.disp_sym; return; }
        if (b.disp_sym) {
            char wb[64], buf[80];
            wrap_sym(b.disp_sym, wb, sizeof(wb));
            snprintf(buf, sizeof(buf), "1/%s", wb);
            *out_sc = 1.0 / b.disp_sc;
            *out_sym = thrum_intern(rt, buf);
        }
        return;
    }
    if (b.disp_sym && b_dl && b.disp_sc == 1.0) {
        *out_sc = a.disp_sc; *out_sym = a.disp_sym; return;
    }
    if (!a.disp_sym && !b.disp_sym) return;
    for (int i = 0; i < 7; i++) {
        int64_t p = a.d.e[i].num * b.d.e[i].num;
        if ((op == '*' && p < 0) || (op == '/' && p > 0)) return;
    }
    char dsa[64], dsb[64];
    const char* sa = a.disp_sym;
    double sca = a.disp_sym ? a.disp_sc : 1.0;
    if (!sa) {
        thrum_dimstr(a.d, dsa, sizeof(dsa));
        sa = dsa[0] ? dsa : "1";
    }
    const char* sb = b.disp_sym;
    double scb = b.disp_sym ? b.disp_sc : 1.0;
    if (!sb) {
        thrum_dimstr(b.d, dsb, sizeof(dsb));
        sb = dsb[0] ? dsb : "1";
    }
    char wa[80], wb[80], out[180];
    if (op == '*' && strcmp(sa, sb) == 0 && sca == scb) {
        wrap_pow_sym(sa, wa, sizeof(wa));
        snprintf(out, sizeof(out), "%s^2", wa);
        *out_sc = sca * scb;
        *out_sym = thrum_intern(rt, out);
        return;
    }
    wrap_sym(sa, wa, sizeof(wa));
    wrap_sym(sb, wb, sizeof(wb));
    snprintf(out, sizeof(out), "%s%c%s", wa, op, wb);
    *out_sc = (op == '*') ? (sca * scb) : (sca / scb);
    *out_sym = thrum_intern(rt, out);
}

static TVal qadd_internal(TRuntime* rt, TQ a, TQ b, int sign, int line, int col) {
    if (!thrum_dim_eq(a.d, b.d)) {
        char la[64], lb[64], msg[180];
        thrum_dimstr(a.d, la, sizeof(la));
        thrum_dimstr(b.d, lb, sizeof(lb));
        snprintf(msg, sizeof(msg), "dimension mismatch: %s %s %s",
                 la[0] ? la : "dimensionless", sign > 0 ? "+" : "-", lb[0] ? lb : "dimensionless");
        thrum_error(rt, line, col, msg, "convert with 'as', or check your units — Thrum never guesses");
    }
    const char* sym = a.disp_sym ? a.disp_sym : b.disp_sym;
    double sc = a.disp_sym ? a.disp_sc : (b.disp_sym ? b.disp_sc : 1.0);
    return thrum_qty(rt, a.v + sign * b.v, hypot(a.u, b.u), a.d, sc, sym);
}

static TVal qmul_internal(TRuntime* rt, TQ a, TQ b) {
    TDim rd = thrum_dadd(a.d, b.d);
    double sc; const char* sym;
    dcomb(rt, a, b, '*', rd, &sc, &sym);
    return thrum_qty(rt, a.v * b.v, hypot(a.u * b.v, b.u * a.v), rd, sc, sym);
}

static TVal qdiv_internal(TRuntime* rt, TQ a, TQ b, int line, int col) {
    if (b.v == 0.0) thrum_error(rt, line, col, "division by zero", NULL);
    TDim rd = thrum_dsub(a.d, b.d);
    double sc; const char* sym;
    dcomb(rt, a, b, '/', rd, &sc, &sym);
    double u = hypot(a.u / b.v, b.u * a.v / (b.v * b.v));
    return thrum_qty(rt, a.v / b.v, u, rd, sc, sym);
}

TVal thrum_pow(TRuntime* rt, TVal va, TVal vb, int line, int col) {
    TQ a = thrum_need_q(rt, va, line, col);
    TQ b = thrum_need_q(rt, vb, line, col);
    if (!thrum_dimless(b.d)) thrum_error(rt, line, col, "exponent must be dimensionless", NULL);
    if (thrum_dimless(a.d)) {
        double ef = b.v;
        if (a.v < 0.0 && (!isfinite(ef) || ef != floor(ef)))
            thrum_error(rt, line, col, "negative base with fractional exponent", NULL);
        if (a.v == 0.0 && ef < 0.0)
            thrum_error(rt, line, col, "division by zero", NULL);
        double v = pow(a.v, ef);
        double u = (ef == 1.0) ? a.u : (ef == 0.0 ? 0.0 : (a.v != 0.0 ? fabs(ef * v / a.v) * a.u : 0.0));
        if (b.u != 0.0 && a.v > 0.0) u = hypot(u, fabs(v * log(a.v)) * b.u);
        if (a.disp_sym && a.disp_sc != 1.0 && ef == 1.0)
            return thrum_qty(rt, v, u, thrum_d0(), a.disp_sc, a.disp_sym);
        return thrum_qty(rt, v, u, thrum_d0(), 1.0, NULL);
    }
    if (!isfinite(b.v)) thrum_error(rt, line, col, "exponent on dimensioned quantity must be finite rational", NULL);
    TFrac e = thrum_frac_approx(b.v, 1000);
    double ef = (double)e.num / (double)e.den;
    if (fabs(ef - b.v) > 1e-9) thrum_error(rt, line, col, "exponent on dimensioned quantity must be rational", NULL);
    if (a.v < 0.0 && e.den != 1) thrum_error(rt, line, col, "negative base with fractional exponent", NULL);
    if (a.v == 0.0 && e.num < 0) thrum_error(rt, line, col, "division by zero", NULL);
    double v = pow(a.v, ef);
    double u = (e.num == 1 && e.den == 1) ? a.u : (e.num == 0 ? 0.0 : (a.v != 0.0 ? fabs(ef * v / a.v) * a.u : 0.0));
    if (b.u != 0.0 && a.v > 0.0) u = hypot(u, fabs(v * log(a.v)) * b.u);
    double dsc = 1.0;
    const char* dsym = NULL;
    if (a.disp_sym && !(thrum_dimless(a.d) && a.disp_sc == 1.0)) {
        char wp[80], eb[24], out[110];
        wrap_pow_sym(a.disp_sym, wp, sizeof(wp));
        fmt_exp(e, eb, sizeof(eb));
        snprintf(out, sizeof(out), "%s^%s", wp, eb);
        dsc = pow(a.disp_sc, ef);
        dsym = thrum_intern(rt, out);
    }
    return thrum_qty(rt, v, u, thrum_dscale(a.d, e), dsc, dsym);
}

TVal thrum_mod(TRuntime* rt, TVal va, TVal vb, int line, int col) {
    TQ a = thrum_need_q(rt, va, line, col);
    TQ b = thrum_need_q(rt, vb, line, col);
    if (!thrum_dim_eq(a.d, b.d)) thrum_error(rt, line, col, "dimension mismatch in '%'", NULL);
    if (b.v == 0.0) thrum_error(rt, line, col, "modulo by zero", NULL);
    return thrum_qty(rt, fmod(a.v, b.v), a.u, a.d, a.disp_sc, a.disp_sym);
}

TVal thrum_neg(TRuntime* rt, TVal va, int line, int col) {
    if (va.kind != TV_NUM) {
        char msg[80];
        snprintf(msg, sizeof(msg), "cannot negate %s", thrum_tname(va));
        thrum_error(rt, line, col, msg, NULL);
    }
    TQ x = va.as.q;
    TVal out = va;
    out.as.q.v = (x.v == 0.0) ? 0.0 : -x.v;
    return out;
}

bool thrum_cmp(TRuntime* rt, TVal va, TVal vb, int op, int line, int col) {
    if (va.kind == TV_NUM && vb.kind == TV_NUM) {
        if (!thrum_dim_eq(va.as.q.d, vb.as.q.d)) {
            char la[64], lb[64], msg[160];
            thrum_dimstr(va.as.q.d, la, sizeof(la));
            thrum_dimstr(vb.as.q.d, lb, sizeof(lb));
            snprintf(msg, sizeof(msg), "cannot compare %s with %s",
                     la[0] ? la : "dimensionless", lb[0] ? lb : "dimensionless");
            thrum_error(rt, line, col, msg, NULL);
        }
        double x = va.as.q.v, y = vb.as.q.v;
        if (op == 0) return x < y;
        if (op == 1) return x > y;
        if (op == 2) return x <= y;
        return x >= y;
    }
    if (va.kind == TV_OBJ && va.as.obj->kind == OBJ_STR && vb.kind == TV_OBJ && vb.as.obj->kind == OBJ_STR) {
        int c = strcmp(((TStr*)va.as.obj)->data, ((TStr*)vb.as.obj)->data);
        if (op == 0) return c < 0;
        if (op == 1) return c > 0;
        if (op == 2) return c <= 0;
        return c >= 0;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "cannot order %s and %s", thrum_tname(va), thrum_tname(vb));
    thrum_error(rt, line, col, msg, NULL);
}

bool thrum_approx(TVal va, TVal vb, double k) {
    if (va.kind == TV_NUM && vb.kind == TV_NUM) {
        TQ a = va.as.q, b = vb.as.q;
        if (!thrum_dim_eq(a.d, b.d)) return false;
        double m = fmax(fabs(a.v), fabs(b.v));
        double tol = fmax(k * hypot(a.u, b.u), 1e-9 * m);
        return fabs(a.v - b.v) <= tol;
    }
    return thrum_veq(va, vb);
}

TVal thrum_convert(TRuntime* rt, TVal va, const char* unit, int line, int col) {
    TQ x = thrum_need_q(rt, va, line, col);
    double sc; TDim d;
    thrum_parse_unit(rt, unit, line, col, &sc, &d);
    if (!thrum_dim_eq(d, x.d)) {
        char lx[64], msg[160];
        thrum_dimstr(x.d, lx, sizeof(lx));
        snprintf(msg, sizeof(msg), "cannot convert %s to %s", lx[0] ? lx : "dimensionless", unit);
        thrum_error(rt, line, col, msg, NULL);
    }
    TVal out;
    out.kind = TV_NUM;
    out.as.q.v = x.v;
    out.as.q.u = x.u;
    out.as.q.d = x.d;
    out.as.q.disp_sc = sc;
    out.as.q.disp_sym = thrum_intern(rt, unit);
    return out;
}

TVal thrum_add(TRuntime* rt, TVal a, TVal b, int line, int col) {
    if (a.kind == TV_NUM && b.kind == TV_NUM) return qadd_internal(rt, a.as.q, b.as.q, 1, line, col);
    if (a.kind == TV_OBJ && b.kind == TV_OBJ) {
        if (a.as.obj->kind == OBJ_STR && b.as.obj->kind == OBJ_STR) {
            TStr* sa = (TStr*)a.as.obj;
            TStr* sb = (TStr*)b.as.obj;
            size_t tot = sa->len + sb->len;
            char* buf = (char*)malloc(tot + 1);
            memcpy(buf, sa->data, sa->len);
            memcpy(buf + sa->len, sb->data, sb->len);
            buf[tot] = '\0';
            TVal out = thrum_str(rt, buf, tot);
            free(buf);
            return out;
        }
        if (a.as.obj->kind == OBJ_LIST && b.as.obj->kind == OBJ_LIST) {
            TList* la = (TList*)a.as.obj;
            TList* lb = (TList*)b.as.obj;
            T_FRAME(rt);
            T_VAR(rt, va, a);
            T_VAR(rt, vb, b);
            T_VAR(rt, res, thrum_list(rt, la->len + lb->len));
            TList* lr = (TList*)res.as.obj;
            for (size_t i = 0; i < ((TList*)va.as.obj)->len; i++) thrum_list_push(rt, lr, ((TList*)va.as.obj)->items[i]);
            for (size_t i = 0; i < ((TList*)vb.as.obj)->len; i++) thrum_list_push(rt, lr, ((TList*)vb.as.obj)->items[i]);
            T_END_FRAME(rt);
            return res;
        }
        if (a.as.obj->kind == OBJ_MAP && b.as.obj->kind == OBJ_MAP) {
            T_FRAME(rt);
            T_VAR(rt, va, a);
            T_VAR(rt, vb, b);
            T_VAR(rt, res, thrum_map(rt));
            TMap* ma = (TMap*)va.as.obj;
            TMap* mb = (TMap*)vb.as.obj;
            TMap* mr = (TMap*)res.as.obj;
            for (size_t i = 0; i < ma->order_len; i++) {
                TVal val;
                if (thrum_map_get(ma, ma->order_keys[i], &val)) thrum_map_set(rt, mr, ma->order_keys[i], val);
            }
            for (size_t i = 0; i < mb->order_len; i++) {
                TVal val;
                if (thrum_map_get(mb, mb->order_keys[i], &val)) thrum_map_set(rt, mr, mb->order_keys[i], val);
            }
            T_END_FRAME(rt);
            return res;
        }
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "cannot add %s and %s", thrum_tname(a), thrum_tname(b));
    thrum_error(rt, line, col, msg, NULL);
}

TVal thrum_sub(TRuntime* rt, TVal a, TVal b, int line, int col) {
    if (a.kind == TV_NUM && b.kind == TV_NUM) return qadd_internal(rt, a.as.q, b.as.q, -1, line, col);
    char msg[96];
    snprintf(msg, sizeof(msg), "cannot subtract %s from %s", thrum_tname(b), thrum_tname(a));
    thrum_error(rt, line, col, msg, NULL);
}

TVal thrum_mul(TRuntime* rt, TVal a, TVal b, int line, int col) {
    if (a.kind == TV_NUM && b.kind == TV_NUM) return qmul_internal(rt, a.as.q, b.as.q);
    if (a.kind == TV_OBJ && a.as.obj->kind == OBJ_STR && b.kind == TV_NUM) {
        int64_t cnt = thrum_need_int(rt, b, line, col);
        if (cnt <= 0) return thrum_cstr(rt, "");
        if (cnt > 1000000) thrum_error(rt, line, col, "repeat count too large", NULL);
        TStr* s = (TStr*)a.as.obj;
        size_t tot = s->len * (size_t)cnt;
        char* buf = (char*)malloc(tot + 1);
        for (int64_t i = 0; i < cnt; i++) memcpy(buf + (size_t)i * s->len, s->data, s->len);
        buf[tot] = '\0';
        TVal out = thrum_str(rt, buf, tot);
        free(buf);
        return out;
    }
    if (a.kind == TV_OBJ && a.as.obj->kind == OBJ_LIST && b.kind == TV_NUM) {
        int64_t cnt = thrum_need_int(rt, b, line, col);
        if (cnt <= 0) return thrum_list(rt, 0);
        if (cnt > 1000000) thrum_error(rt, line, col, "repeat count too large", NULL);
        T_FRAME(rt);
        T_VAR(rt, va, a);
        TList* la = (TList*)va.as.obj;
        T_VAR(rt, res, thrum_list(rt, la->len * (size_t)cnt));
        TList* lr = (TList*)res.as.obj;
        for (int64_t i = 0; i < cnt; i++) {
            for (size_t j = 0; j < la->len; j++) thrum_list_push(rt, lr, la->items[j]);
        }
        T_END_FRAME(rt);
        return res;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "cannot multiply %s and %s", thrum_tname(a), thrum_tname(b));
    thrum_error(rt, line, col, msg, NULL);
}

TVal thrum_div(TRuntime* rt, TVal a, TVal b, int line, int col) {
    if (a.kind == TV_NUM && b.kind == TV_NUM) return qdiv_internal(rt, a.as.q, b.as.q, line, col);
    char msg[96];
    snprintf(msg, sizeof(msg), "cannot divide %s by %s", thrum_tname(a), thrum_tname(b));
    thrum_error(rt, line, col, msg, NULL);
}
