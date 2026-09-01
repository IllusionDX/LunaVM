/* range.c — lazy range objects for the Python frontend.
 *
 * A PyRange stores only (start, stop, step): O(1) memory regardless of how
 * many elements it represents. Consumers (for-loops via OP_GETITER/OP_FORITER,
 * len(), indexing) compute each value on demand with exact arbitrary-precision
 * bigint arithmetic, matching CPython's semantics for astronomically large
 * ranges instead of materializing a list.
 *
 * Bounds/step are always int32 immediates or bigints (bn_range validates the
 * integer-only contract), never doubles. */
#include "range.h"
#include "bigint.h"
#include "frontend_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- owned bigint view of an int32/bigint Value ------------------- */

static ObjBigInt *owned_from_value(Value v) {
    if (IS_BIGINT(v)) return bigint_copy((ObjBigInt *)AS_OBJ(v));
    if (IS_INT(v)) return bigint_from_i64(AS_INT(v));
    return bigint_from_i64(0);
}

/* -1/0/1 comparison for int32/bigint pairs (never doubles). */
static int iv_compare(Value a, Value b) {
    bool ab = IS_BIGINT(a), bb = IS_BIGINT(b);
    if (ab && bb) return bigint_cmp((ObjBigInt *)AS_OBJ(a), (ObjBigInt *)AS_OBJ(b));
    if (ab) return bigint_cmp_value((ObjBigInt *)AS_OBJ(a), b);
    if (bb) return -bigint_cmp_value((ObjBigInt *)AS_OBJ(b), a);
    int64_t x = AS_INT(a), y = AS_INT(b);
    return x < y ? -1 : x > y ? 1 : 0;
}

static int iv_sign(Value v) {
    if (IS_BIGINT(v)) return ((ObjBigInt *)AS_OBJ(v))->sign;
    int32_t s = AS_INT(v);
    return s > 0 ? 1 : s < 0 ? -1 : 0;
}

static bool iv_is_zero(Value v) {
    if (IS_BIGINT(v)) return ((ObjBigInt *)AS_OBJ(v))->sign == 0;
    return AS_INT(v) == 0;
}

/* Exact sum of two int32/bigint Values, returned as int32 or bigint. */
static Value iv_add(Value a, Value b) {
    if (IS_BIGINT(a) || IS_BIGINT(b)) {
        ObjBigInt *A = owned_from_value(a);
        ObjBigInt *B = owned_from_value(b);
        Value r = bigint_to_value(bigint_add(A, B));
        bigint_release(A); bigint_release(B);
        return r;
    }
    int64_t s = (int64_t)AS_INT(a) + (int64_t)AS_INT(b);
    return bigint_from_i64_value(s);
}

/* ============================================================ */
/* Constructors                                                  */
/* ============================================================ */

ObjRange *new_range(Value start, Value stop, Value step) {
    ObjRange *r = (ObjRange *)malloc(sizeof(ObjRange));
    if (!r) { fprintf(stderr, "OOM\n"); exit(1); }
    py_init_object((Object *)r, OBJ_RANGE, sizeof(ObjRange));
    r->start = start; r->stop = stop; r->step = step;
    return r;
}

ObjRangeIter *new_range_iter(Value start, Value stop, Value step) {
    ObjRangeIter *it = (ObjRangeIter *)malloc(sizeof(ObjRangeIter));
    if (!it) { fprintf(stderr, "OOM\n"); exit(1); }
    py_init_object((Object *)it, OBJ_RANGEITER, sizeof(ObjRangeIter));
    it->current = start; it->stop = stop; it->step = step;
    return it;
}

/* ============================================================ */
/* Iteration                                                     */
/* ============================================================ */

bool range_iter_next(struct VM *vm, ObjRangeIter *it, Value *elem) {
    (void)vm;
    if (IS_INT(it->current) && IS_INT(it->stop) && IS_INT(it->step)) {
        /* Fast path: int32 bounds, inlined arithmetic. */
        int32_t cur = AS_INT(it->current);
        int32_t stop = AS_INT(it->stop);
        int32_t step = AS_INT(it->step);
        if (step == 0) return false;
        if ((step > 0 && cur >= stop) || (step < 0 && cur <= stop)) return false;
        *elem = it->current;
        it->current = bigint_from_i64_value((int64_t)cur + (int64_t)step);
        return true;
    }
    /* Arbitrary-precision path (bigints). */
    if (iv_is_zero(it->step)) return false;
    int c = iv_compare(it->current, it->stop);
    int sgn = iv_sign(it->step);
    if ((sgn > 0 && c >= 0) || (sgn < 0 && c <= 0)) return false;
    *elem = it->current;
    it->current = iv_add(it->current, it->step);
    return true;
}

/* ============================================================ */
/* Length (O(1))                                                 */
/* ============================================================ */

Value range_length_value(ObjRange *r) {
    if (IS_BIGINT(r->start) || IS_BIGINT(r->stop) || IS_BIGINT(r->step)) {
        ObjBigInt *s = owned_from_value(r->start);
        ObjBigInt *e = owned_from_value(r->stop);
        ObjBigInt *p = owned_from_value(r->step);
        Value out;
        int sgn = p->sign;
        int cmp = bigint_cmp(s, e);
        if (sgn == 0) {
            out = make_int(0);
        } else if ((sgn > 0 && cmp >= 0) || (sgn < 0 && cmp <= 0)) {
            out = make_int(0);
        } else {
            ObjBigInt *one = bigint_from_i64(1);
            ObjBigInt *div;
            if (sgn > 0) {
                ObjBigInt *diff = bigint_sub(e, s);   /* stop - start > 0 */
                ObjBigInt *num = bigint_sub(diff, one); /* stop-start-1 */
                bigint_release(diff);
                div = p; /* step > 0 */
                ObjBigInt *qq, *rem;
                bigint_divmod(num, div, &qq, &rem);   /* both positive: floor */
                bigint_release(num); bigint_release(rem);
                ObjBigInt *len = bigint_add(qq, one);
                bigint_release(qq);
                out = bigint_to_value(len);
            } else {
                ObjBigInt *sp = bigint_neg(p);        /* |step| > 0 */
                ObjBigInt *diff = bigint_sub(s, e);   /* start - stop > 0 */
                ObjBigInt *num = bigint_sub(diff, one);
                bigint_release(diff);
                ObjBigInt *qq, *rem;
                bigint_divmod(num, sp, &qq, &rem);    /* both positive: floor */
                bigint_release(num); bigint_release(rem);
                ObjBigInt *len = bigint_add(qq, one);
                bigint_release(qq);
                bigint_release(sp);
                out = bigint_to_value(len);
            }
            bigint_release(one);
        }
        bigint_release(s); bigint_release(e); bigint_release(p);
        return out;
    }

    /* int32 fast path (exact in int64). */
    int64_t s = AS_INT(r->start);
    int64_t e = AS_INT(r->stop);
    int64_t p = AS_INT(r->step);
    if (p == 0) return make_int(0);
    int64_t n;
    if (p > 0) {
        if (s >= e) return make_int(0);
        n = 1 + (e - s - 1) / p;
    } else {
        if (s <= e) return make_int(0);
        n = 1 + (s - e - 1) / (-p);
    }
    return bigint_from_i64_value(n);
}

/* ============================================================ */
/* Indexing (O(1))                                               */
/* ============================================================ */

Value range_item(ObjRange *r, int64_t i64, Value ikey, bool is_big_index) {
    if (IS_BIGINT(r->start) || IS_BIGINT(r->step)) {
        ObjBigInt *s = owned_from_value(r->start);
        ObjBigInt *p = owned_from_value(r->step);
        ObjBigInt *i = is_big_index ? owned_from_value(ikey) : bigint_from_i64(i64);
        ObjBigInt *prod = bigint_mul(i, p);
        ObjBigInt *sum = bigint_add(s, prod);
        Value out = bigint_to_value(sum);
        bigint_release(s); bigint_release(p); bigint_release(i);
        bigint_release(prod);
        return out;
    }
    int64_t item = (int64_t)AS_INT(r->start) + (i64 * (int64_t)AS_INT(r->step));
    return bigint_from_i64_value(item);
}

/* getitem entry point used by the MOP vtable. Raises IndexError out of
 * range (with CPython negative-index support). */
Value py_range_getitem(VM *vm, Value self, Value key) {
    ObjRange *r = (ObjRange *)AS_OBJ(self);
    if (!IS_INT(key) && !IS_BIGINT(key)) {
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->type_error_class,
            "range indices must be integers");
        return make_null();
    }
    Value lenv = range_length_value(r);
    if (iv_is_zero(r->step)) {
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->value_error_class,
            "range() arg 3 must not be zero");
        return make_null();
    }
    /* Normalize negative index: i += len until >= 0. */
    Value idx = key;
    if (iv_compare(idx, make_int(0)) < 0) idx = iv_add(idx, lenv);
    if (iv_compare(idx, make_int(0)) < 0 || iv_compare(idx, lenv) >= 0) {
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->index_error_class,
            "range object index out of range");
        return make_null();
    }
    bool big = IS_BIGINT(idx);
    int64_t ii = 0;
    if (!big) ii = AS_INT(idx);
    return range_item(r, ii, idx, big);
}

/* ============================================================ */
/* repr / cstr                                                   */
/* ============================================================ */

static char *iv_decimal(Value v) {
    if (IS_BIGINT(v)) return bigint_to_decimal((ObjBigInt *)AS_OBJ(v));
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", AS_INT(v));
    return strdup(buf);
}

char *py_range_to_cstr(Value self) {
    ObjRange *r = (ObjRange *)AS_OBJ(self);
    char *s0 = iv_decimal(r->start);
    char *s1 = iv_decimal(r->stop);
    bool step_one = !IS_BIGINT(r->step) && AS_INT(r->step) == 1;
    size_t cap = 32 + strlen(s0) + strlen(s1);
    char *out = malloc(cap);
    if (!out) { fprintf(stderr, "OOM\n"); exit(1); }
    if (step_one) {
        snprintf(out, cap, "range(%s, %s)", s0, s1);
    } else {
        char *s2 = iv_decimal(r->step);
        size_t need = 32 + strlen(s0) + strlen(s1) + strlen(s2);
        if (cap < need) { cap = need; out = realloc(out, cap); }
        snprintf(out, cap, "range(%s, %s, %s)", s0, s1, s2);
        free(s2);
    }
    free(s0); free(s1);
    return out;
}

char *py_range_iter_to_cstr(Value self) {
    (void)self;
    char *out = malloc(32);
    if (!out) { fprintf(stderr, "OOM\n"); exit(1); }
    snprintf(out, 32, "<range_iterator>");
    return out;
}

/* MOP `tostring`: return a string Value for `str(range_obj)`. */
Value py_range_tostring(VM *vm, Value self) {
    (void)vm;
    char *s = py_range_to_cstr(self);
    Value v = make_obj((Object *)new_string(s, (int)strlen(s)));
    free(s);
    return v;
}

/* Materialize a range into a fresh list (used by list()/tuple()). Only safe
 * for ranges whose length is reasonable; callers requesting a huge range will
 * hit the same memory pressure as CPython would. */
ObjList *list_from_range(ObjRange *r) {
    ObjRangeIter *it = new_range_iter(r->start, r->stop, r->step);
    ObjList *l = new_list(0);
    Value elem;
    while (range_iter_next(NULL, it, &elem)) list_add(l, elem);
    return l;
}
