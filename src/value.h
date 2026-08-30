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
typedef struct Type     Type;   /* MOP vtable — defined below (language-agnostic) */
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

/* ============================================================
 * Heap object (core-owned, language-agnostic).
 *
 * Every heap value is just an `Object *` in the NaN space. The concrete
 * meaning of an object (list, dict, instance, function, …) lives entirely in
 * the frontend's `Type` (the MOP vtable) — the core only knows this uniform
 * header. See docs/architecture.md §2–§3.
 * ============================================================ */
typedef struct Object {
    Type          *type;   /* MOP vtable pointer (frontend-owned) */
    uint8_t        gc_color;
    size_t         size;
    struct Object *next;
    struct Object *prev;
    struct Object *finalizer_next;
    struct Object *finalizer_prev;
} Object;

/* ============================================================
 * MOP: language-agnostic operation dispatch via vtable.
 *
 * The frontend (e.g. Luna) defines one `Type` instance per heap-object kind
 * and fills its vtable. The VM core reaches operations through `Object.type`
 * and never names a concrete frontend type. Immediate int/double are handled
 * natively by the core.
 * ============================================================ */
typedef Value (*MOP_Bin)(struct VM *vm, Value a, Value b);
typedef int   (*MOP_Cmp)(struct VM *vm, Value a, Value b);          /* -1 / 0 / +1 */
typedef Value (*MOP_Un)(struct VM *vm, Value self);
typedef Value (*MOP_Idx)(struct VM *vm, Value self, Value key);
typedef void  (*MOP_IdxSet)(struct VM *vm, Value self, Value key, Value val);
typedef Value (*MOP_Attr)(struct VM *vm, Value self, const char *name);
typedef int   (*MOP_AttrSet)(struct VM *vm, Value self, const char *name, Value val);
typedef Value (*MOP_Call)(struct VM *vm, Value self, Value *args, int argc);
typedef uint32_t (*MOP_Hash)(Value self);
typedef int   (*MOP_Len)(struct VM *vm, Value self);

/* callable / closure protocol — how the core invokes & inspects a callable
 * object through the vtable instead of switching on concrete frontend kinds. */
typedef struct Chunk* (*MOP_Chunk)(Value self);     /* bytecode chunk; NULL for native callables */
typedef Value    (*MOP_Self)(Value self);           /* self to bind before a bytecode frame (NIL if none) */
typedef const char* (*MOP_Name)(Value self);        /* object display name (stack traces / module naming) */
typedef Value    (*MOP_Upval)(struct VM *vm, Value closure, int i); /* read upvalue i (NIL if absent) */
typedef void     (*MOP_UpvalSet)(struct VM *vm, Value closure, int i, Value v); /* write upvalue i */
typedef Value    (*MOP_UpvalRef)(Value closure, int i); /* shared upvalue object (closure capture), NIL if absent */
typedef int      (*MOP_ParamCount)(Value self);          /* number of declared parameters */
typedef Value    (*MOP_ParamName)(Value self, int i);     /* parameter name i as a string Value (NIL if absent) */
/* Remap keyword arguments into the callee's parameter registers and apply
 * defaults.  `fn_val` is the callable, `fn_reg` its register, `nargs` the total
 * argument count (positional + keyword), and `kw_names` a static tuple of the
 * keyword names (a List of name strings).  Returns false on a binding error
 * (the exception is already set in vm->last_exception).  Frontend-owned: the
 * core OP_CALL just invokes it after reading the OP_KW_PREFIX state. */
typedef bool     (*MOP_BindKw)(struct VM *vm, Value fn_val, uint8_t nargs, Value kw_names);
typedef void     (*MOP_Free)(Object *obj);                /* free container's own memory (GC frees the Object) */
typedef void     (*MOP_Mark)(struct VM *vm, Object *obj); /* mark referenced objects for GC */
typedef bool     (*MOP_Eq)(Value a, Value b);             /* equality of this type with another Value */
typedef char*    (*MOP_CStr)(Value self);                 /* string form (caller frees); NULL if unprintable */
typedef const char* (*MOP_Msg)(struct VM *vm, Value self); /* raw exception message (NULL if none) */
typedef const char* (*MOP_ClassName)(Value self);         /* display class name (NULL if none) */
typedef const char* (*MOP_Chars)(Value self);             /* internal chars if a string object, else NULL */

typedef struct Type {
    const char *name;   /* human-readable type name (e.g. "list") */
    int         kind;   /* frontend discriminator (core never switches on it) */
    /* arithmetic / comparison */
    MOP_Bin  add, sub, mul, div, mod;
    MOP_Un   neg;
    MOP_Cmp  cmp;
    /* indexing */
    MOP_Idx     getitem;
    MOP_IdxSet  setitem;
    /* attributes */
    MOP_Attr    getattr;
    MOP_AttrSet setattr;
    /* call */
    MOP_Call call;
    /* misc */
    MOP_Un   tostring;  /* returns a string Value */
    MOP_Hash hash;
    MOP_Len  len;
    /* callable / closure protocol */
    MOP_Chunk    get_chunk;
    MOP_Self     get_self;
    MOP_Name     name_of;
    MOP_Upval    get_upvalue;
    MOP_UpvalSet set_upvalue;
    MOP_UpvalRef get_upvalue_ref;
    MOP_ParamCount param_count;
    MOP_ParamName  get_param_name;
    MOP_BindKw     bind_keyword_arguments;
    /* lifecycle / formatting */
    MOP_Free      free;
    MOP_Mark      mark;
    MOP_Eq        eq;
    MOP_CStr      to_cstr;
    MOP_Msg       message;
    MOP_ClassName class_name;
    MOP_Chars     string_chars;
} Type;

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
/* Core universal numeric types only: the 32-bit tagged int and the double.
 * int64 (and any other big-number type) is a frontend object handled by the
 * numeric MOPs / binary-unary-compare hooks, never by core macros. */
#define IS_NUMBER(v) (IS_DOUBLE(v) || IS_INT(v))

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

/* The core VM is language-agnostic: it depends only on Value / Object / Type /
 * Chunk / VM / CallFrame and the MOP vtable.  Luna-specific object kinds live in
 * the frontend (src/luna/), which includes luna/object.h directly.  Operations
 * reach Luna semantics through the MOP vtable and the VMFrontendHooks table. */

#endif /* LUNA_VALUE_H */
