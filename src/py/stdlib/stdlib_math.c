/* stdlib_math.c — Python 3.11 `math` module.
 * Faithful CPython surface: same names, semantics, and error behavior.
 * Registered as a built-in module so `import math` works without a file.
 */

#include <math.h>
#include <errno.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stdlib_math.h"
#include "value.h"
#include "py/object.h"
#include "py/range.h"
#include "py/frontend_state.h"

/* ============================================================ */
/* Validation helpers                                           */
/* ============================================================ */

/* bool participates as int (Python subclass semantics). */
static inline bool is_num(Value v) { return IS_NUMBER(v) || IS_BOOL(v); }

static inline void check_arity(VM *vm, int n, int expected, const char *name) {
    if (n != expected) {
        luna_throw(vm, py_fe(vm)->argument_error_class,
            "%s() expects exactly %d argument(s), got %d", name, expected, n);
    }
}

static const char *py_type_name(Value v);

static inline double to_double_checked(VM *vm, Value v, const char *fn, int arg_idx) {
    if (IS_BOOL(v)) return AS_BOOL(v) ? 1.0 : 0.0;
    if (!is_num(v)) {
        luna_throw(vm, py_fe(vm)->type_error_class,
            "%s() argument %d must be a real number, not %s",
            fn, arg_idx, py_type_name(v));
    }
    return value_to_double(v);
}

static const char *py_type_name(Value v) {
    if (IS_INT(v) || IS_BOOL(v) || IS_BIGINT(v)) return "int";
    if (IS_DOUBLE(v)) return "float";
    if (IS_STRING(v)) return "str";
    if (IS_LIST(v)) return "list";
    if (IS_TUPLE(v)) return "tuple";
    if (IS_DICT(v)) return "dict";
    return "object";
}

static inline bool is_int_like(Value v) { return IS_INT(v) || IS_BOOL(v) || IS_BIGINT(v); }

/* Integer argument conversion (CPython: "'x' object cannot be interpreted
 * as an integer").  allow_integral_float accepts 5.0-style floats. */
static int64_t to_i64_checked(VM *vm, Value v, const char *fn, int arg_idx,
                              bool allow_integral_float) {
    if (IS_INT(v)) return AS_INT(v);
    if (IS_BOOL(v)) return AS_BOOL(v) ? 1 : 0;
    if (IS_BIGINT(v)) {
        int64_t out;
        if (bigint_get_i64((ObjBigInt *)AS_OBJ(v), &out)) return out;
        luna_throw(vm, py_fe(vm)->overflow_error_class,
            "%s() argument %d too large to convert to int", fn, arg_idx);
    }
    if (IS_DOUBLE(v)) {
        double d = AS_DOUBLE(v);
        if (!allow_integral_float || isnan(d) || isinf(d) || d != floor(d)) {
            luna_throw(vm, py_fe(vm)->type_error_class,
                "'float' object cannot be interpreted as an integer");
        }
        if (d > 9.2e18 || d < -9.2e18) {
            luna_throw(vm, py_fe(vm)->overflow_error_class,
                "%s() argument %d too large to convert to int", fn, arg_idx);
        }
        return (int64_t)d;
    }
    luna_throw(vm, py_fe(vm)->type_error_class,
        "'%s' object cannot be interpreted as an integer", py_type_name(v));
    return 0;
}

/* Borrowed-or-temp bigint view of an int-like value (release temps). */
static ObjBigInt *as_bigint(Value v, bool *was_temp) {
    *was_temp = false;
    if (IS_BIGINT(v)) return (ObjBigInt *)AS_OBJ(v);
    *was_temp = true;
    if (IS_INT(v)) return bigint_from_i64(AS_INT(v));
    return bigint_from_i64(IS_BOOL(v) && AS_BOOL(v) ? 1 : 0);
}

/* Rounding-to-int shared by ceil/floor/trunc.  Callers reject NaN/inf
 * first (CPython: OverflowError for inf, ValueError for NaN). */
static Value round_to_int_value(double d, int mode /* 0=ceil 1=floor 2=trunc */) {
    double r = (mode == 0) ? ceil(d) : (mode == 1) ? floor(d) : trunc(d);
    Value out;
    if (bigint_from_f64(r, &out)) return out;  /* exact; normalizes to int32 */
    return make_int(0);
}

/* ============================================================ */
/* Iterable collection (list / tuple / range)                    */
/* ============================================================ */

static void collect_iterable(VM *vm, Value v, const char *fn, Value **items_out, int *count_out) {
    if (IS_LIST(v)) {
        ObjList *l = (ObjList *)AS_OBJ(v);
        *items_out = l->items ? l->items : l->inline_items;
        *count_out = l->count;
        return;
    }
    if (IS_TUPLE(v)) {
        ObjTuple *t = (ObjTuple *)AS_OBJ(v);
        *items_out = t->items;
        *count_out = t->count;
        return;
    }
    if (IS_RANGE(v)) {
        ObjList *l = list_from_range((ObjRange *)AS_OBJ(v));
        *items_out = l->items ? l->items : l->inline_items;
        *count_out = l->count;
        return;
    }
    luna_throw(vm, py_fe(vm)->type_error_class,
        "%s() argument must be an iterable, not %s", fn, py_type_name(v));
}

/* ============================================================ */
/* Number-theoretic and representation functions                 */
/* ============================================================ */

static Value math_ceil(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "ceil");
    Value v = args[0];
    if (is_int_like(v)) return v;
    double d = to_double_checked(vm, v, "ceil", 1);
    if (isnan(d)) luna_throw(vm, py_fe(vm)->value_error_class, "cannot convert float NaN to integer");
    if (isinf(d)) luna_throw(vm, py_fe(vm)->overflow_error_class, "cannot convert float infinity to integer");
    return round_to_int_value(d, 0);
}

static Value math_floor(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "floor");
    Value v = args[0];
    if (is_int_like(v)) return v;
    double d = to_double_checked(vm, v, "floor", 1);
    if (isnan(d)) luna_throw(vm, py_fe(vm)->value_error_class, "cannot convert float NaN to integer");
    if (isinf(d)) luna_throw(vm, py_fe(vm)->overflow_error_class, "cannot convert float infinity to integer");
    return round_to_int_value(d, 1);
}

static Value math_trunc(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "trunc");
    Value v = args[0];
    if (is_int_like(v)) return v;
    double d = to_double_checked(vm, v, "trunc", 1);
    if (isnan(d)) luna_throw(vm, py_fe(vm)->value_error_class, "cannot convert float NaN to integer");
    if (isinf(d)) luna_throw(vm, py_fe(vm)->overflow_error_class, "cannot convert float infinity to integer");
    return round_to_int_value(d, 2);
}

/* Binary-splitting product of [lo, hi) — exact factorial core. */
static ObjBigInt *prod_range(int64_t lo, int64_t hi) {
    if (hi - lo <= 0) return bigint_from_i64(1);
    if (hi - lo == 1) return bigint_from_i64(lo);
    int64_t mid = lo + (hi - lo) / 2;
    ObjBigInt *a = prod_range(lo, mid);
    ObjBigInt *b = prod_range(mid, hi);
    ObjBigInt *m = bigint_mul(a, b);
    bigint_release(a);
    bigint_release(b);
    return m;
}

static Value math_factorial(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "factorial");
    Value v = args[0];
    int64_t k;
    if (IS_DOUBLE(v)) {
        double d = AS_DOUBLE(v);
        if (isnan(d) || isinf(d) || d != floor(d))
            luna_throw(vm, py_fe(vm)->value_error_class,
                "'float' object cannot be interpreted as an integer");
        if (d > 1e7) luna_throw(vm, py_fe(vm)->overflow_error_class, "factorial() argument too large");
        k = (int64_t)d;
    } else {
        k = to_i64_checked(vm, v, "factorial", 1, false);
    }
    if (k < 0)
        luna_throw(vm, py_fe(vm)->value_error_class, "factorial() not defined for negative values");
    if (k <= 1) return make_int(1);
    ObjBigInt *acc = prod_range(2, k + 1);
    return bigint_to_value(acc);
}

static Value math_comb(VM *vm, Value *args, int n) {
    check_arity(vm, n, 2, "comb");
    int64_t nn = to_i64_checked(vm, args[0], "comb", 1, false);
    int64_t kk = to_i64_checked(vm, args[1], "comb", 2, false);
    if (nn < 0 || kk < 0)
        luna_throw(vm, py_fe(vm)->value_error_class, "comb() doesn't define factorial for negative values");
    if (kk > nn) return make_int(0);
    if (kk > nn - kk) kk = nn - kk;   /* symmetry */
    ObjBigInt *r = bigint_from_i64(1);
    for (int64_t i = 1; i <= kk; i++) {
        ObjBigInt *num = bigint_from_i64(nn - kk + i);
        ObjBigInt *t = bigint_mul(r, num);
        bigint_release(num);
        ObjBigInt *den = bigint_from_i64(i);
        ObjBigInt *q = NULL, *rem = NULL;
        bigint_divmod(t, den, &q, &rem);
        bigint_release(rem);
        bigint_release(den);
        bigint_release(t);
        bigint_release(r);
        r = q;
    }
    return bigint_to_value(r);
}

static Value math_perm(VM *vm, Value *args, int n) {
    if (n < 1 || n > 2) {
        luna_throw(vm, py_fe(vm)->argument_error_class,
            "perm() expects 1 or 2 arguments, got %d", n);
    }
    int64_t nn = to_i64_checked(vm, args[0], "perm", 1, false);
    int64_t kk;
    if (n == 2) kk = to_i64_checked(vm, args[1], "perm", 2, false);
    else kk = nn;
    if (nn < 0 || kk < 0)
        luna_throw(vm, py_fe(vm)->value_error_class, "perm() doesn't define factorial for negative values");
    if (kk > nn) return make_int(0);
    if (kk <= 1) return bigint_to_value(prod_range(nn > 1 ? nn - kk + 1 : 2, nn + 1));
    /* product of (n-k+1 .. n) */
    ObjBigInt *acc = prod_range(nn - kk + 1, nn + 1);
    return bigint_to_value(acc);
}

static ObjBigInt *bigint_gcd(ObjBigInt *a, ObjBigInt *b) {
    /* inputs are owned copies; returns gcd, consumes a and b */
    while (!bigint_is_zero(b)) {
        ObjBigInt *q = NULL, *r = NULL;
        bigint_divmod(a, b, &q, &r);
        bigint_release(a);
        bigint_release(q);
        a = b;
        b = r;
    }
    bigint_release(b);
    return a;
}

static Value math_gcd(VM *vm, Value *args, int n) {
    if (n == 0) return make_int(0);
    ObjBigInt *g = NULL;
    for (int i = 0; i < n; i++) {
        if (!is_int_like(args[i]))
            luna_throw(vm, py_fe(vm)->type_error_class,
                "'%s' object cannot be interpreted as an integer", py_type_name(args[i]));
        bool t1, t2;
        ObjBigInt *a = as_bigint(args[i], &t1);
        ObjBigInt *abs_a = bigint_abs(a);
        if (t1) bigint_release(a);
        if (!g) { g = abs_a; continue; }
        ObjBigInt *next = bigint_gcd(g, abs_a);   /* consumes both */
        g = next;
    }
    return bigint_to_value(g);
}

static Value math_lcm(VM *vm, Value *args, int n) {
    if (n == 0) return make_int(1);
    ObjBigInt *l = NULL;
    for (int i = 0; i < n; i++) {
        if (!is_int_like(args[i]))
            luna_throw(vm, py_fe(vm)->type_error_class,
                "'%s' object cannot be interpreted as an integer", py_type_name(args[i]));
        bool t1;
        ObjBigInt *a = as_bigint(args[i], &t1);
        ObjBigInt *abs_a = bigint_abs(a);
        if (t1) bigint_release(a);
        if (bigint_is_zero(abs_a)) { bigint_release(abs_a); if (l) bigint_release(l); return make_int(0); }
        if (!l) { l = abs_a; continue; }
        /* lcm = l / gcd(l, a) * a */
        ObjBigInt *l_copy = bigint_copy(l);
        ObjBigInt *g = bigint_gcd(l_copy, bigint_copy(abs_a));
        ObjBigInt *q = NULL, *rem = NULL;
        bigint_divmod(l, g, &q, &rem);
        bigint_release(rem);
        bigint_release(g);
        ObjBigInt *next = bigint_mul(q, abs_a);
        bigint_release(q);
        bigint_release(abs_a);
        bigint_release(l);
        l = next;
    }
    return bigint_to_value(l);
}

static Value math_isqrt(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "isqrt");
    Value v = args[0];
    if (!is_int_like(v))
        luna_throw(vm, py_fe(vm)->type_error_class,
            "'%s' object cannot be interpreted as an integer", py_type_name(v));
    bool t;
    ObjBigInt *num = as_bigint(v, &t);
    if (num->sign < 0) {
        if (t) bigint_release(num);
        luna_throw(vm, py_fe(vm)->value_error_class, "isqrt() argument must be nonnegative");
    }
    if (bigint_is_zero(num)) {
        if (t) bigint_release(num);
        return make_int(0);
    }
    /* Newton: x0 = 2^ceil(bits/2); iterate x = (x + n//x) >> 1 while x decreases. */
    int bits = bigint_bit_length(num);
    ObjBigInt *x = bigint_shl(bigint_from_i64(1), (int64_t)((bits + 1) / 2));
    for (;;) {
        ObjBigInt *q = NULL, *rem = NULL;
        bigint_divmod(num, x, &q, &rem);
        bigint_release(rem);
        ObjBigInt *sum = bigint_add(x, q);
        bigint_release(q);
        ObjBigInt *y = bigint_shr(sum, 1);
        bigint_release(sum);
        if (bigint_cmp(y, x) >= 0) {
            bigint_release(y);
            break;
        }
        bigint_release(x);
        x = y;
    }
    if (t) bigint_release(num);
    return bigint_to_value(x);
}

static Value math_copysign(VM *vm, Value *args, int n) {
    check_arity(vm, n, 2, "copysign");
    double x = to_double_checked(vm, args[0], "copysign", 1);
    double y = to_double_checked(vm, args[1], "copysign", 2);
    return make_double(copysign(x, y));
}

static Value math_fabs(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "fabs");
    return make_double(fabs(to_double_checked(vm, args[0], "fabs", 1)));
}

static Value math_fmod(VM *vm, Value *args, int n) {
    check_arity(vm, n, 2, "fmod");
    double x = to_double_checked(vm, args[0], "fmod", 1);
    double y = to_double_checked(vm, args[1], "fmod", 2);
    if (y == 0.0 && !isnan(x) && !isnan(y))
        luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    return make_double(fmod(x, y));
}

static Value math_frexp(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "frexp");
    double d = to_double_checked(vm, args[0], "frexp", 1);
    int e = 0;
    double m = frexp(d, &e);
    ObjTuple *t = new_tuple(2);
    t->items[0] = make_double(m);
    t->items[1] = make_int(e);
    return make_obj((Object *)t);
}

static Value math_ldexp(VM *vm, Value *args, int n) {
    check_arity(vm, n, 2, "ldexp");
    double x = to_double_checked(vm, args[0], "ldexp", 1);
    /* CPython's ldexp rejects the exponent with its own message before
     * generic int conversion, so handle the float case inline. */
    if (IS_DOUBLE(args[1])) {
        luna_throw(vm, py_fe(vm)->type_error_class,
            "Expected an int as second argument to ldexp.");
    }
    int64_t i = to_i64_checked(vm, args[1], "ldexp", 2, false);
    if (i > 65536 || i < -65536) {
        if (x == 0.0) return make_double(x);
        luna_throw(vm, py_fe(vm)->overflow_error_class, "math range error");
    }
    double r = ldexp(x, (int)i);
    if (isinf(r) && isfinite(x))
        luna_throw(vm, py_fe(vm)->overflow_error_class, "math range error");
    return make_double(r);
}

static Value math_modf(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "modf");
    double d = to_double_checked(vm, args[0], "modf", 1);
    double ip;
    double fp = modf(d, &ip);
    ObjTuple *t = new_tuple(2);
    t->items[0] = make_double(fp);
    t->items[1] = make_double(ip);
    return make_obj((Object *)t);
}

static Value math_nextafter(VM *vm, Value *args, int n) {
    check_arity(vm, n, 2, "nextafter");
    double x = to_double_checked(vm, args[0], "nextafter", 1);
    double y = to_double_checked(vm, args[1], "nextafter", 2);
    if (x == y) return make_double(y);
    return make_double(nextafter(x, y));
}

static Value math_ulp(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "ulp");
    double x = to_double_checked(vm, args[0], "ulp", 1);
    if (isnan(x)) return make_double(x);
    if (x < 0) x = -x;
    if (isinf(x)) return make_double(x);
    if (x == 0.0) return make_double(4.9406564584124654e-324); /* DBL_TRUE_MIN */
    double nx = nextafter(x, INFINITY);
    return make_double(nx - x);
}

/* Shewchuk msum: exact-ish accumulation through loss-free partials. */
static Value math_fsum(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "fsum");
    Value *items;
    int count;
    collect_iterable(vm, args[0], "fsum", &items, &count);
    double partials[64];
    int np = 0;
    for (int k = 0; k < count; k++) {
        double x = to_double_checked(vm, items[k], "fsum", 1);
        int i = 0;
        for (int j = 0; j < np; j++) {
            double y = partials[j];
            if (fabs(x) < fabs(y)) { double tmp = x; x = y; y = tmp; }
            double hi = x + y;
            double lo = y - (hi - x);
            if (lo != 0.0) partials[i++] = lo;
            x = hi;
        }
        if (i < (int)(sizeof(partials) / sizeof(partials[0]))) {
            partials[i++] = x;
            np = i;
        } else {
            /* partials exhausted: fold the overflow run (rare) */
            for (int j2 = 1; j2 < i; j2++) x += partials[j2];
            partials[i - 1] = x;
            np = i;
        }
    }
    double sum = 0.0;
    for (int j = np - 1; j >= 0; j--) sum += partials[j];
    return make_double(sum);
}

static Value math_isfinite(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "isfinite");
    Value v = args[0];
    if (is_int_like(v)) return make_bool(true);
    double d = to_double_checked(vm, v, "isfinite", 1);
    return make_bool(isfinite(d));
}

static Value math_isinf(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "isinf");
    Value v = args[0];
    if (is_int_like(v)) return make_bool(false);
    double d = to_double_checked(vm, v, "isinf", 1);
    return make_bool(isinf(d) != 0);
}

static Value math_isnan(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "isnan");
    Value v = args[0];
    if (is_int_like(v)) return make_bool(false);
    double d = to_double_checked(vm, v, "isnan", 1);
    return make_bool(isnan(d) != 0);
}

static Value math_isclose(VM *vm, Value *args, int argc, Value kw_names) {
    if (argc < 2 || argc > 4) {
        luna_throw(vm, py_fe(vm)->argument_error_class,
            "isclose() takes 2 positional arguments, got %d", argc);
    }
    double rel_tol = 1e-9, abs_tol = 0.0;
    int positional = argc;
    int kw_count = 0;
    ObjList *names = NULL;
    if (IS_LIST(kw_names)) {
        names = (ObjList *)AS_OBJ(kw_names);
        kw_count = list_length(names);
        positional = argc - kw_count;
    }
    if (positional > 2) {
        luna_throw(vm, py_fe(vm)->argument_error_class,
            "isclose() takes 2 positional arguments, got %d", positional);
    }
    double a = to_double_checked(vm, args[0], "isclose", 1);
    double b = to_double_checked(vm, args[1], "isclose", 2);
    for (int i = 0; i < kw_count; i++) {
        Value nv = list_get(names, i);
        Value vv = args[positional + i];
        const char *nm = IS_STRING(nv) ? ((ObjString *)AS_OBJ(nv))->chars : "";
        if (strcmp(nm, "rel_tol") == 0) rel_tol = to_double_checked(vm, vv, "isclose", 3);
        else if (strcmp(nm, "abs_tol") == 0) abs_tol = to_double_checked(vm, vv, "isclose", 4);
        else luna_throw(vm, py_fe(vm)->type_error_class, "isclose() got an unexpected keyword argument '%s'", nm);
    }
    if (rel_tol < 0.0 || abs_tol < 0.0)
        luna_throw(vm, py_fe(vm)->value_error_class,
            "tolerances must be non-negative");
    if (isnan(a) || isnan(b)) return make_bool(false);
    if (isinf(a) && isinf(b)) return make_bool((a > 0) == (b > 0));
    double diff = fabs(a - b);
    return make_bool(diff <= fmax(rel_tol * fmax(fabs(a), fabs(b)), abs_tol));
}

/* ============================================================ */
/* Power and logarithmic functions                               */
/* ============================================================ */

static Value overflow_check(double r, double x) {
    if (isinf(r) && isfinite(x)) return (Value)0;  /* caller throws */
    return (Value)1;
}

static Value math_exp(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "exp");
    double x = to_double_checked(vm, args[0], "exp", 1);
    double r = exp(x);
    if (isinf(r) && isfinite(x))
        luna_throw(vm, py_fe(vm)->overflow_error_class, "math range error");
    return make_double(r);
}

static Value math_exp2(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "exp2");
    double x = to_double_checked(vm, args[0], "exp2", 1);
    double r = exp2(x);
    if (isinf(r) && isfinite(x))
        luna_throw(vm, py_fe(vm)->overflow_error_class, "math range error");
    return make_double(r);
}

static Value math_expm1(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "expm1");
    double x = to_double_checked(vm, args[0], "expm1", 1);
    double r = expm1(x);
    if (isinf(r) && isfinite(x))
        luna_throw(vm, py_fe(vm)->overflow_error_class, "math range error");
    return make_double(r);
}

static Value math_log(VM *vm, Value *args, int n) {
    if (n < 1 || n > 2) {
        luna_throw(vm, py_fe(vm)->argument_error_class,
            "log() expects 1 or 2 arguments, got %d", n);
    }
    double x = to_double_checked(vm, args[0], "log", 1);
    if (x <= 0.0) luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    if (n == 1) return make_double(log(x));
    double base = to_double_checked(vm, args[1], "log", 2);
    if (base <= 0.0) luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    if (base == 2.0) return make_double(log2(x));
    if (base == 10.0) return make_double(log10(x));
    return make_double(log(x) / log(base));
}

static Value math_log1p(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "log1p");
    double x = to_double_checked(vm, args[0], "log1p", 1);
    if (x <= -1.0) luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    return make_double(log1p(x));
}

static Value math_log2(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "log2");
    double x = to_double_checked(vm, args[0], "log2", 1);
    if (x <= 0.0) luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    return make_double(log2(x));
}

static Value math_log10(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "log10");
    double x = to_double_checked(vm, args[0], "log10", 1);
    if (x <= 0.0) luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    return make_double(log10(x));
}

static Value math_pow(VM *vm, Value *args, int n) {
    check_arity(vm, n, 2, "pow");
    double base = to_double_checked(vm, args[0], "pow", 1);
    double expv = to_double_checked(vm, args[1], "pow", 2);
    if (base < 0.0 && expv != floor(expv))
        luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    double r = pow(base, expv);
    if (isinf(r) && isfinite(base) && isfinite(expv))
        luna_throw(vm, py_fe(vm)->overflow_error_class, "math range error");
    return make_double(r);
}

static Value math_sqrt(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "sqrt");
    double v = to_double_checked(vm, args[0], "sqrt", 1);
    if (v < 0.0) luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    return make_double(sqrt(v));
}

static Value math_remainder(VM *vm, Value *args, int n) {
    check_arity(vm, n, 2, "remainder");
    double x = to_double_checked(vm, args[0], "remainder", 1);
    double y = to_double_checked(vm, args[1], "remainder", 2);
    if (y == 0.0 && !isnan(x))
        luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    if (isinf(x) && !isnan(y))
        luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    return make_double(remainder(x, y));
}

static Value math_cbrt(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "cbrt");
    return make_double(cbrt(to_double_checked(vm, args[0], "cbrt", 1)));
}

/* ============================================================ */
/* Trigonometric functions                                       */
/* ============================================================ */

static Value math_sin(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "sin");
    return make_double(sin(to_double_checked(vm, args[0], "sin", 1)));
}

static Value math_cos(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "cos");
    return make_double(cos(to_double_checked(vm, args[0], "cos", 1)));
}

static Value math_tan(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "tan");
    return make_double(tan(to_double_checked(vm, args[0], "tan", 1)));
}

static Value math_asin(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "asin");
    double v = to_double_checked(vm, args[0], "asin", 1);
    if (v < -1.0 || v > 1.0) luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    return make_double(asin(v));
}

static Value math_acos(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "acos");
    double v = to_double_checked(vm, args[0], "acos", 1);
    if (v < -1.0 || v > 1.0) luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    return make_double(acos(v));
}

static Value math_atan(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "atan");
    return make_double(atan(to_double_checked(vm, args[0], "atan", 1)));
}

static Value math_atan2(VM *vm, Value *args, int n) {
    check_arity(vm, n, 2, "atan2");
    double y = to_double_checked(vm, args[0], "atan2", 1);
    double x = to_double_checked(vm, args[1], "atan2", 2);
    return make_double(atan2(y, x));
}

static Value math_hypot(VM *vm, Value *args, int n) {
    if (n == 0)
        luna_throw(vm, py_fe(vm)->argument_error_class, "hypot() expects at least 1 argument");
    double m = 0.0;
    double vals[256];
    if (n > 256)
        luna_throw(vm, py_fe(vm)->argument_error_class, "hypot() supports at most 256 arguments");
    for (int i = 0; i < n; i++) {
        vals[i] = fabs(to_double_checked(vm, args[i], "hypot", i + 1));
        if (vals[i] > m) m = vals[i];
    }
    if (m == 0.0) return make_double(0.0);
    if (isinf(m)) return make_double(m);
    double s = 0.0;
    for (int i = 0; i < n; i++) {
        double r = vals[i] / m;
        s += r * r;
    }
    return make_double(m * sqrt(s));
}

static Value math_dist(VM *vm, Value *args, int n) {
    check_arity(vm, n, 2, "dist");
    Value *pa, *pb;
    int ca, cb;
    collect_iterable(vm, args[0], "dist", &pa, &ca);
    collect_iterable(vm, args[1], "dist", &pb, &cb);
    if (ca != cb)
        luna_throw(vm, py_fe(vm)->value_error_class,
            "dist() points must have the same dimension");
    double m = 0.0;
    double diffs[256];
    if (ca > 256)
        luna_throw(vm, py_fe(vm)->argument_error_class, "dist() supports at most 256 dimensions");
    for (int i = 0; i < ca; i++) {
        double d = to_double_checked(vm, pa[i], "dist", 1) - to_double_checked(vm, pb[i], "dist", 2);
        diffs[i] = fabs(d);
        if (diffs[i] > m) m = diffs[i];
    }
    if (m == 0.0) return make_double(0.0);
    if (isinf(m)) return make_double(m);
    double s = 0.0;
    for (int i = 0; i < ca; i++) {
        double r = diffs[i] / m;
        s += r * r;
    }
    return make_double(m * sqrt(s));
}

/* ============================================================ */
/* Angular conversion                                            */
/* ============================================================ */

static Value math_degrees(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "degrees");
    return make_double(to_double_checked(vm, args[0], "degrees", 1) * (180.0 / 3.14159265358979323846));
}

static Value math_radians(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "radians");
    return make_double(to_double_checked(vm, args[0], "radians", 1) * (3.14159265358979323846 / 180.0));
}

/* ============================================================ */
/* Hyperbolic functions                                          */
/* ============================================================ */

static Value math_sinh(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "sinh");
    double x = to_double_checked(vm, args[0], "sinh", 1);
    double r = sinh(x);
    if (isinf(r) && isfinite(x))
        luna_throw(vm, py_fe(vm)->overflow_error_class, "math range error");
    return make_double(r);
}

static Value math_cosh(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "cosh");
    double x = to_double_checked(vm, args[0], "cosh", 1);
    double r = cosh(x);
    if (isinf(r) && isfinite(x))
        luna_throw(vm, py_fe(vm)->overflow_error_class, "math range error");
    return make_double(r);
}

static Value math_tanh(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "tanh");
    return make_double(tanh(to_double_checked(vm, args[0], "tanh", 1)));
}

static Value math_asinh(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "asinh");
    return make_double(asinh(to_double_checked(vm, args[0], "asinh", 1)));
}

static Value math_acosh(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "acosh");
    double x = to_double_checked(vm, args[0], "acosh", 1);
    if (x < 1.0) luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    return make_double(acosh(x));
}

static Value math_atanh(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "atanh");
    double x = to_double_checked(vm, args[0], "atanh", 1);
    if (x < -1.0 || x > 1.0) luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    double r = atanh(x);
    if (isinf(r) && isfinite(x))
        luna_throw(vm, py_fe(vm)->overflow_error_class, "math range error");
    return make_double(r);
}

/* ============================================================ */
/* Special functions                                             */
/* ============================================================ */

static Value math_erf(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "erf");
    return make_double(erf(to_double_checked(vm, args[0], "erf", 1)));
}

static Value math_erfc(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "erfc");
    return make_double(erfc(to_double_checked(vm, args[0], "erfc", 1)));
}

static Value math_gamma(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "gamma");
    double x = to_double_checked(vm, args[0], "gamma", 1);
    if (x == 0.0 || (x < 0.0 && x == floor(x)))
        luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    double r = tgamma(x);
    if (isinf(r) && isfinite(x))
        luna_throw(vm, py_fe(vm)->overflow_error_class, "math range error");
    return make_double(r);
}

static Value math_lgamma(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "lgamma");
    double x = to_double_checked(vm, args[0], "lgamma", 1);
    if (x == 0.0 || (x < 0.0 && x == floor(x)))
        luna_throw(vm, py_fe(vm)->value_error_class, "math domain error");
    return make_double(lgamma(x));
}

/* ============================================================ */
/* prod — iterable product (CPython 3.8+, keyword-only start)    */
/* ============================================================ */

static Value math_prod(VM *vm, Value *args, int argc, Value kw_names) {
    int kw_count = 0;
    ObjList *names = NULL;
    if (IS_LIST(kw_names)) {
        names = (ObjList *)AS_OBJ(kw_names);
        kw_count = list_length(names);
    }
    if (argc - kw_count != 1) {
        luna_throw(vm, py_fe(vm)->argument_error_class,
            "prod() takes exactly 1 positional argument (%d given)", argc - kw_count);
    }
    Value *items;
    int count;
    collect_iterable(vm, args[0], "prod", &items, &count);
    Value start = make_int(1);
    for (int i = 0; i < kw_count; i++) {
        Value nv = list_get(names, i);
        const char *nm = IS_STRING(nv) ? ((ObjString *)AS_OBJ(nv))->chars : "";
        if (strcmp(nm, "start") == 0) start = args[1 + i];
        else luna_throw(vm, py_fe(vm)->type_error_class, "prod() got an unexpected keyword argument '%s'", nm);
    }
    for (int i = 0; i < count; i++) {
        if (!is_num(items[i]))
            luna_throw(vm, py_fe(vm)->type_error_class,
                "prod() can only multiply numbers, not %s", py_type_name(items[i]));
        start = bigint_binary_value(vm, VM_OP_MUL, start, items[i]);
    }
    return start;
}

/* ============================================================ */
/* Module registration                                           */
/* ============================================================ */

static void math_add_fn(VM *vm, ObjDict *exports, const char *name, NativeFn fn) {
    (void)vm;
    ObjFunction *f = new_native_function(name, fn);
    Value key = make_obj((Object*)new_string(name, (int)strlen(name)));
    dict_set(exports, key, make_obj((Object*)f));
}

static void math_add_fn_kw(VM *vm, ObjDict *exports, const char *name, NativeKwFn fn) {
    (void)vm;
    ObjFunction *f = new_native_kw_function(name, fn);
    Value key = make_obj((Object*)new_string(name, (int)strlen(name)));
    dict_set(exports, key, make_obj((Object*)f));
}

static void math_add_const(ObjDict *exports, const char *name, double val) {
    Value key = make_obj((Object*)new_string(name, (int)strlen(name)));
    dict_set(exports, key, make_double(val));
}

void vm_register_math_module(VM *vm) {
    ObjModule *mod = new_module("math");
    ObjDict *e = mod->exports;

    /* Number-theoretic and representation */
    math_add_fn(vm, e, "ceil",      math_ceil);
    math_add_fn(vm, e, "comb",      math_comb);
    math_add_fn(vm, e, "copysign",  math_copysign);
    math_add_fn(vm, e, "fabs",      math_fabs);
    math_add_fn(vm, e, "factorial", math_factorial);
    math_add_fn(vm, e, "floor",     math_floor);
    math_add_fn(vm, e, "fmod",      math_fmod);
    math_add_fn(vm, e, "frexp",     math_frexp);
    math_add_fn(vm, e, "fsum",      math_fsum);
    math_add_fn(vm, e, "gcd",       math_gcd);
    math_add_fn_kw(vm, e, "isclose", math_isclose);
    math_add_fn(vm, e, "isfinite",  math_isfinite);
    math_add_fn(vm, e, "isinf",     math_isinf);
    math_add_fn(vm, e, "isnan",     math_isnan);
    math_add_fn(vm, e, "isqrt",     math_isqrt);
    math_add_fn(vm, e, "lcm",       math_lcm);
    math_add_fn(vm, e, "ldexp",     math_ldexp);
    math_add_fn(vm, e, "modf",      math_modf);
    math_add_fn(vm, e, "nextafter", math_nextafter);
    math_add_fn(vm, e, "perm",      math_perm);
    math_add_fn_kw(vm, e, "prod",   math_prod);
    math_add_fn(vm, e, "remainder", math_remainder);
    math_add_fn(vm, e, "trunc",     math_trunc);
    math_add_fn(vm, e, "ulp",       math_ulp);

    /* Power and logarithmic */
    math_add_fn(vm, e, "cbrt",   math_cbrt);
    math_add_fn(vm, e, "exp",    math_exp);
    math_add_fn(vm, e, "exp2",   math_exp2);
    math_add_fn(vm, e, "expm1",  math_expm1);
    math_add_fn(vm, e, "log",    math_log);
    math_add_fn(vm, e, "log1p",  math_log1p);
    math_add_fn(vm, e, "log2",   math_log2);
    math_add_fn(vm, e, "log10",  math_log10);
    math_add_fn(vm, e, "pow",    math_pow);
    math_add_fn(vm, e, "sqrt",   math_sqrt);

    /* Trigonometric */
    math_add_fn(vm, e, "acos",   math_acos);
    math_add_fn(vm, e, "asin",   math_asin);
    math_add_fn(vm, e, "atan",   math_atan);
    math_add_fn(vm, e, "atan2",  math_atan2);
    math_add_fn(vm, e, "cos",    math_cos);
    math_add_fn(vm, e, "dist",   math_dist);
    math_add_fn(vm, e, "hypot",  math_hypot);
    math_add_fn(vm, e, "sin",    math_sin);
    math_add_fn(vm, e, "tan",    math_tan);

    /* Angular conversion */
    math_add_fn(vm, e, "degrees", math_degrees);
    math_add_fn(vm, e, "radians", math_radians);

    /* Hyperbolic */
    math_add_fn(vm, e, "acosh", math_acosh);
    math_add_fn(vm, e, "asinh", math_asinh);
    math_add_fn(vm, e, "atanh", math_atanh);
    math_add_fn(vm, e, "cosh",  math_cosh);
    math_add_fn(vm, e, "sinh",  math_sinh);
    math_add_fn(vm, e, "tanh",  math_tanh);

    /* Special */
    math_add_fn(vm, e, "erf",    math_erf);
    math_add_fn(vm, e, "erfc",   math_erfc);
    math_add_fn(vm, e, "gamma",  math_gamma);
    math_add_fn(vm, e, "lgamma", math_lgamma);

    /* Constants */
    math_add_const(e, "pi",  3.14159265358979323846);
    math_add_const(e, "e",   2.71828182845904523536);
    math_add_const(e, "tau", 6.28318530717958647692);
    math_add_const(e, "inf", INFINITY);
    math_add_const(e, "nan", (double)NAN);

    /* Cache it so import math finds it */
    Value mod_val = make_obj((Object*)mod);
    ObjString *key = new_string("math", 4);
    dict_set(py_fe(vm)->module_cache, make_obj((Object*)key), mod_val);
}
