#ifndef LUNA_VALUE_H
#define LUNA_VALUE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct luna_State;

#if defined(__GNUC__) || defined(__clang__)
#define LUNA_LIKELY(x)   __builtin_expect(!!(x), 1)
#define LUNA_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LUNA_LIKELY(x)   (x)
#define LUNA_UNLIKELY(x) (x)
#endif

/* Forward declarations */
typedef struct Object   Object;
typedef struct ObjInt64 ObjInt64;
typedef struct Type     Type;   /* MOP vtable (defined in luna/object.h) */
struct Chunk;
struct VM;

/* ============================================================
 * NaN-boxed value — language-agnostic core.
 *
 * Value layout (64-bit):
 *   Real doubles are stored as raw IEEE-754 bits.
 *   All other values are tagged with a quiet-NaN signature (0x7FF8 in
 *   the top 16 bits) so they are never confused with valid doubles.
 *
 *   The only inline tags are a 3-bit sub-tag in bits 0-2 (malloc-aligned
 *   pointers => subtag 0 = a heap object). There is NO per-type tag: the
 *   object's type lives in the heap (the language frontend's object model),
 *   not in the 64-bit value. This keeps the VM language-agnostic.
 *
 *   HeapObject:  QNAN | ptr          (ptr 8-byte aligned => low 3 bits = 000)
 *   int32_t:     QNAN | TAG_INT | (i << 3)
 *   int64_t:     heap object (Object.type in the frontend model)
 * ============================================================ */

typedef uint64_t Value;

/* Quiet NaN base: sign=0, exponent=all-1s, quiet-bit=1. Top 16 bits = 0x7FF8.
 * Objects carry no inline type bits; the type lives in the heap object.
 * NaN values are normalized to payload>=1 via make_double() to avoid collision. */
#define QNAN_TAG     ((uint64_t)0x7ff8000000000000)

/* 3-bit sub-tag in bits 0-2 (malloc-aligned ptrs => subtag 0 = HeapObject).
 * Semantics grouped: 001/010 null-ish, 011/100 bools, 101 int, 110/111 sentinels. */
#define TAG_HEAP      0  /* 000 -> HeapObject (no inline type) */
#define TAG_NIL       1  /* 001 -> None / null */
#define TAG_NOTIMPL   2  /* 010 -> Undefined (JS) / NotImplemented (Python) */
#define TAG_TRUE      3  /* 011 -> True */
#define TAG_FALSE     4  /* 100 -> False */
#define TAG_INT       5  /* 101 -> Int32 immediate (payload in bits 3+) */
#define TAG_EMPTY     6  /* 110 -> The Hole / dict EMPTY (never-occupied) slot */
#define TAG_TOMB      7  /* 111 -> dict TOMBSTONE (deleted) slot */

#define IS_DOUBLE(v) LUNA_LIKELY( \
    (((v) & 0x7FF8000000000000ULL) != 0x7FF8000000000000ULL) || \
    ((v) == 0x7FF800000000000AULL) \
)

#define IS_OBJ(v)    (((v) & (QNAN_TAG | 7)) == QNAN_TAG)
#define IS_NIL(v)    ((v) == (QNAN_TAG | TAG_NIL))
#define IS_NOTIMPL(v) ((v) == (QNAN_TAG | TAG_NOTIMPL))
#define IS_TRUE(v)   ((v) == (QNAN_TAG | TAG_TRUE))
#define IS_FALSE(v)  ((v) == (QNAN_TAG | TAG_FALSE))
#define IS_BOOL(v)   (IS_TRUE(v) || IS_FALSE(v))
#define IS_INT(v)    (((v) & (QNAN_TAG | 7)) == (QNAN_TAG | TAG_INT))
#define IS_EMPTY(v)  ((v) == (QNAN_TAG | TAG_EMPTY))
#define IS_TOMB(v)   ((v) == (QNAN_TAG | TAG_TOMB))
#define IS_NUMBER(v) (IS_DOUBLE(v) || IS_INT(v) || IS_INT64(v))

#define IS_INF(v)    (((v) & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL && ((v) & 0x000fffffffffffffULL) == 0)
#define IS_POS_INF(v) ((v) == 0x7ff0000000000000ULL)
#define IS_NEG_INF(v) ((v) == 0xfff0000000000000ULL)
#define IS_NAN(v)    ((v) == 0x7ff800000000000AULL)

#define make_pos_inf() ((Value)0x7ff0000000000000ULL)
#define make_neg_inf() ((Value)0xfff0000000000000ULL)

#define AS_OBJ(v)    ((Object*)(uintptr_t)((v) & 0x00007fffffffffffULL))
#define AS_BOOL(v)   IS_TRUE(v)
#define AS_INT(v)    ((int32_t)((v) >> 3))

static inline Value make_double(double d) {
    Value v;
    memcpy(&v, &d, sizeof(v));
    uint64_t abs_v = v & 0x7FFFFFFFFFFFFFFFULL;
    if (abs_v >= 0x7FF0000000000000ULL) {
        if (abs_v != 0x7FF0000000000000ULL) {
            /* Normalize every NaN to one quiet NaN (payload 0x0A). Its low 3 bits
               (subtag 2) coincide with the NOTIMPL slot, but it is distinguished
               from immediates by exact equality (IS_DOUBLE / IS_OBJ / IS_NOTIMPL). */
            return 0x7FF800000000000AULL;
        }
    }
    return v;
}
static inline double AS_DOUBLE(Value v) {
    double d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

static inline double as_double(Value v);
static inline int64_t as_int64(Value v);

#define RETURN_EXT_DOUBLE(d) make_double((double)(d))

#define INT_VAL(i)   (QNAN_TAG | TAG_INT | ((uint64_t)(uint32_t)(i) << 3))
#define NIL_VAL      (QNAN_TAG | TAG_NIL)
#define NOTIMPL_VAL  (QNAN_TAG | TAG_NOTIMPL)
#define TRUE_VAL     (QNAN_TAG | TAG_TRUE)
#define FALSE_VAL    (QNAN_TAG | TAG_FALSE)
#define BOOL_VAL(b)  ((b) ? TRUE_VAL : FALSE_VAL)
#define EMPTY_VAL    (QNAN_TAG | TAG_EMPTY)
#define TOMBSTONE_VAL (QNAN_TAG | TAG_TOMB)

#define make_null()   NIL_VAL
#define make_bool(b)  BOOL_VAL(b)
#define make_int(i)   INT_VAL((int32_t)(i))

/* Generic heap-object constructor: just a pointer in the NaN space. The type
 * lives in the heap object (frontend-owned); no inline type bits are stored. */
static inline Value make_obj(void *ptr) {
    if (!ptr) return NIL_VAL;
    return QNAN_TAG | (uint64_t)(uintptr_t)ptr;
}
#define OBJ_VAL(obj) make_obj(obj)

/* Generic value utilities (frontend-independent). Defined in value.c. */
bool  is_null(Value v);
bool  is_truthy(Value v);
bool  values_equal(Value a, Value b);
char *value_to_string(Value v);
uint32_t hash_value(Value value);
void value_free_intern_table(void);

/* GC bookkeeping (frontend-independent). */
extern Object *all_objects;
extern Object *userdata_objects;
extern int allocated_objects;
extern size_t bytes_allocated;
extern size_t next_gc_threshold;
#define GC_COLOR_WHITE 0
#define GC_COLOR_GRAY  1
#define GC_COLOR_BLACK 2

enum { GC_STATE_IDLE, GC_STATE_MARK, GC_STATE_SWEEP };
extern int gc_state;
extern Object *sweep_cursor;

void free_object_container(Object *obj);

/*
 * TEMPORARY shim: the core VM still references the Luna object model
 * (ObjType / Obj* / IS_X) directly until the MOP vtable dispatch lands.
 * Remove this include once vm.c dispatches operations via the frontend's
 * vtable instead of switching on inline object kinds. (See docs/architecture.md.)
 */
#include "luna/object.h"

#endif /* LUNA_VALUE_H */
