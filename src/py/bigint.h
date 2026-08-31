#ifndef LUNA_PY_BIGINT_H
#define LUNA_PY_BIGINT_H

/* bigint.h — arbitrary-precision integers for the Python frontend.
 *
 * PyLong-style representation: base 2^30 digits in little-endian order
 * (digits[0] is the least significant), sign stored separately (-1/0/+1).
 * The heap object only holds values that do not fit the int32 immediate;
 * every constructor normalizes to make_int() when the value fits, so an
 * int32 immediate and a heap bigint never represent the same number.
 * The core VM treats bigints as opaque frontend objects; all operations
 * flow through the frontend hooks and the Type MOP vtable (see vm.h).
 */

#include <stdbool.h>
#include <stdint.h>
#include "vm.h"

#define BIGINT_DIGIT_BITS 30u
#define BIGINT_DIGIT_MASK ((1u << 30) - 1u)

typedef struct ObjBigInt {
    Object   obj;
    int      sign;      /* -1, 0, +1 */
    int      size;      /* significant digits; size >= 1; digits[size-1] != 0 */
    int      capacity;  /* trailing digit slots (size <= capacity) */
    uint32_t digits[];  /* base 2^30, least-significant first */
} ObjBigInt;

/* ---- memory / constructors ---------------------------------------------
 * bigint_alloc links into the GC object list like every frontend object.
 * bigint_release unlinks and frees. bigint_to_value takes ownership of the
 * passed object, normalizing to an int32 immediate (released) when it fits. */
ObjBigInt *bigint_alloc(int size);
void       bigint_release(ObjBigInt *a);
void       bigint_trim(ObjBigInt *a);       /* strip leading zero digits */
bool       bigint_is_zero(const ObjBigInt *a);
int        bigint_bit_length(const ObjBigInt *a);
ObjBigInt *bigint_from_i64(int64_t v);      /* heap, exact */
ObjBigInt *bigint_from_u64(uint64_t v);     /* heap, non-negative */
ObjBigInt *bigint_copy(const ObjBigInt *a);
ObjBigInt *bigint_from_value(Value v);      /* int32 -> temp; bigint -> borrowed */
Value      bigint_to_value(ObjBigInt *a);   /* takes ownership; normalizes */

/* Value-level constructors (normalize to int32 immediate when they fit). */
Value bigint_from_i64_value(int64_t v);
bool  bigint_from_decimal(const char *s, size_t len, Value *out);  /* false on invalid literal */
bool  bigint_from_f64(double d, Value *out);  /* false for NaN/inf; truncates toward zero */

/* ---- arithmetic (all allocate fresh results; inputs are borrowed) ------ */
ObjBigInt *bigint_abs(const ObjBigInt *a);
ObjBigInt *bigint_neg(const ObjBigInt *a);
ObjBigInt *bigint_add(const ObjBigInt *a, const ObjBigInt *b);
ObjBigInt *bigint_sub(const ObjBigInt *a, const ObjBigInt *b);
ObjBigInt *bigint_mul(const ObjBigInt *a, const ObjBigInt *b);
void bigint_divmod(const ObjBigInt *a, const ObjBigInt *b,
                   ObjBigInt **q, ObjBigInt **r);   /* truncating; requires b != 0 */
ObjBigInt *bigint_pow(const ObjBigInt *base, const ObjBigInt *exp); /* exp >= 0 */
bool  bigint_pow_too_big(const ObjBigInt *base, const ObjBigInt *exp);

/* ---- shifts and bitwise (Python two's-complement semantics) ------------ */
ObjBigInt *bigint_shl(const ObjBigInt *a, int64_t bits);  /* bits < 0 mirrors to shr */
ObjBigInt *bigint_shr(const ObjBigInt *a, int64_t bits);  /* bits < 0 mirrors to shl; floor for negatives */
ObjBigInt *bigint_and(const ObjBigInt *a, const ObjBigInt *b);
ObjBigInt *bigint_or (const ObjBigInt *a, const ObjBigInt *b);
ObjBigInt *bigint_xor(const ObjBigInt *a, const ObjBigInt *b);
ObjBigInt *bigint_not(const ObjBigInt *a);

/* ---- comparisons (exact) ----------------------------------------------- */
int bigint_cmp(const ObjBigInt *a, const ObjBigInt *b);        /* -1/0/1 */
int bigint_cmp_value(const ObjBigInt *a, Value b);             /* b: int32/bigint/double */
int bigint_cmp_f64(const ObjBigInt *a, double d);              /* 2 = incomparable (NaN) */

/* ---- conversions (exact, no clamping) ---------------------------------- */
bool   bigint_get_i64(const ObjBigInt *a, int64_t *out);       /* false when out of range */
double bigint_to_f64(const ObjBigInt *a);                      /* correctly rounded */
double bigint_truediv(const ObjBigInt *x, const ObjBigInt *y); /* correctly rounded x / y, y != 0 */
uint32_t bigint_hash(const ObjBigInt *a);                      /* value mod 2^61-1 (CPython-style) */
char  *bigint_to_decimal(const ObjBigInt *a);                  /* malloc'd decimal string */

/* ---- value-level dispatch (errors set vm->last_exception, return null) - */
/* Operands must be integers (int32 or bigint); doubles are coerced for the
 * arithmetic ops. Returns a normalized Value (int32 immediate or bigint). */
Value bigint_binary_value(struct VM *vm, VMOperation op, Value a, Value b);
Value bigint_unary_value(struct VM *vm, VMOperation op, Value a);

#endif /* LUNA_PY_BIGINT_H */