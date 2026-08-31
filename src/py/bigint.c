/* bigint.c — arbitrary-precision integers for the Python frontend.
 *
 * PyLong-style representation: base 2^30 digits in little-endian order with
 * a separate sign (-1/0/+1). All arithmetic follows CPython semantics:
 * floor division, divisor-signed modulo, exponentiation by squaring,
 * infinite two's-complement bitwise ops, round-to-nearest-even float
 * conversion. Conversions are exact and never clamp: values that do not fit
 * an int64_t report an overflow instead of truncating.
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/bigint.h"
#include "py/object.h"
#include "py/frontend_state.h"

/* Safety limits (a game engine must not hang or exhaust memory on `2**10**9`). */
#define BIGINT_POW_MAX_WORDS  (1 << 20)   /* ~4 MB per pow() result */
#define BIGINT_SHIFT_MAX_BITS (1 << 26)   /* 64 MB for a shift result */

/* ============================================================ */
/* Memory / constructors                                         */
/* ============================================================ */

static inline unsigned bigint_clz32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return x ? (unsigned)__builtin_clz(x) : 32u;
#else
    unsigned n = 0;
    if (!x) return 32;
    while (!(x & 0x80000000u)) { n++; x <<= 1; }
    return n;
#endif
}

static inline unsigned bigint_clz64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return x ? (unsigned)__builtin_clzll((unsigned long long)x) : 64u;
#else
    unsigned n = 0;
    if (!x) return 64;
    while (!(x & 0x8000000000000000ULL)) { n++; x <<= 1; }
    return n;
#endif
}

ObjBigInt *bigint_alloc(int size) {
    if (size < 1) size = 1;
    size_t total = sizeof(ObjBigInt) + (size_t)size * sizeof(uint32_t);
    ObjBigInt *b = malloc(total);
    if (!b) { fprintf(stderr, "OOM\n"); exit(1); }
    py_init_object((Object *)b, OBJ_BIGINT, total);
    b->sign = 0;
    b->size = size;
    b->capacity = size;
    memset(b->digits, 0, (size_t)size * sizeof(uint32_t));
    return b;
}

void bigint_release(ObjBigInt *a) {
    if (a) free_object_container((Object *)a);
}

void bigint_trim(ObjBigInt *a) {
    while (a->size > 1 && a->digits[a->size - 1] == 0) a->size--;
    if (a->size == 1 && a->digits[0] == 0) a->sign = 0;
}

bool bigint_is_zero(const ObjBigInt *a) { return a->sign == 0; }

int bigint_bit_length(const ObjBigInt *a) {
    if (a->sign == 0) return 0;
    unsigned topz = bigint_clz32(a->digits[a->size - 1]);
    return (a->size - 1) * 30 + 32 - (int)topz;
}

static int bigint_cmp_mag(const ObjBigInt *a, const ObjBigInt *b) {
    if (a->size != b->size) return a->size < b->size ? -1 : 1;
    for (int i = a->size - 1; i >= 0; i--) {
        if (a->digits[i] != b->digits[i]) return a->digits[i] < b->digits[i] ? -1 : 1;
    }
    return 0;
}

ObjBigInt *bigint_from_i64(int64_t v) {
    ObjBigInt *b = bigint_alloc(3);
    if (v == 0) return b;                 /* sign 0, zero digits */
    uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    b->sign = v < 0 ? -1 : 1;
    b->digits[0] = (uint32_t)(u & BIGINT_DIGIT_MASK);
    b->digits[1] = (uint32_t)((u >> 30) & BIGINT_DIGIT_MASK);
    b->digits[2] = (uint32_t)(u >> 60);
    bigint_trim(b);
    return b;
}

ObjBigInt *bigint_from_u64(uint64_t u) {
    ObjBigInt *b = bigint_alloc(3);
    if (u == 0) return b;
    b->sign = 1;
    b->digits[0] = (uint32_t)(u & BIGINT_DIGIT_MASK);
    b->digits[1] = (uint32_t)((u >> 30) & BIGINT_DIGIT_MASK);
    b->digits[2] = (uint32_t)(u >> 60);
    bigint_trim(b);
    return b;
}

ObjBigInt *bigint_copy(const ObjBigInt *a) {
    ObjBigInt *r = bigint_alloc(a->size);
    r->sign = a->sign;
    memcpy(r->digits, a->digits, (size_t)a->size * sizeof(uint32_t));
    return r;
}

ObjBigInt *bigint_from_value(Value v) {
    if (IS_BIGINT(v)) return (ObjBigInt *)AS_OBJ(v);
    if (IS_INT(v)) return bigint_from_i64(AS_INT(v));
    return NULL;
}

/* Takes ownership of `a`: normalizes to an int32 immediate (releasing the
 * heap object) when the magnitude fits, otherwise returns the heap value. */
Value bigint_to_value(ObjBigInt *a) {
    bigint_trim(a);
    /* Magnitude < 2^31 always fits the int32 immediate (int32 covers
     * [-2^31, 2^31-1]); heap bigints therefore never duplicate an int32. */
    if (a->size == 1 || (a->size == 2 && a->digits[1] == 1)) {
        int32_t v = (a->size == 1) ? (int32_t)a->digits[0]
                                   : (int32_t)(a->digits[0] + (1u << 30));
        if (a->sign < 0) v = -v;
        bigint_release(a);
        return make_int(v);
    }
    return make_obj(a);
}

Value bigint_from_i64_value(int64_t v) {
    if (v >= INT32_MIN && v <= INT32_MAX) return make_int((int32_t)v);
    return make_obj((Object *)bigint_from_i64(v));
}

/* ============================================================ */
/* Magnitude helpers                                             */
/* ============================================================ */

/* |a| + |b|, sign +1 */
static ObjBigInt *bigint_mag_add(const ObjBigInt *a, const ObjBigInt *b) {
    int n = (a->size > b->size ? a->size : b->size) + 1;
    ObjBigInt *r = bigint_alloc(n);
    r->sign = 1;
    uint64_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t sum = carry;
        if (i < a->size) sum += a->digits[i];
        if (i < b->size) sum += b->digits[i];
        r->digits[i] = (uint32_t)(sum & BIGINT_DIGIT_MASK);
        carry = sum >> 30;
    }
    bigint_trim(r);
    return r;
}

/* |a| - |b|, requires |a| >= |b|, sign +1 */
static ObjBigInt *bigint_mag_sub(const ObjBigInt *a, const ObjBigInt *b) {
    ObjBigInt *r = bigint_alloc(a->size);
    r->sign = 1;
    int64_t borrow = 0;
    for (int i = 0; i < a->size; i++) {
        int64_t t = (int64_t)a->digits[i] - (int64_t)(i < b->size ? b->digits[i] : 0) - borrow;
        if (t < 0) { t += BIGINT_DIGIT_MASK + 1; borrow = 1; } else borrow = 0;
        r->digits[i] = (uint32_t)t;
    }
    bigint_trim(r);
    return r;
}

static void bigint_mag_increment(ObjBigInt *a) {
    for (int i = 0; i < a->size; i++) {
        uint32_t d = a->digits[i] + 1;
        a->digits[i] = d & BIGINT_DIGIT_MASK;
        if (d > BIGINT_DIGIT_MASK) continue;   /* carry propagates */
        bigint_trim(a);
        return;
    }
    bigint_trim(a);
}

/* |a| * mul + add, sign +1 */
static ObjBigInt *bigint_mag_muladd(const ObjBigInt *a, uint32_t mul, uint32_t add) {
    ObjBigInt *r = bigint_alloc(a->size + 1);
    r->sign = 1;
    uint64_t carry = add;
    for (int i = 0; i < a->size; i++) {
        uint64_t cur = (uint64_t)a->digits[i] * mul + carry;
        r->digits[i] = (uint32_t)(cur & BIGINT_DIGIT_MASK);
        carry = cur >> 30;
    }
    r->digits[a->size] = (uint32_t)carry;
    bigint_trim(r);
    return r;
}

/* ============================================================ */
/* add / sub / mul                                               */
/* ============================================================ */

ObjBigInt *bigint_abs(const ObjBigInt *a) {
    ObjBigInt *r = bigint_copy(a);
    if (r->sign < 0) r->sign = 1;
    return r;
}

ObjBigInt *bigint_neg(const ObjBigInt *a) {
    ObjBigInt *r = bigint_copy(a);
    r->sign = -r->sign;
    return r;
}

ObjBigInt *bigint_add(const ObjBigInt *a, const ObjBigInt *b) {
    if (a->sign == 0) return bigint_copy(b);
    if (b->sign == 0) return bigint_copy(a);
    if (a->sign == b->sign) {
        ObjBigInt *r = bigint_mag_add(a, b);
        r->sign = a->sign;
        return r;
    }
    int c = bigint_cmp_mag(a, b);
    if (c == 0) return bigint_alloc(1);
    const ObjBigInt *x = (c > 0) ? a : b;
    const ObjBigInt *y = (c > 0) ? b : a;
    ObjBigInt *r = bigint_mag_sub(x, y);
    r->sign = (c > 0) ? a->sign : b->sign;
    return r;
}

ObjBigInt *bigint_sub(const ObjBigInt *a, const ObjBigInt *b) {
    if (b->sign == 0) return bigint_copy(a);
    if (a->sign == 0) return bigint_neg(b);
    if (a->sign != b->sign) {
        ObjBigInt *r = bigint_mag_add(a, b);
        r->sign = a->sign;
        return r;
    }
    int c = bigint_cmp_mag(a, b);
    if (c == 0) return bigint_alloc(1);
    const ObjBigInt *x = (c > 0) ? a : b;
    const ObjBigInt *y = (c > 0) ? b : a;
    ObjBigInt *r = bigint_mag_sub(x, y);
    r->sign = (c > 0) ? a->sign : -a->sign;
    return r;
}

ObjBigInt *bigint_mul(const ObjBigInt *a, const ObjBigInt *b) {
    if (a->sign == 0 || b->sign == 0) return bigint_alloc(1);
    ObjBigInt *r = bigint_alloc(a->size + b->size);
    r->sign = (a->sign == b->sign) ? 1 : -1;
    for (int i = 0; i < a->size; i++) {
        uint64_t carry = 0;
        uint64_t ai = a->digits[i];
        for (int j = 0; j < b->size; j++) {
            uint64_t cur = (uint64_t)r->digits[i + j] + ai * b->digits[j] + carry;
            r->digits[i + j] = (uint32_t)(cur & BIGINT_DIGIT_MASK);
            carry = cur >> 30;
        }
        r->digits[i + b->size] = (uint32_t)carry;
    }
    bigint_trim(r);
    return r;
}

/* ============================================================ */
/* Division (single-word fast path + Knuth Algorithm D)           */
/* ============================================================ */

/* |u| / v and |u| % v for a single 30-bit divisor v. */
static ObjBigInt *bigint_divmod_small(const ObjBigInt *u, uint32_t v, ObjBigInt **rem) {
    ObjBigInt *q = bigint_alloc(u->size);
    q->sign = 1;
    uint64_t rem64 = 0;
    for (int i = u->size - 1; i >= 0; i--) {
        rem64 = (rem64 << 30) | u->digits[i];
        q->digits[i] = (uint32_t)(rem64 / v);
        rem64 %= v;
    }
    *rem = bigint_from_u64(rem64);
    bigint_trim(q);
    return q;
}

static void bigint_shift_left_words(const uint32_t *src, int n, int s, uint32_t *dst) {
    uint32_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t w = ((uint64_t)src[i] << s) | carry;
        dst[i] = (uint32_t)(w & BIGINT_DIGIT_MASK);
        carry = (uint32_t)(w >> 30);
    }
    dst[n] = carry;
}

static void bigint_shift_right_words(const uint32_t *src, int n, int s, uint32_t *dst) {
    uint32_t carry = 0;
    for (int i = n - 1; i >= 0; i--) {
        uint32_t cur = src[i];
        dst[i] = (cur >> s) | (carry << (30 - s));
        carry = cur & ((1u << s) - 1);
    }
}

/* Knuth Algorithm D — direct port of CPython's x_divrem (longobject.c):
 * normalize so the divisor's top digit has its MSB at bit 29, then for each
 * quotient digit estimate qhat = top2digits / wm1 and refine it; if the
 * normalized dividend's top digit >= the divisor's top digit (or a carry
 * bit spilled), give the dividend one extra (zero/carry) digit so that
 * dividend < divisor * B^k holds, which the estimator requires. */
static void bigint_divmod_big(const ObjBigInt *u, const ObjBigInt *v,
                              ObjBigInt **out_q, ObjBigInt **out_r) {
    int size_u = u->size;
    int size_w = v->size;                                          /* >= 2 */
    int d = 30 - (int)(32 - bigint_clz32(v->digits[size_w - 1]));  /* 30 - bitlen */

    uint32_t *vv = calloc((size_t)size_w + 1, sizeof(uint32_t));
    uint32_t *uu = calloc((size_t)size_u + 1, sizeof(uint32_t));
    if (!vv || !uu) { fprintf(stderr, "OOM\n"); exit(1); }
    bigint_shift_left_words(v->digits, size_w, d, vv);   /* top carry is 0 (normalized) */
    bigint_shift_left_words(u->digits, size_u, d, uu);   /* uu[size_u] holds the carry */
    uint32_t carry = uu[size_u];

    int size_v = size_u;
    if (carry != 0 || uu[size_v - 1] >= vv[size_w - 1]) {
        uu[size_v] = carry;      /* extra leading digit (0 or carry) */
        size_v++;
    }
    int k = size_v - size_w;     /* quotient digit count */

    ObjBigInt *q = bigint_alloc(k);
    q->sign = 1;
    uint32_t wm1 = vv[size_w - 1];
    uint32_t wm2 = vv[size_w - 2];

    for (int j = k - 1; j >= 0; j--) {
        /* Estimate the quotient digit from the top two digits of the
         * current window; the correction loop makes it exact. */
        uint32_t vtop = uu[j + size_w];
        uint64_t vv2 = ((uint64_t)vtop << 30) | uu[j + size_w - 1];
        uint64_t qhat = vv2 / wm1;
        uint64_t rhat = vv2 % wm1;
        while (((uint64_t)wm2 * qhat) > ((rhat << 30) | uu[j + size_w - 2])) {
            qhat--;
            rhat += wm1;
            if (rhat >= (BIGINT_DIGIT_MASK + 1ull)) break;
        }

        /* Multiply and subtract: uu[j..j+size_w] -= qhat * vv */
        int64_t zhi = 0;
        for (int i = 0; i < size_w; i++) {
            int64_t z = (int64_t)uu[j + i] + zhi - (int64_t)qhat * (int64_t)vv[i];
            uu[j + i] = (uint32_t)z & BIGINT_DIGIT_MASK;
            zhi = z >> 30;   /* arithmetic shift (GCC/Clang) */
        }
        if ((int64_t)vtop + zhi < 0) {
            /* qhat was one too large: add vv back and decrement it. */
            uint64_t c2 = 0;
            for (int i = 0; i < size_w; i++) {
                c2 += (uint64_t)uu[j + i] + vv[i];
                uu[j + i] = (uint32_t)c2 & BIGINT_DIGIT_MASK;
                c2 >>= 30;
            }
            qhat--;
        }
        q->digits[j] = (uint32_t)qhat;
    }

    /* Remainder = uu[0..size_w-1] >> d */
    ObjBigInt *r = bigint_alloc(size_w);
    r->sign = 1;
    if (d == 0) {
        memcpy(r->digits, uu, (size_t)size_w * sizeof(uint32_t));
    } else {
        bigint_shift_right_words(uu, size_w, d, r->digits);
    }
    bigint_trim(r);

    free(uu);
    free(vv);
    bigint_trim(q);
    *out_q = q;
    *out_r = r;
}

void bigint_divmod(const ObjBigInt *a, const ObjBigInt *b,
                   ObjBigInt **out_q, ObjBigInt **out_r) {
    /* Caller guarantees b != 0. */
    if (b->sign == 0) { *out_q = bigint_alloc(1); *out_r = bigint_alloc(1); return; }
    if (bigint_cmp_mag(a, b) < 0) {
        *out_q = bigint_alloc(1);            /* zero quotient */
        *out_r = bigint_copy(a);
        return;
    }
    ObjBigInt *qm, *rm;
    if (b->size == 1) {
        qm = bigint_divmod_small(a, b->digits[0], &rm);
    } else {
        bigint_divmod_big(a, b, &qm, &rm);
    }
    /* Truncating division: the remainder takes the dividend's sign. */
    qm->sign = (a->sign == b->sign) ? 1 : -1;
    rm->sign = a->sign;
    bigint_trim(qm);
    bigint_trim(rm);
    *out_q = qm;
    *out_r = rm;
}

/* ============================================================ */
/* Power                                                          */
/* ============================================================ */

bool bigint_pow_too_big(const ObjBigInt *base, const ObjBigInt *exp) {
    if (exp->sign <= 0 || base->sign == 0) return false;               /* tiny results */
    if (base->size == 1 && base->digits[0] == 1) return false;         /* +/-1 */
    int64_t expv;
    if (!bigint_get_i64(exp, &expv)) return true;
    /* result words ~= bit_length(base) * exp / 30 */
    if (expv > ((int64_t)BIGINT_POW_MAX_WORDS * 30) / bigint_bit_length(base)) return true;
    return false;
}

ObjBigInt *bigint_pow(const ObjBigInt *base, const ObjBigInt *exp) {
    /* Requires exp >= 0 and !bigint_pow_too_big(). */
    if (exp->sign == 0) return bigint_from_i64(1);
    if (base->sign == 0) return bigint_alloc(1);
    if (base->size == 1 && base->digits[0] == 1) {
        bool odd = (exp->digits[0] & 1u) != 0;
        return bigint_from_i64(base->sign < 0 && odd ? -1 : 1);
    }
    ObjBigInt *acc = bigint_from_i64(1);
    ObjBigInt *sq = bigint_copy(base);
    int bits = bigint_bit_length(exp);
    for (int i = 0; i < bits; i++) {
        if (exp->digits[i / 30] & (1u << (i % 30))) {
            ObjBigInt *t = bigint_mul(acc, sq);
            bigint_release(acc);
            acc = t;
        }
        if (i + 1 < bits) {
            ObjBigInt *t = bigint_mul(sq, sq);
            bigint_release(sq);
            sq = t;
        }
    }
    bigint_release(sq);
    return acc;
}

/* ============================================================ */
/* Shifts and bitwise ops (infinite two's complement)             */
/* ============================================================ */

ObjBigInt *bigint_shl(const ObjBigInt *a, int64_t bits) {
    if (bits < 0) return bigint_shr(a, -bits);
    if (a->sign == 0 || bits == 0) return bigint_copy(a);
    int ws = (int)(bits / 30);
    int bs = (int)(bits % 30);
    ObjBigInt *r = bigint_alloc(a->size + ws + 1);
    r->sign = a->sign;
    uint64_t carry = 0;
    for (int i = 0; i < a->size; i++) {
        uint64_t w = ((uint64_t)a->digits[i] << bs) | carry;
        r->digits[i + ws] = (uint32_t)(w & BIGINT_DIGIT_MASK);
        carry = w >> 30;
    }
    r->digits[a->size + ws] = (uint32_t)carry;
    bigint_trim(r);
    return r;
}

ObjBigInt *bigint_shr(const ObjBigInt *a, int64_t bits) {
    if (bits < 0) return bigint_shl(a, -bits);
    if (a->sign == 0 || bits == 0) return bigint_copy(a);
    if (bits >= bigint_bit_length(a)) {
        /* All magnitude bits shifted out: 0 for positives, -1 for negatives
         * (floor semantics: -5 >> 100 == -1). */
        return bigint_from_i64(a->sign < 0 ? -1 : 0);
    }
    int ws = (int)(bits / 30);
    int bs = (int)(bits % 30);
    bool sticky = false;
    for (int i = 0; i < ws && i < a->size; i++) {
        if (a->digits[i]) { sticky = true; break; }
    }
    ObjBigInt *r = bigint_alloc(a->size - ws);
    if (bs == 0) {
        for (int i = ws; i < a->size; i++) r->digits[i - ws] = a->digits[i];
    } else {
        if (ws < a->size && (a->digits[ws] & ((1u << bs) - 1))) sticky = true;
        for (int i = ws; i < a->size; i++) {
            uint32_t hi = a->digits[i] >> bs;
            uint32_t lo = (i + 1 < a->size)
                ? (uint32_t)(((uint64_t)a->digits[i + 1] << (30 - bs)) & BIGINT_DIGIT_MASK)
                : 0;
            r->digits[i - ws] = hi | lo;
        }
    }
    bigint_trim(r);
    r->sign = 1;
    if (a->sign < 0) {
        r->sign = -1;
        if (sticky) bigint_mag_increment(r);
    }
    return r;
}

/* Two's complement digit i of a (infinite sign extension). */
static void bigint_to_twos(const ObjBigInt *a, uint32_t *out, int n) {
    uint32_t borrow = 1;   /* negative values: -m == ~(m-1) */
    for (int i = 0; i < n; i++) {
        uint32_t d = (i < a->size) ? a->digits[i] : 0;
        if (a->sign < 0) {
            uint32_t l;
            if (d < borrow) { l = d + BIGINT_DIGIT_MASK; }   /* d - 1 mod base */
            else { l = d - borrow; borrow = 0; }
            out[i] = ~l & BIGINT_DIGIT_MASK;
        } else {
            out[i] = d;   /* i >= size -> zero sign extension */
        }
    }
}

static ObjBigInt *bigint_from_twos(const uint32_t *t, int n) {
    bool neg = (t[n - 1] >> 29) & 1;
    ObjBigInt *r = bigint_alloc(n);
    if (!neg) {
        memcpy(r->digits, t, (size_t)n * sizeof(uint32_t));
        r->sign = 1;
    } else {
        r->sign = -1;
        uint32_t carry = 1;
        for (int i = 0; i < n; i++) {
            uint32_t w = (~t[i] & BIGINT_DIGIT_MASK) + carry;
            if (w >= (BIGINT_DIGIT_MASK + 1u)) { w -= BIGINT_DIGIT_MASK + 1u; carry = 1; }
            else carry = 0;
            r->digits[i] = w;
        }
    }
    bigint_trim(r);
    return r;
}

static ObjBigInt *bigint_bitwise(const ObjBigInt *a, const ObjBigInt *b, int op) {
    int n = (a->size > b->size ? a->size : b->size) + 1;
    uint32_t *ta = malloc((size_t)n * sizeof(uint32_t));
    uint32_t *tb = malloc((size_t)n * sizeof(uint32_t));
    uint32_t *tr = malloc((size_t)n * sizeof(uint32_t));
    if (!ta || !tb || !tr) { fprintf(stderr, "OOM\n"); exit(1); }
    bigint_to_twos(a, ta, n);
    bigint_to_twos(b, tb, n);
    for (int i = 0; i < n; i++) {
        if (op == 0) tr[i] = ta[i] & tb[i];
        else if (op == 1) tr[i] = ta[i] | tb[i];
        else tr[i] = ta[i] ^ tb[i];
        tr[i] &= BIGINT_DIGIT_MASK;
    }
    ObjBigInt *r = bigint_from_twos(tr, n);
    free(ta);
    free(tb);
    free(tr);
    return r;
}

ObjBigInt *bigint_and(const ObjBigInt *a, const ObjBigInt *b) { return bigint_bitwise(a, b, 0); }
ObjBigInt *bigint_or (const ObjBigInt *a, const ObjBigInt *b) { return bigint_bitwise(a, b, 1); }
ObjBigInt *bigint_xor(const ObjBigInt *a, const ObjBigInt *b) { return bigint_bitwise(a, b, 2); }

ObjBigInt *bigint_not(const ObjBigInt *a) {
    int n = a->size + 1;
    uint32_t *t = malloc((size_t)n * sizeof(uint32_t));
    if (!t) { fprintf(stderr, "OOM\n"); exit(1); }
    bigint_to_twos(a, t, n);
    for (int i = 0; i < n; i++) t[i] = ~t[i] & BIGINT_DIGIT_MASK;
    ObjBigInt *r = bigint_from_twos(t, n);
    free(t);
    return r;
}

/* ============================================================ */
/* Comparisons                                                    */
/* ============================================================ */

int bigint_cmp(const ObjBigInt *a, const ObjBigInt *b) {
    if (a->sign != b->sign) return a->sign < b->sign ? -1 : 1;
    if (a->sign == 0) return 0;
    int c = bigint_cmp_mag(a, b);
    return a->sign > 0 ? c : -c;
}

static int bigint_cmp_mag_u64(const ObjBigInt *a, uint64_t u) {
    if (a->size > 2) return 1;
    uint64_t mag = a->digits[0];
    if (a->size > 1) mag |= (uint64_t)a->digits[1] << 30;
    return mag < u ? -1 : mag > u ? 1 : 0;
}

int bigint_cmp_value(const ObjBigInt *a, Value b) {
    if (IS_BIGINT(b)) return bigint_cmp(a, (const ObjBigInt *)AS_OBJ(b));
    if (IS_DOUBLE(b)) return bigint_cmp_f64(a, AS_DOUBLE(b));
    int64_t s = AS_INT(b);
    if (a->sign == 0) return s == 0 ? 0 : (s > 0 ? -1 : 1);
    if (s == 0) return a->sign > 0 ? 1 : -1;
    if ((a->sign > 0) != (s > 0)) return a->sign > 0 ? 1 : -1;
    uint64_t smag = s < 0 ? (uint64_t)(-(s + 1)) + 1 : (uint64_t)s;
    int c = bigint_cmp_mag_u64(a, smag);
    return a->sign > 0 ? c : -c;
}

/* ============================================================ */
/* Exact double conversions                                       */
/* ============================================================ */

double bigint_to_f64(const ObjBigInt *a) {
    /* Port of CPython's _PyLong_Frexp / PyLong_AsDouble: round the top
     * DBL_MANT_DIG+2 bits to the nearest even multiple of 4 (half-to-even),
     * then rescale with ldexp. Returns ±HUGE_VAL when the value exceeds the
     * double range (callers decide whether to raise OverflowError). */
    static const int half_even_correction[8] = {0, -1, -2, 1, 0, -1, 2, 1};
    const int DBL_MANT_DIG = 53;
    const int KEEP_BITS = DBL_MANT_DIG + 2;          /* 55 */
    const double BASE = 1073741824.0;                /* 2^30 */
    const double SCALE = 4.0 * 9007199254740992.0;   /* 2^55 */

    if (a->sign == 0) return 0.0;
    int a_size = a->size;
    uint32_t top = a->digits[a_size - 1];

    /* a_bits = bit_length(a) */
    long long a_bits = (long long)(a_size - 1) * 30 + (long long)(32 - bigint_clz32(top));

    /* Shift the top KEEP_BITS bits of a into x_digits[0:x_size] (fixed, small). */
    uint32_t x_digits[3] = {0};
    int x_size;
    int i;
    if (a_bits <= KEEP_BITS) {
        int shift_digits = (int)((KEEP_BITS - a_bits) / 30);
        int shift_bits = (int)((KEEP_BITS - a_bits) % 30);
        x_size = shift_digits;
        uint32_t carry = 0;
        for (i = 0; i < a_size; i++) {
            uint64_t acc = ((uint64_t)a->digits[i] << shift_bits) | carry;
            x_digits[x_size + i] = (uint32_t)(acc & BIGINT_DIGIT_MASK);
            carry = (uint32_t)(acc >> 30);
        }
        x_size += a_size;
        x_digits[x_size++] = carry;
    } else {
        int shift_digits = (int)((a_bits - KEEP_BITS) / 30);
        int shift_bits = (int)((a_bits - KEEP_BITS) % 30);
        int m = a_size - shift_digits;
        x_size = m;
        /* v_rshift: shift the top m digits right by shift_bits; rem = dropped bits. */
        uint32_t carry = 0;
        uint32_t mask = (1u << shift_bits) - 1u;
        for (i = m - 1; i >= 0; i--) {
            uint64_t acc = ((uint64_t)carry << 30) | a->digits[i + shift_digits];
            carry = (uint32_t)acc & mask;
            x_digits[i] = (uint32_t)(acc >> shift_bits);
        }
        /* Sticky bit: any nonzero bit shifted out must keep the LSB set. */
        if (carry) x_digits[0] |= 1;
        else while (shift_digits > 0)
                if (a->digits[--shift_digits]) { x_digits[0] |= 1; break; }
    }

    /* Round to nearest multiple of 4, ties to even (the kept 2 extra bits). */
    x_digits[0] += (uint32_t)half_even_correction[x_digits[0] & 7];

    /* Horner into a double, then rescale to a significand in [0.5, 1). */
    double dx = x_digits[--x_size];
    while (x_size > 0) dx = dx * BASE + x_digits[--x_size];
    dx /= SCALE;
    if (dx == 1.0) { dx = 0.5; a_bits += 1; }

    if (a_bits > 1024) return a->sign < 0 ? -HUGE_VAL : HUGE_VAL;  /* DBL_MAX_EXP */
    return ldexp(a->sign < 0 ? -dx : dx, (int)a_bits);
}

/* Magnitude-only right shift (a must be non-negative) tracking whether any
 * dropped bit was nonzero (CPython's v_rshift + sticky scan). */
static ObjBigInt *bigint_shr_sticky(const ObjBigInt *a, int64_t bits, bool *sticky) {
    *sticky = false;
    int ws = (int)(bits / 30);
    int bs = (int)(bits % 30);
    for (int i = 0; i < ws && i < a->size; i++) {
        if (a->digits[i]) { *sticky = true; break; }
    }
    ObjBigInt *r = bigint_alloc(a->size - ws);
    if (bs == 0) {
        for (int i = ws; i < a->size; i++) r->digits[i - ws] = a->digits[i];
    } else {
        if (a->digits[ws] & ((1u << bs) - 1)) *sticky = true;
        for (int i = ws; i < a->size; i++) {
            uint32_t hi = a->digits[i] >> bs;
            uint32_t lo = (i + 1 < a->size)
                ? (uint32_t)(((uint64_t)a->digits[i + 1] << (30 - bs)) & BIGINT_DIGIT_MASK)
                : 0;
            r->digits[i - ws] = hi | lo;
        }
    }
    r->sign = 1;
    bigint_trim(r);
    return r;
}

/* Correctly-rounded true division — direct port of CPython's
 * long_true_divide (Objects/longobject.c, 3.11):  compute
 * x = floor(2**-shift * |a| / |b|) in integer arithmetic with a sticky
 * "inexact" flag, round x to the nearest double using round-half-to-even
 * on the extra bits, and rescale with ldexp. */
double bigint_truediv(const ObjBigInt *a, const ObjBigInt *b) {
    const int DBL_MANT_DIG = 53;
    const int DBL_MAX_EXP = 1024;    /* IEEE-754 double */
    const int DBL_MIN_EXP = -1021;

    if (a->sign == 0) return b->sign < 0 ? -0.0 : 0.0;
    bool negate = (a->sign < 0) ^ (b->sign < 0);

    /* Fast path: both magnitudes below 2^53 are exact in a double, and
     * floating-point division is correctly rounded. */
    if (bigint_bit_length(a) <= DBL_MANT_DIG && bigint_bit_length(b) <= DBL_MANT_DIG) {
        uint64_t ma = a->digits[0];
        if (a->size > 1) ma |= (uint64_t)a->digits[1] << 30;
        uint64_t mb = b->digits[0];
        if (b->size > 1) mb |= (uint64_t)b->digits[1] << 30;
        double d = (double)ma / (double)mb;
        return negate ? -d : d;
    }

    long long diff = (long long)bigint_bit_length(a) - (long long)bigint_bit_length(b);
    if (diff > DBL_MAX_EXP) return negate ? -HUGE_VAL : HUGE_VAL;      /* overflow */
    if (diff < DBL_MIN_EXP - DBL_MANT_DIG - 1) return negate ? -0.0 : 0.0;

    int shift = (diff > DBL_MIN_EXP ? (int)diff : DBL_MIN_EXP) - DBL_MANT_DIG - 2;
    bool inexact = false;

    /* x = |a * 2^-shift| (magnitudes; a, b >= 1 here) */
    ObjBigInt *x;
    if (shift <= 0) {
        x = bigint_shl(a, -(int64_t)shift);   /* low bits zero: shift is exact */
    } else {
        x = bigint_shr_sticky(a, shift, &inexact);
    }

    /* x //= b; a nonzero remainder marks the division inexact. */
    ObjBigInt *rem;
    {
        ObjBigInt *q;
        bigint_divmod(x, b, &q, &rem);
        bigint_release(x);
        x = q;
    }
    if (!bigint_is_zero(rem)) inexact = true;
    bigint_release(rem);

    int x_bits = bigint_bit_length(x);
    /* Number of extra bits to round away (guaranteed 2 or 3). */
    int extra_bits = (x_bits > DBL_MIN_EXP - shift ? x_bits : DBL_MIN_EXP - shift) - DBL_MANT_DIG;

    /* Round x to nearest double, half-to-even, by editing its low digit. */
    uint32_t mask = 1u << (extra_bits - 1);
    uint32_t low = x->digits[0] | (inexact ? 1u : 0u);
    if ((low & mask) && (low & (3u * mask - 1u))) low += mask;
    x->digits[0] = low & ~(2u * mask - 1u);

    /* Convert x to a double (the remaining value is exactly representable
     * in 53 bits); then rescale. */
    double dx = x->digits[--x->size];
    while (x->size > 0) dx = dx * 1073741824.0 + x->digits[--x->size];
    bigint_release(x);

    /* Check whether ldexp will overflow a double. */
    if (shift + x_bits >= DBL_MAX_EXP &&
        (shift + x_bits > DBL_MAX_EXP || dx == ldexp(1.0, x_bits)))
        return negate ? -HUGE_VAL : HUGE_VAL;
    double result = ldexp(dx, shift);
    return negate ? -result : result;
}

/* Exact comparison of a bigint against a double (2 = incomparable, NaN). */
int bigint_cmp_f64(const ObjBigInt *a, double d) {
    if (isnan(d)) return 2;
    if (a->sign == 0) {
        if (d == 0.0) return 0;
        return d > 0 ? -1 : 1;
    }
    if (isinf(d)) return d > 0 ? -1 : 1;
    if (d == 0.0) return a->sign > 0 ? 1 : -1;
    bool dneg = signbit(d);
    if ((a->sign > 0) != !dneg) return a->sign > 0 ? 1 : -1;
    /* Compare magnitudes exactly: decompose |d| = frac * 2^(e-53). */
    int e;
    double m = frexp(fabs(d), &e);
    uint64_t frac = (uint64_t)ldexp(m, 53);
    int shift = e - 53;
    int c;
    if (shift >= 0) {
        ObjBigInt *ft = bigint_from_u64(frac);
        ObjBigInt *ft2 = bigint_shl(ft, shift);
        c = bigint_cmp_mag(a, ft2);
        bigint_release(ft);
        bigint_release(ft2);
    } else {
        int s = -shift;
        if (s > 63) return a->sign > 0 ? 1 : -1;   /* |d| < 1 */
        uint64_t low = frac & ((1ull << s) - 1);
        uint64_t q = frac >> s;
        if (q == 0) return a->sign > 0 ? 1 : -1;
        c = bigint_cmp_mag_u64(a, q);
        if (c == 0 && low != 0) c = -1;            /* |a| == floor(|d|) < |d| */
    }
    return (a->sign > 0) ? c : -c;
}

/* Exact int64 extraction: false when the magnitude does not fit. */
bool bigint_get_i64(const ObjBigInt *a, int64_t *out) {
    if (a->sign == 0) { *out = 0; return true; }
    if (a->size > 3) return false;
    if (a->size == 3 && a->digits[2] >= 8) return false;   /* magnitude >= 2^63 */
    uint64_t mag = a->digits[0];
    if (a->size > 1) mag |= (uint64_t)a->digits[1] << 30;
    if (a->size > 2) mag |= (uint64_t)a->digits[2] << 60;
    if (a->sign > 0) {
        if (mag > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)mag;
    } else {
        if (mag > (uint64_t)INT64_MAX + 1) return false;
        *out = (mag == (uint64_t)INT64_MAX + 1) ? INT64_MIN : -(int64_t)mag;
    }
    return true;
}

/* ============================================================ */
/* Hash (CPython-style: value mod 2^61-1, folded per digit)      */
/* ============================================================ */

uint32_t bigint_hash(const ObjBigInt *a) {
    const uint64_t P = (1ull << 61) - 1;
    uint64_t h = 0;
    for (int i = 0; i < a->size; i++) {
        /* h * 2^30 = (h>>31)*2^61 + (h&2^31-1)*2^30 ≡ hi + lo*2^30 (mod P) */
        uint64_t hi = h >> 31;
        uint64_t lo = h & 0x7FFFFFFF;
        uint64_t t = hi + (lo << 30) + a->digits[i];
        if (t >= P) t -= P;
        h = t;
    }
    if (a->sign < 0 && h != 0) h = P - h;
    return (uint32_t)h;
}

/* ============================================================ */
/* Decimal parsing / rendering                                    */
/* ============================================================ */

bool bigint_from_decimal(const char *s, size_t len, Value *out) {
    size_t i = 0;
    while (i < len && isspace((unsigned char)s[i])) i++;
    bool neg = false;
    if (i < len && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; i++; }
    size_t dstart = i;
    while (i < len && isdigit((unsigned char)s[i])) i++;
    if (i == dstart) return false;           /* no digits */
    size_t dend = i;
    while (i < len && isspace((unsigned char)s[i])) i++;
    if (i != len) return false;              /* trailing junk */

    /* Fast path: short literals fit an int64 without heap arithmetic. */
    size_t n = dend - dstart;
    if (n <= 18) {
        uint64_t u = 0;
        for (size_t k = dstart; k < dend; k++) u = u * 10 + (uint64_t)(s[k] - '0');
        *out = bigint_from_i64_value(neg ? -(int64_t)u : (int64_t)u);
        return true;
    }

    ObjBigInt *m = bigint_alloc(1);          /* zero */
    for (size_t k = dstart; k < dend; k++) {
        ObjBigInt *t = bigint_mag_muladd(m, 10, (uint32_t)(s[k] - '0'));
        bigint_release(m);
        m = t;
    }
    if (neg && m->sign != 0) m->sign = -1;
    *out = bigint_to_value(m);
    return true;
}

char *bigint_to_decimal(const ObjBigInt *a) {
    if (a->sign == 0) return strdup("0");
    int cap = a->size * 10 + 2;              /* ~9.03 decimal digits per word */
    char *buf = malloc((size_t)cap);
    uint32_t *work = malloc((size_t)a->size * sizeof(uint32_t));
    if (!buf || !work) { fprintf(stderr, "OOM\n"); exit(1); }
    memcpy(work, a->digits, (size_t)a->size * sizeof(uint32_t));
    int wn = a->size;

    /* Repeatedly divide by 10^9, collecting chunks (least significant first). */
    static const uint32_t CHUNK = 1000000000u;
    int max_chunks = a->size * 2 + 2;
    uint32_t *chunks = malloc((size_t)max_chunks * sizeof(uint32_t));
    if (!chunks) { fprintf(stderr, "OOM\n"); exit(1); }
    int nc = 0;
    while (wn > 1 || work[0] > 0) {
        uint64_t rem = 0;
        for (int i = wn - 1; i >= 0; i--) {
            uint64_t cur = (rem << 30) | work[i];
            work[i] = (uint32_t)(cur / CHUNK);
            rem = cur % CHUNK;
        }
        while (wn > 1 && work[wn - 1] == 0) wn--;
        chunks[nc++] = (uint32_t)rem;
    }

    char *p = buf;
    if (a->sign < 0) *p++ = '-';
    p += sprintf(p, "%u", chunks[nc - 1]);
    for (int i = nc - 2; i >= 0; i--) p += sprintf(p, "%09u", chunks[i]);
    *p = '\0';
    free(work);
    free(chunks);
    return buf;
}

/* ============================================================ */
/* Exact float -> bigint                                         */
/* ============================================================ */

bool bigint_from_f64(double d, Value *out) {
    if (isnan(d) || isinf(d)) return false;
    if (d == 0.0) { *out = make_int(0); return true; }
    bool neg = signbit(d);
    double f = trunc(fabs(d));               /* truncate toward zero */
    if (f == 0.0) { *out = make_int(0); return true; }
    /* f = m * 2^e with a 53-bit integral mantissa (f is an integer). */
    int e;
    double m = frexp(f, &e);
    uint64_t frac = (uint64_t)ldexp(m, 53);
    ObjBigInt *mag = bigint_from_u64(frac);
    ObjBigInt *r = bigint_shl(mag, (int64_t)(e - 53));   /* negative mirrors to shr; exact */
    bigint_release(mag);
    if (neg && r->sign != 0) r->sign = -1;
    *out = bigint_to_value(r);
    return true;
}

/* ============================================================ */
/* Value-level dispatch (errors set vm->last_exception)          */
/* ============================================================ */

static bool bigint_numberish(Value v) { return IS_INT(v) || IS_BIGINT(v) || IS_DOUBLE(v); }

static void bigint_raise(VM *vm, void *cls, const char *msg) {
    vm->last_exception = make_exception_instance(vm, cls, msg);
}

Value bigint_unary_value(VM *vm, VMOperation op, Value a) {
    (void)vm;
    if (op == VM_OP_NEG) {
        if (IS_INT(a)) {
            int32_t v = AS_INT(a);
            if (v == INT32_MIN) return bigint_from_i64_value(-(int64_t)v);
            return make_int(-v);
        }
        if (IS_BIGINT(a)) return bigint_to_value(bigint_neg((ObjBigInt *)AS_OBJ(a)));
        return make_null();
    }
    if (op == VM_OP_BNOT) {
        if (IS_INT(a)) {
            int32_t v = AS_INT(a);
            return bigint_from_i64_value(~(int64_t)v);
        }
        if (IS_BIGINT(a)) return bigint_to_value(bigint_not((ObjBigInt *)AS_OBJ(a)));
        return make_null();
    }
    return make_null();
}

/* Both operands are int32 or bigint values here. */
static Value bigint_int_binary(VM *vm, VMOperation op, ObjBigInt *x, ObjBigInt *y) {
    switch (op) {
        case VM_OP_ADD: return bigint_to_value(bigint_add(x, y));
        case VM_OP_SUB: return bigint_to_value(bigint_sub(x, y));
        case VM_OP_MUL: return bigint_to_value(bigint_mul(x, y));
        case VM_OP_DIV:
            if (bigint_is_zero(y)) {
                bigint_raise(vm, py_fe(vm)->runtime_error_class, "division by zero");
                return make_null();
            }
            {
                double d = bigint_truediv(x, y);
                if (isinf(d)) {
                    /* Both operands are finite, so inf means range overflow
                     * (CPython: "integer division result too large for a float"). */
                    bigint_raise(vm, py_fe(vm)->overflow_error_class,
                        "integer division result too large for a float");
                    return make_null();
                }
                return make_double(d);
            }
        case VM_OP_MOD:
        case VM_OP_IDIV: {
            if (bigint_is_zero(y)) {
                bigint_raise(vm, op == VM_OP_MOD
                    ? py_fe(vm)->exception_class
                    : py_fe(vm)->runtime_error_class,
                    op == VM_OP_MOD ? "mod/0" : "division by zero");
                return make_null();
            }
            ObjBigInt *q, *rem;
            bigint_divmod(x, y, &q, &rem);
            /* Floor semantics: adjust so the remainder takes the divisor's
             * sign, matching CPython's // and %. */
            if (rem->sign != 0 && rem->sign != y->sign) {
                ObjBigInt *one = bigint_from_i64(1);
                ObjBigInt *q1 = bigint_sub(q, one);
                ObjBigInt *r1 = bigint_add(rem, y);
                bigint_release(one);
                bigint_release(q);
                bigint_release(rem);
                q = q1;
                rem = r1;
            }
            if (op == VM_OP_MOD) {
                Value out = bigint_to_value(rem);
                bigint_release(q);
                return out;
            }
            Value out = bigint_to_value(q);
            bigint_release(rem);
            return out;
        }
        case VM_OP_POW: {
            if (y->sign < 0) {
                return make_double(pow(bigint_to_f64(x), bigint_to_f64(y)));
            }
            if (bigint_pow_too_big(x, y)) {
                bigint_raise(vm, py_fe(vm)->overflow_error_class, "pow() result too large");
                return make_null();
            }
            return bigint_to_value(bigint_pow(x, y));
        }
        case VM_OP_BAND: return bigint_to_value(bigint_and(x, y));
        case VM_OP_BOR:  return bigint_to_value(bigint_or(x, y));
        case VM_OP_BXOR: return bigint_to_value(bigint_xor(x, y));
        case VM_OP_SHL:
        case VM_OP_SHR: {
            int64_t bits;
            if (!bigint_get_i64(y, &bits)) {
                bigint_raise(vm, py_fe(vm)->overflow_error_class, "shift count too large");
                return make_null();
            }
            if (bits < 0) {
                bigint_raise(vm, py_fe(vm)->value_error_class, "negative shift count");
                return make_null();
            }
            if (bits > BIGINT_SHIFT_MAX_BITS) {
                bigint_raise(vm, py_fe(vm)->overflow_error_class, "shift count too large");
                return make_null();
            }
            return bigint_to_value(op == VM_OP_SHL ? bigint_shl(x, bits) : bigint_shr(x, bits));
        }
        default:
            return make_null();
    }
}

Value bigint_binary_value(VM *vm, VMOperation op, Value a, Value b) {
    if (!bigint_numberish(a) || !bigint_numberish(b)) return make_null();
    if (IS_DOUBLE(a) || IS_DOUBLE(b)) {
        double da = as_double(a), db = as_double(b);
        switch (op) {
            case VM_OP_ADD:  return make_double(da + db);
            case VM_OP_SUB:  return make_double(da - db);
            case VM_OP_MUL:  return make_double(da * db);
            case VM_OP_DIV:  return make_double(da / db);
            case VM_OP_MOD:  return make_double(fmod(da, db));
            case VM_OP_IDIV: return make_double(floor(da / db));
            case VM_OP_POW:  return make_double(pow(da, db));
            default:         return make_null();   /* bitwise/shift need integers */
        }
    }
    ObjBigInt *xt = NULL, *yt = NULL;
    ObjBigInt *x = bigint_from_value(a);
    ObjBigInt *y = bigint_from_value(b);
    if (IS_INT(a)) xt = x;
    if (IS_INT(b)) yt = y;
    Value out = bigint_int_binary(vm, op, x, y);
    if (xt) bigint_release(xt);
    if (yt) bigint_release(yt);
    return out;
}