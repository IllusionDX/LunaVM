/* stdlib_math.c — Built-in math module (C99 wrappers + game dev sugar).
 * Registered as a built-in module so `import math` works without a file.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stdlib_math.h"
#include "value.h"

/* ============================================================ */
/* Helper macros                                               */
/* ============================================================ */

#define MATH_UNARY(name, cfn)                                   \
    static Value math_##name(VM *vm, Value *args, int n) {      \
        (void)vm;                                               \
        if (!n) return make_double(0.0);                       \
        double v = IS_INT(args[0]) ? (double)AS_INT(args[0])   \
                                   : AS_DOUBLE(args[0]);       \
        return make_double(cfn(v));                             \
    }

#define MATH_BINARY(name, cfn)                                  \
    static Value math_##name(VM *vm, Value *args, int n) {      \
        (void)vm;                                               \
        if (n < 2) return make_double(0.0);                     \
        double a = IS_INT(args[0]) ? (double)AS_INT(args[0])   \
                                   : AS_DOUBLE(args[0]);       \
        double b = IS_INT(args[1]) ? (double)AS_INT(args[1])   \
                                   : AS_DOUBLE(args[1]);       \
        return make_double(cfn(a, b));                          \
    }

/* ============================================================ */
/* C99 wrappers                                                */
/* ============================================================ */

MATH_UNARY(sin,   sin)
MATH_UNARY(cos,   cos)
MATH_UNARY(tan,   tan)
MATH_UNARY(acos,  acos)
MATH_UNARY(asin,  asin)
MATH_UNARY(sqrt,  sqrt)
MATH_UNARY(log,   log)
MATH_UNARY(exp,   exp)
MATH_UNARY(floor, floor)
MATH_UNARY(ceil,  ceil)
MATH_UNARY(round, round)   /* C99 round() */

MATH_BINARY(pow, pow)
MATH_BINARY(atan2, atan2)
MATH_BINARY(fmod, fmod)

static Value math_abs(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_double(0.0);
    if (IS_INT(args[0])) {
        int v = AS_INT(args[0]);
        return make_int(v < 0 ? -v : v);
    }
    double v = AS_DOUBLE(args[0]);
    return make_double(v < 0.0 ? -v : v);
}

/* ============================================================ */
/* Game dev sugar                                              */
/* ============================================================ */

static Value math_clamp(VM *vm, Value *args, int n) {
    (void)vm;
    if (n < 3) return make_double(0.0);
    double v = IS_INT(args[0]) ? (double)AS_INT(args[0]) : AS_DOUBLE(args[0]);
    double mn = IS_INT(args[1]) ? (double)AS_INT(args[1]) : AS_DOUBLE(args[1]);
    double mx = IS_INT(args[2]) ? (double)AS_INT(args[2]) : AS_DOUBLE(args[2]);
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    return make_double(v);
}

static Value math_lerp(VM *vm, Value *args, int n) {
    (void)vm;
    if (n < 3) return make_double(0.0);
    double a = IS_INT(args[0]) ? (double)AS_INT(args[0]) : AS_DOUBLE(args[0]);
    double b = IS_INT(args[1]) ? (double)AS_INT(args[1]) : AS_DOUBLE(args[1]);
    double t = IS_INT(args[2]) ? (double)AS_INT(args[2]) : AS_DOUBLE(args[2]);
    return make_double(a + (b - a) * t);
}

static Value math_sign(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_int(0);
    double v = IS_INT(args[0]) ? (double)AS_INT(args[0]) : AS_DOUBLE(args[0]);
    if (v > 0.0) return make_int(1);
    if (v < 0.0) return make_int(-1);
    return make_int(0);
}

static Value math_deg_to_rad(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_double(0.0);
    double d = IS_INT(args[0]) ? (double)AS_INT(args[0]) : AS_DOUBLE(args[0]);
    return make_double(d * (3.14159265358979323846 / 180.0));
}

static Value math_rad_to_deg(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_double(0.0);
    double r = IS_INT(args[0]) ? (double)AS_INT(args[0]) : AS_DOUBLE(args[0]);
    return make_double(r * (180.0 / 3.14159265358979323846));
}

/* ============================================================ */
/* Random (simple)                                             */
/* ============================================================ */

static Value math_random(VM *vm, Value *args, int n) {
    (void)vm; (void)args; (void)n;
    return make_double((double)rand() / (double)RAND_MAX);
}

static Value math_random_int(VM *vm, Value *args, int n) {
    (void)vm;
    if (n < 2) return make_int(0);
    int mn = IS_INT(args[0]) ? AS_INT(args[0]) : (int)AS_DOUBLE(args[0]);
    int mx = IS_INT(args[1]) ? AS_INT(args[1]) : (int)AS_DOUBLE(args[1]);
    if (mn > mx) { int t = mn; mn = mx; mx = t; }
    return make_int(mn + rand() % (mx - mn + 1));
}

/* ============================================================ */
/* Module registration                                         */
/* ============================================================ */

static void math_add_fn(VM *vm, ObjDict *exports, const char *name, NativeFn fn) {
    (void)vm;
    ObjFunction *f = new_native_function(name, fn);
    retain_obj((Object*)f);
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

    /* Random */
    math_add_fn(vm, mod->exports, "random",     math_random);
    math_add_fn(vm, mod->exports, "random_int", math_random_int);

    /* Constants */
    math_add_const(mod->exports, "pi",       3.14159265358979323846);
    math_add_const(mod->exports, "tau",      6.28318530717958647692);
    math_add_const(mod->exports, "epsilon",  1e-6);
    math_add_const(mod->exports, "inf",      INFINITY);
    math_add_const(mod->exports, "e",        2.71828182845904523536);

    /* Cache it so import math finds it */
    Value mod_val = make_obj((Object*)mod);
    ObjString *key = new_string("math", 4);
    dict_set(vm->module_cache, make_obj((Object*)key), mod_val);
}
