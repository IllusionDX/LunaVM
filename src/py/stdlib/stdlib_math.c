/* stdlib_math.c — Built-in math module (C99 wrappers + game dev sugar).
 * Registered as a built-in module so `import math` works without a file.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stdlib_math.h"
#include "value.h"
#include "py/object.h"
#include "py/frontend_state.h"

/* ============================================================ */
/* Validation helpers                                           */
/* ============================================================ */

static inline bool is_num(Value v) { return IS_NUMBER(v); }

static inline void check_arity(VM *vm, int n, int expected, const char *name) {
    if (n != expected) {
        luna_throw(vm, py_fe(vm)->argument_error_class,
            "%s() expects exactly %d argument(s), got %d", name, expected, n);
    }
}

static inline void check_arity_min(VM *vm, int n, int min, const char *name) {
    if (n < min) {
        luna_throw(vm, py_fe(vm)->argument_error_class,
            "%s() expects at least %d argument(s), got %d", name, min, n);
    }
}

static inline double to_double_checked(VM *vm, Value v, const char *fn, int arg_idx) {
    if (!is_num(v)) {
        luna_throw(vm, py_fe(vm)->type_error_class,
            "%s() argument %d must be a number", fn, arg_idx);
    }
    return value_to_double(v);
}

/* ============================================================ */
/* C99 wrappers                                                */
/* ============================================================ */

static Value math_sin(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "sin");
    double v = to_double_checked(vm, args[0], "sin", 1);
    return make_double(sin(v));
}

static Value math_cos(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "cos");
    double v = to_double_checked(vm, args[0], "cos", 1);
    return make_double(cos(v));
}

static Value math_tan(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "tan");
    double v = to_double_checked(vm, args[0], "tan", 1);
    return make_double(tan(v));
}

static Value math_acos(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "acos");
    double v = to_double_checked(vm, args[0], "acos", 1);
    if (v < -1.0 || v > 1.0) {
        luna_throw(vm, py_fe(vm)->value_error_class,
            "Math domain error: acos() argument must be in range [-1, 1]");
    }
    return make_double(acos(v));
}

static Value math_asin(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "asin");
    double v = to_double_checked(vm, args[0], "asin", 1);
    if (v < -1.0 || v > 1.0) {
        luna_throw(vm, py_fe(vm)->value_error_class,
            "Math domain error: asin() argument must be in range [-1, 1]");
    }
    return make_double(asin(v));
}

static Value math_sqrt(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "sqrt");
    double v = to_double_checked(vm, args[0], "sqrt", 1);
    if (v < 0.0) {
        luna_throw(vm, py_fe(vm)->value_error_class,
            "Math domain error: sqrt() of a negative number");
    }
    return make_double(sqrt(v));
}

static Value math_log(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "log");
    double v = to_double_checked(vm, args[0], "log", 1);
    if (v <= 0.0) {
        luna_throw(vm, py_fe(vm)->value_error_class,
            "Math domain error: log() argument must be positive");
    }
    return make_double(log(v));
}

static Value math_log10(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "log10");
    double v = to_double_checked(vm, args[0], "log10", 1);
    if (v <= 0.0) {
        luna_throw(vm, py_fe(vm)->value_error_class,
            "Math domain error: log10() argument must be positive");
    }
    return make_double(log10(v));
}

static Value math_exp(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "exp");
    double v = to_double_checked(vm, args[0], "exp", 1);
    return make_double(exp(v));
}

static Value math_floor(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "floor");
    double v = to_double_checked(vm, args[0], "floor", 1);
    return make_double(floor(v));
}

static Value math_ceil(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "ceil");
    double v = to_double_checked(vm, args[0], "ceil", 1);
    return make_double(ceil(v));
}

static Value math_round(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "round");
    double v = to_double_checked(vm, args[0], "round", 1);
    return make_double(round(v));
}

static Value math_pow(VM *vm, Value *args, int n) {
    check_arity(vm, n, 2, "pow");
    double base = to_double_checked(vm, args[0], "pow", 1);
    double expv = to_double_checked(vm, args[1], "pow", 2);
    if (base < 0.0 && expv != floor(expv)) {
        luna_throw(vm, py_fe(vm)->value_error_class,
            "Math domain error: pow() with negative base and non-integer exponent");
    }
    return make_double(pow(base, expv));
}

static Value math_atan2(VM *vm, Value *args, int n) {
    check_arity(vm, n, 2, "atan2");
    double y = to_double_checked(vm, args[0], "atan2", 1);
    double x = to_double_checked(vm, args[1], "atan2", 2);
    return make_double(atan2(y, x));
}

static Value math_fmod(VM *vm, Value *args, int n) {
    check_arity(vm, n, 2, "mod");
    double a = to_double_checked(vm, args[0], "mod", 1);
    double b = to_double_checked(vm, args[1], "mod", 2);
    if (b == 0.0) {
        luna_throw(vm, py_fe(vm)->value_error_class,
            "Math domain error: mod() by zero");
    }
    return make_double(fmod(a, b));
}

static Value math_abs(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "abs");
    if (!is_num(args[0])) {
        luna_throw(vm, py_fe(vm)->type_error_class, "abs() requires a numeric argument");
    }
    if (IS_INT(args[0])) {
        int v = AS_INT(args[0]);
        return make_int(v < 0 ? -v : v);
    }
    if (IS_BIGINT(args[0])) {
        ObjBigInt *a = (ObjBigInt *)AS_OBJ(args[0]);
        return bigint_to_value(bigint_abs(a));
    }
    double v = AS_DOUBLE(args[0]);
    return make_double(v < 0.0 ? -v : v);
}

/* ============================================================ */
/* Game dev sugar                                              */
/* ============================================================ */

static Value math_clamp(VM *vm, Value *args, int n) {
    check_arity(vm, n, 3, "clamp");
    double v = to_double_checked(vm, args[0], "clamp", 1);
    double mn = to_double_checked(vm, args[1], "clamp", 2);
    double mx = to_double_checked(vm, args[2], "clamp", 3);
    if (mn > mx) {
        double t = mn; mn = mx; mx = t;
    }
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    return make_double(v);
}

static Value math_lerp(VM *vm, Value *args, int n) {
    check_arity(vm, n, 3, "lerp");
    double a = to_double_checked(vm, args[0], "lerp", 1);
    double b = to_double_checked(vm, args[1], "lerp", 2);
    double t = to_double_checked(vm, args[2], "lerp", 3);
    return make_double(a + (b - a) * t);
}

static Value math_sign(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "sign");
    double v = to_double_checked(vm, args[0], "sign", 1);
    if (v > 0.0) return make_int(1);
    if (v < 0.0) return make_int(-1);
    return make_int(0);
}

static Value math_deg_to_rad(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "deg_to_rad");
    double d = to_double_checked(vm, args[0], "deg_to_rad", 1);
    return make_double(d * (3.14159265358979323846 / 180.0));
}

static Value math_rad_to_deg(VM *vm, Value *args, int n) {
    check_arity(vm, n, 1, "rad_to_deg");
    double r = to_double_checked(vm, args[0], "rad_to_deg", 1);
    return make_double(r * (180.0 / 3.14159265358979323846));
}



/* ============================================================ */
/* Module registration                                         */
/* ============================================================ */

static void math_add_fn(VM *vm, ObjDict *exports, const char *name, NativeFn fn) {
    (void)vm;
    ObjFunction *f = new_native_function(name, fn);
    Value key = make_obj((Object*)new_string(name, (int)strlen(name)));
    dict_set(exports, key, make_obj((Object*)f));
}

static void math_add_const(ObjDict *exports, const char *name, double val) {
    Value key = make_obj((Object*)new_string(name, (int)strlen(name)));
    dict_set(exports, key, make_double(val));
}

void vm_register_math_module(VM *vm) {
    ObjModule *mod = new_module("math");

    /* C99 wrappers */
    math_add_fn(vm, mod->exports, "sin",        math_sin);
    math_add_fn(vm, mod->exports, "cos",        math_cos);
    math_add_fn(vm, mod->exports, "tan",        math_tan);
    math_add_fn(vm, mod->exports, "acos",       math_acos);
    math_add_fn(vm, mod->exports, "asin",       math_asin);
    math_add_fn(vm, mod->exports, "atan2",      math_atan2);
    math_add_fn(vm, mod->exports, "sqrt",       math_sqrt);
    math_add_fn(vm, mod->exports, "pow",        math_pow);
    math_add_fn(vm, mod->exports, "log",        math_log);
    math_add_fn(vm, mod->exports, "log10",      math_log10);
    math_add_fn(vm, mod->exports, "exp",        math_exp);
    math_add_fn(vm, mod->exports, "floor",      math_floor);
    math_add_fn(vm, mod->exports, "ceil",       math_ceil);
    math_add_fn(vm, mod->exports, "round",      math_round);
    math_add_fn(vm, mod->exports, "abs",        math_abs);
    math_add_fn(vm, mod->exports, "mod",        math_fmod);

    /* Game dev sugar */
    math_add_fn(vm, mod->exports, "clamp",      math_clamp);
    math_add_fn(vm, mod->exports, "lerp",       math_lerp);
    math_add_fn(vm, mod->exports, "sign",       math_sign);
    math_add_fn(vm, mod->exports, "deg_to_rad", math_deg_to_rad);
    math_add_fn(vm, mod->exports, "rad_to_deg", math_rad_to_deg);

    /* Constants */
    math_add_const(mod->exports, "pi",       3.14159265358979323846);
    math_add_const(mod->exports, "tau",      6.28318530717958647692);
    math_add_const(mod->exports, "epsilon",  1e-6);
    math_add_const(mod->exports, "inf",      INFINITY);
    math_add_const(mod->exports, "e",        2.71828182845904523536);

    /* Cache it so import math finds it */
    Value mod_val = make_obj((Object*)mod);
    ObjString *key = new_string("math", 4);
    dict_set(py_fe(vm)->module_cache, make_obj((Object*)key), mod_val);
}
