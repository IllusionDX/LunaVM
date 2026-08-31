/* number_methods.c — virtual object support for immediate numbers.
 *
 * int/float/bool live inline in the NaN box and have no heap object, but in
 * Python they are still objects with methods and attributes. Their canonical
 * classes and native methods live here, owned by the py frontend. The class
 * pointers are stored in the frontend state (py_fe(vm)->int_class /
 * float_class), not in the language-agnostic VM core. */

#include <stdint.h>
#include <math.h>

#include "vm.h"
#include "value.h"
#include "py/object.h"
#include "py/frontend_state.h"
#include "py/number_methods.h"

/* __int__ / __index__ / int(self) — exact conversions (CPython semantics). */
static Value py_num_int(VM *vm, Value *args, int n) {
    (void)n;
    Value v = args[0];
    if (IS_BOOL(v)) return make_int(IS_TRUE(v) ? 1 : 0);
    if (IS_INT(v) || IS_BIGINT(v)) return v;
    if (IS_DOUBLE(v)) {
        Value out;
        if (bigint_from_f64(AS_DOUBLE(v), &out)) return out;
        luna_throw(vm, py_fe(vm)->value_error_class,
            isnan(AS_DOUBLE(v)) ? "cannot convert float NaN to integer"
                                : "cannot convert float infinity to integer");
    }
    return make_int(0);
}

static Value py_num_index(VM *vm, Value *args, int n) {
    return py_num_int(vm, args, n);
}

/* __float__ / float(self) — correctly rounded for bigints; overflow raises. */
static Value py_num_float(VM *vm, Value *args, int n) {
    (void)n;
    Value v = args[0];
    double d;
    if (IS_BOOL(v)) d = IS_TRUE(v) ? 1.0 : 0.0;
    else if (IS_DOUBLE(v)) d = AS_DOUBLE(v);
    else if (IS_BIGINT(v)) {
        d = bigint_to_f64((ObjBigInt *)AS_OBJ(v));
        if (isinf(d)) {
            luna_throw(vm, py_fe(vm)->overflow_error_class,
                "int too large to convert to float");
        }
    }
    else d = (double)AS_INT(v);
    return make_double(d);
}

/* __bool__ / bool(self) */
static Value py_num_bool(VM *vm, Value *args, int n) {
    (void)vm; (void)n;
    Value v = args[0];
    if (IS_BOOL(v)) return v;
    if (IS_DOUBLE(v)) return BOOL_VAL(AS_DOUBLE(v) != 0.0);
    if (IS_BIGINT(v)) return BOOL_VAL(!bigint_is_zero((ObjBigInt *)AS_OBJ(v)));
    return BOOL_VAL(AS_INT(v) != 0);
}

/* int.bit_length() — bits needed for the magnitude (CPython semantics). */
static Value py_num_bit_length(VM *vm, Value *args, int n) {
    (void)vm; (void)n;
    Value v = args[0];
    if (IS_BOOL(v)) return make_int(IS_TRUE(v) ? 1 : 0);
    if (IS_BIGINT(v)) return make_int(bigint_bit_length((ObjBigInt *)AS_OBJ(v)));
    int64_t val = AS_INT(v);
    uint64_t mag = (val < 0) ? (uint64_t)(-(val + 1)) + 1 : (uint64_t)val;
    int bits = 0;
    while (mag) { bits++; mag >>= 1; }
    return make_int(bits);
}

/* float.is_integer() */
static Value py_flt_is_integer(VM *vm, Value *args, int n) {
    (void)vm; (void)n;
    Value v = args[0];
    if (!IS_DOUBLE(v) || IS_INF(v) || IS_NAN(v)) return FALSE_VAL;
    return BOOL_VAL(trunc(AS_DOUBLE(v)) == AS_DOUBLE(v));
}

/* real / imag — zero-arg natives, evaluated eagerly on attribute access. */
static Value py_num_self(VM *vm, Value *args, int n) {
    (void)vm; (void)n;
    return args[0];
}

static Value py_int_imag(VM *vm, Value *args, int n) {
    (void)vm; (void)args; (void)n;
    return make_int(0);
}

static Value py_flt_imag(VM *vm, Value *args, int n) {
    (void)vm; (void)args; (void)n;
    return make_double(0.0);
}

void py_register_number_methods(VM *vm) {
    ObjClass *int_cls = new_class("int", NULL);
    ObjClass *flt_cls = new_class("float", NULL);

    class_add_native_method(int_cls, "bit_length", py_num_bit_length);
    class_add_native_method(int_cls, "__int__",    py_num_int);
    class_add_native_method(int_cls, "__index__",  py_num_index);
    class_add_native_method(int_cls, "__float__",  py_num_float);
    class_add_native_method(int_cls, "__bool__",   py_num_bool);
    class_add_native_method(int_cls, "real",       py_num_self);
    class_add_native_method(int_cls, "imag",       py_int_imag);

    class_add_native_method(flt_cls, "is_integer", py_flt_is_integer);
    class_add_native_method(flt_cls, "__int__",    py_num_int);
    class_add_native_method(flt_cls, "__float__",  py_num_float);
    class_add_native_method(flt_cls, "__bool__",   py_num_bool);
    class_add_native_method(flt_cls, "real",       py_num_self);
    class_add_native_method(flt_cls, "imag",       py_flt_imag);

    py_fe(vm)->int_class = int_cls;
    py_fe(vm)->float_class = flt_cls;
}