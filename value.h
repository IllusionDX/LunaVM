/* value.h — Luna VM value and object types (NaN-boxed).
 *
 * Value layout (64-bit):
 *   Real doubles are stored as raw IEEE-754 bits.
 *   All other values are tagged with a quiet-NaN signature (0x7FF8 in
 *   the top 16 bits) so they are never confused with valid doubles.
 *
 *   Pointer (Obj*):  QNAN | ptr          (ptr is 8-byte aligned => low 3 bits = 000)
 *   int32_t:         QNAN | TAG_INT | (i << 3)
 *   int64_t:         heap ObjInt64 (OBJ_INT64)
 *   true:            QNAN | TAG_TRUE
 *   false:           QNAN | TAG_FALSE
 *   nil:             QNAN | TAG_NIL
 */

#ifndef LUNA_VALUE_H
#define LUNA_VALUE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
struct Chunk;   /* defined in chunk.h — avoid circular include */
struct VM;      /* defined in vm.h */

/* ============================================================ */
/* NaN-boxed value                                               */
/* ============================================================ */

typedef uint64_t Value;

/* Quiet NaN base: sign=0, exponent=all-1s, quiet-bit=1
 * Top 16 bits = 0x7FF8. Bit 50 is 0 to avoid overlap with type tags in bits 47-50.
 * Type 0 signature: 0x7FF8000000000000 (payload=0 reserved for OBJ_STRING).
 * NaN values are normalized to payload>=1 via make_double() to avoid collision. */
#define QNAN_TAG     ((uint64_t)0x7ff8000000000000)

/* Sub-tags in the lowest 3 payload bits.
 * Pointers from malloc are at least 8-byte aligned → low 3 bits = 000. */
#define TAG_NIL      1  /* 0x001 */
#define TAG_TRUE     2  /* 0x010 */
#define TAG_FALSE    3  /* 0x011 */
#define TAG_INT      4  /* 0x100 */
#define TAG_EMPTY    5  /* 0x101 (internal tombstone) */

#define IS_DOUBLE(v) LUNA_LIKELY( \
    (((v) & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL) || \
    ((v) == 0x7FF800000000000AULL) || \
    (((v) & 0x000FFFFFFFFFFFFFULL) == 0) \
)
/*
 * IS_OBJ must stay signature-based, not just "!IS_DOUBLE".
 *
 * Reason:
 * - NIL / TRUE / FALSE / INT / EMPTY are immediate sub-tags that live inside the
 *   NaN-box payload and are not heap objects.
 * - +Inf and -Inf are valid IEEE doubles and must remain doubles, not objects.
 * - Using "!IS_DOUBLE" would incorrectly classify these immediates and special
 *   numeric values as heap pointers, which breaks the VM and the GC.
 */
#define IS_OBJ(v)    (((v) & (QNAN_TAG | 7)) == QNAN_TAG)
#define IS_NIL(v)    ((v) == (QNAN_TAG | TAG_NIL))
#define IS_TRUE(v)   ((v) == (QNAN_TAG | TAG_TRUE))
#define IS_FALSE(v)  ((v) == (QNAN_TAG | TAG_FALSE))
#define IS_BOOL(v)   (IS_TRUE(v) || IS_FALSE(v))
#define IS_INT(v)    (((v) & (QNAN_TAG | 7)) == (QNAN_TAG | TAG_INT))
#define IS_INT64(v)  (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(13))
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

#define OBJ_SIGNATURE_MASK 0xFFFF800000000007ULL
#define TYPE_SIGNATURE(t) (QNAN_TAG | (((uint64_t)(t) & 15) << 47) | ((((uint64_t)(t) >> 4) & 1ULL) << 63))

static inline Value make_double(double d) {
    Value v;
    memcpy(&v, &d, sizeof(v));
    /* Strip sign bit for analysis — hardware can produce negative NaNs
     * (0xFFF8...) which must be treated identically to positive NaNs. */
    uint64_t abs_v = v & 0x7FFFFFFFFFFFFFFFULL;
    /* Exponent all 1s means Inf or NaN */
    if (abs_v >= 0x7FF0000000000000ULL) {
        /* If it's not exactly +Inf, it's a NaN (any payload, any sign) */
        if (abs_v != 0x7FF0000000000000ULL) {
            /* Normalize ALL NaNs to safe positive quiet NaN, payload=10 (0x0A).
               Payload=10 is well outside the sub-tag range (1-5), ensuring
               no collision with NIL, TRUE, FALSE, INT, EMPTY, or future sub-tags. */
            return 0x7FF800000000000AULL;
        }
        /* It's +Inf or -Inf — pass through unchanged */
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

/* FFI Boundary Sanitization: ALL external C doubles MUST pass through make_double().
 * Hardware NaNs have random payloads that could collide with object type signatures.
 * make_double() normalizes them to payload=10 (safe for doubles, never collides).
 * Usage: return make_double(external_c_function(...));
 * NEVER: return raw_double_value;  // UNSAFE — can crash the VM */
#define RETURN_EXT_DOUBLE(d) make_double((double)(d))

/* OBJ_VAL moved below Object struct definition */
#define INT_VAL(i)   (QNAN_TAG | TAG_INT | ((uint64_t)(uint32_t)(i) << 3))
#define NIL_VAL      (QNAN_TAG | TAG_NIL)
#define TRUE_VAL     (QNAN_TAG | TAG_TRUE)
#define FALSE_VAL    (QNAN_TAG | TAG_FALSE)
#define BOOL_VAL(b)  ((b) ? TRUE_VAL : FALSE_VAL)
#define EMPTY_VAL    (QNAN_TAG | TAG_EMPTY)

/* Compatibility constructors (same names as before) */
#define make_null()   NIL_VAL
#define make_bool(b)  BOOL_VAL(b)
#define make_int(i)   INT_VAL((int32_t)(i))
/* make_obj moved below Object struct definition */

/* ============================================================ */
/* Object type tags                                              */
/* ============================================================ */

typedef enum {
    OBJ_STRING,
    OBJ_LIST,
    OBJ_DICT,
    OBJ_INSTANCE,
    OBJ_FUNCTION,  /* native flag lives in is_native field, not a separate type */
    OBJ_UPVALUE,
    OBJ_CLOSURE,
    OBJ_ENUM,
    OBJ_CLASS,
    OBJ_BOUND_METHOD,
    OBJ_MODULE,
    OBJ_BUFFER = 12,
    OBJ_INT64 = 13,
    OBJ_USERDATA,
    OBJ_VECTOR = 15
} ObjType;

/* ============================================================ */
/* GC object header — embedded first in every heap object        */
/* ============================================================ */

typedef struct Object {
    ObjType        type;
    int            refcount;
    bool           is_marked;
    size_t         size;
    struct Object *next;    /* intrusive GC linked list */
    struct Object *prev;    /* doubly linked for O(1) removal */
    struct Object *finalizer_next; /* intrusive userdata finalizer list */
    struct Object *finalizer_prev;
} Object;

/* ============================================================ */
/* Fast Object Type Checking & Creation                          */
/* ============================================================ */

/*
 * TYPE TAG ENCODING EVOLUTION:
 *
 *   3-bit (legacy) — bits 48, 49, 63  →  8 types.  Was used before 4-bit expansion.
 *   4-bit (current) — bits 47-50       → 16 types.  Bit 47 reclaimed from QNAN_TAG
 *                     by changing it from 0x7FFC to 0x7FF8 (dropping bit 50 from
 *                     the signature).  `t & 15` encoded in contiguous bits 47-50.
 *   5-bit (enabled) — bits 47-50 + 63  → 32 types.  Bit 63 is reserved as the high
 *                     tag bit for type ids 16-31.  NaN normalization keeps bit 63=0
 *                     for all non-object doubles.
 *
 * bit 63 must be 0 for all tagged values and normalized NaNs for 5-bit expansion.
 */

static inline Value make_obj(void *ptr) {
    Object *obj = (Object*)ptr;
    uint64_t t = (uint64_t)obj->type;
    uint64_t type_tag = ((t & 15) << 47) | ((t & 16) ? (1ULL << 63) : 0ULL);
    return QNAN_TAG | type_tag | (uint64_t)(uintptr_t)obj;
}
#define OBJ_VAL(obj) make_obj(obj)

#define IS_STRING(v)   (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_STRING))
#define IS_LIST(v)     (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_LIST))
#define IS_DICT(v)     (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_DICT))
#define IS_INSTANCE(v) (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_INSTANCE))
#define IS_FUNCTION(v) (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_FUNCTION))
#define IS_CLOSURE(v)  (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_CLOSURE))
#define IS_ENUM(v)     (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_ENUM))
#define IS_CLASS(v)    (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_CLASS))
#define IS_BOUND_METHOD(v) (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_BOUND_METHOD))
#define IS_MODULE(v)       (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_MODULE))
#define IS_BUFFER(v)       (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_BUFFER))
#define IS_USERDATA(v)     (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_USERDATA))
#define IS_VECTOR(v)       (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_VECTOR))

/* ============================================================ */
/* Native function signature                                     */
/* ============================================================ */

typedef Value (*NativeFn)(struct VM *vm, Value *args, int arg_count);

/* ============================================================ */
/* Heap object structs                                           */
/* ============================================================ */

/* String — immutable, interned by the chunk's constant pool */
typedef struct {
    Object   obj;
    char    *chars;
    int      length;
    uint32_t hash;
} ObjString;

/* List — dynamic array (no embedded type info — dynamic typing)
 * Small Object Optimization: up to 4 elements stored inline
 * to avoid a separate heap allocation. */
typedef struct {
    Object  obj;
    Value  *items;           /* NULL = using inline storage */
    int     count;
    int     capacity;
    Value   inline_items[4]; /* inline buffer for SOO */
} ObjList;

/* Dict entry in the dense array */
typedef struct {
    uint32_t hash;
    Value    key;
    Value    value;
} ObjDictEntry;

/* Dict — Compact Ordered Dict maintaining insertion order
 * Small Object Optimization: up to 4 entries stored inline
 * with linear search to avoid sparse+dense allocations. */
typedef struct {
    Object     obj;
    int       *indices;          /* NULL = using SOO (linear search) */
    ObjDictEntry *entries;       /* NULL = using SOO */
    int        capacity;         /* Size of indices array (power of 2) */
    int        entry_count;      /* Number of valid entries */
    int        next_entry;       /* Next available index in entries array */
    int        deleted_count;    /* Number of tombstones */
    ObjDictEntry inline_entries[4]; /* inline buffer for SOO */
} ObjDict;

/* Class object — first-class runtime representation of a class */
typedef struct ObjClass {
    Object              obj;
    char               *name;
    struct ObjClass    *base;          /* parent class, NULL if none */
    struct ObjInstance *prototype;     /* template with default field values */
    struct ObjFunction **methods;
    char               **method_names;
    int                  method_count;
    int                  method_capacity;
} ObjClass;

/* Bound method — instance + method function, callable like a closure */
typedef struct {
    Object              obj;
    Value               self;           /* the bound instance */
    struct ObjFunction *fn;             /* the method function */
} ObjBoundMethod;

/* Module — namespace holding exported values */
typedef struct ObjModule {
    Object    obj;
    ObjString *name;      /* module name (e.g. "math") */
    ObjDict   *exports;   /* dict of name -> value */
} ObjModule;

/* Buffer â€” raw byte storage for binary I/O and network APIs */
typedef struct ObjBuffer {
    Object  obj;
    uint8_t *data;
    size_t  size;
    size_t  capacity;
    size_t  cursor;
} ObjBuffer;

/* Int64 â€” heap boxed 64-bit integer */
typedef struct ObjInt64 {
    Object   obj;
    uint64_t value;
} ObjInt64;

static inline double as_double(Value v) {
    if (IS_DOUBLE(v)) return AS_DOUBLE(v);
    if (IS_INT64(v)) return (double)((int64_t)((ObjInt64*)AS_OBJ(v))->value);
    return (double)AS_INT(v);
}
static inline int64_t as_int64(Value v) {
    if (IS_INT(v)) return (int64_t)AS_INT(v);
    if (IS_INT64(v)) return (int64_t)((ObjInt64*)AS_OBJ(v))->value;
    return (int64_t)AS_DOUBLE(v);
}

typedef void (*UserdataFinalizer)(void *data);

typedef struct ObjUserdata {
    Object           obj;
    char            *tag;
    void            *data;
    UserdataFinalizer finalizer;
    bool             finalized;
} ObjUserdata;

typedef struct ObjVector {
    Object obj;
    float  data[4];   /* x, y, z, w */
} ObjVector;

/* Class instance — fields as a parallel-array open dict */
typedef struct ObjInstance {
    Object              obj;
    char               *class_name;
    char              **field_names;
    Value              *fields;
    int                 field_count;
    int                 field_capacity;
    struct ObjClass    *klass;         /* owning class (retained) */
} ObjInstance;

/* Upvalue descriptor — stored in ObjFunction for CLOSURE instruction */
typedef struct {
    uint8_t index;
    bool    is_local;
} UpvalueDesc;

/* Function — Luna bytecode function or native C function */
typedef struct ObjFunction {
    Object obj;
    char *name;
    /* Bytecode (NULL for natives) */
    struct Chunk *chunk;
    /* Parameter names — for debug and future reflection */
    char **param_names;
    /* Pre-interned ObjString* for each parameter name — avoids redundant
       string interning on every keyword-args lookup (Issue #1). */
    ObjString **param_name_objs;
    int param_count;
    /* Native flag */
    bool is_native;
    NativeFn native_fn;
    /* Closure metadata */
    int upvalue_count;
    UpvalueDesc *upvalue_descriptors;
    /* Legacy tree-walking fields (used by eval_*.c until Phase 4) */
    struct FunctionParam *params;
    struct Stmt **body;
    int body_count;
    struct Environment *closure;
} ObjFunction;

/* Upvalue — pointer to a captured variable (either on stack or closed) */
typedef struct ObjUpvalue {
    Object       obj;
    int          stack_index;   /* offset into vm->stack (if open) */
    bool         is_open;
    Value        closed;        /* heap storage when closed */
    struct ObjUpvalue *next;    /* linked list of open upvalues */
    int          frame_depth;   /* vm->frame_count when captured */
} ObjUpvalue;

/* Closure — function + captured upvalues */
typedef struct {
    Object        obj;
    struct ObjFunction *function;
    ObjUpvalue  **upvalues;
    int           upvalue_count;
} ObjClosure;

/* Enum */
typedef struct {
    Object   obj;
    char    *name;
    char   **names;      /* variant names */
    int64_t *values;     /* variant values */
    int      count;
} ObjEnum;

/* ============================================================ */
/* Object constructors                                           */
/* ============================================================ */

ObjString   *new_string(const char *chars, int length);
ObjList     *new_list(int capacity);
ObjDict     *new_dict(void);
ObjInstance *new_instance(struct ObjClass *klass, int field_capacity);
ObjClass    *new_class(const char *name, const char *base_name);
ObjFunction *new_function(const char *name);
ObjFunction *new_native_function(const char *name, NativeFn fn);
ObjUserdata *new_userdata_tagged(const char *tag, void *data, UserdataFinalizer finalizer);
#define new_userdata(data, finalizer) new_userdata_tagged("userdata", (data), (finalizer))
Value make_exception_instance(struct VM *vm, struct ObjClass *cls, const char *message);
ObjUpvalue  *new_upvalue(int stack_index);
ObjClosure  *new_closure(ObjFunction *function);
ObjEnum     *new_enum(const char *name, int count);
ObjBoundMethod *new_bound_method(Value self, struct ObjFunction *fn);
ObjModule     *new_module(const char *name);
ObjInt64      *new_int64(int64_t value);
ObjVector     *new_vector(float x, float y, float z, float w);
static inline Value make_int64(int64_t value) { return make_obj((Object*)new_int64(value)); }
Value          buffer_read_byte(const ObjBuffer *buf, size_t offset);
Value          buffer_read_short(const ObjBuffer *buf, size_t offset);
Value          buffer_read_int(const ObjBuffer *buf, size_t offset);
Value          buffer_read_long(const ObjBuffer *buf, size_t offset);
ObjBuffer     *new_buffer(size_t capacity);
void           buffer_reserve(ObjBuffer *buf, size_t capacity);
void           buffer_resize(ObjBuffer *buf, size_t size);
void           buffer_append_byte(ObjBuffer *buf, uint8_t byte);
void           buffer_append_data(ObjBuffer *buf, const uint8_t *data, size_t len);

/* ============================================================ */
/* Value predicates / utilities                                  */
/* ============================================================ */

bool  is_null(Value v);
bool  is_truthy(Value v);
bool  values_equal(Value a, Value b);
char *value_to_string(Value v);     /* caller must free() */

/* ============================================================ */
/* ARC memory management                                         */
/* ============================================================ */

extern Object *all_objects;
extern Object *userdata_objects;
extern int allocated_objects;
extern size_t bytes_allocated;
extern size_t next_gc_threshold;
extern bool gc_collecting;

void free_object(Object *obj);
void free_object_container(Object *obj);

static inline void retain_obj(Object *obj) {
    if (obj) obj->refcount++;
}

static inline void release_obj(Object *obj) {
    if (!obj) return;
    if (obj->refcount <= 0) {
#ifndef NDEBUG
        fprintf(stderr, "BUG: invalid release! obj=%p type=%d refcount=%d\n", (void*)obj, obj->type, obj->refcount);
        abort();
#else
        return;
#endif
    }
    if (--obj->refcount <= 0 && !gc_collecting) {
        free_object(obj);
    }
}

static inline void retain_value(Value v) {
    if (IS_OBJ(v)) retain_obj(AS_OBJ(v));
}

static inline void release_value(Value v) {
    if (IS_OBJ(v)) release_obj(AS_OBJ(v));
}

static inline double value_to_double(Value v) {
    if (IS_INT(v)) return (double)AS_INT(v);
    if (IS_INT64(v)) return (double)((int64_t)((ObjInt64*)AS_OBJ(v))->value);
    return AS_DOUBLE(v);
}

void value_free_intern_table(void);

/* ============================================================ */
/* List operations                                               */
/* ============================================================ */

void  list_add(ObjList *list, Value value);
void  list_insert(ObjList *list, int index, Value value);
Value list_remove(ObjList *list, int index);
Value list_pop(ObjList *list);
void  list_clear(ObjList *list);
Value list_get(ObjList *list, int index);
void  list_set(ObjList *list, int index, Value value);
int   list_length(ObjList *list);
bool  list_contains(ObjList *list, Value value);

/* ============================================================ */
/* Dict operations                                               */
/* ============================================================ */

void  dict_set(ObjDict *dict, Value key, Value value);
Value dict_get(ObjDict *dict, Value key);
bool  dict_has(ObjDict *dict, Value key);
Value dict_remove(ObjDict *dict, Value key);
void  dict_clear(ObjDict *dict);
int   dict_length(ObjDict *dict);
Value dict_keys(ObjDict *dict);     /* returns Value wrapping ObjList */
Value dict_values(ObjDict *dict);   /* returns Value wrapping ObjList */

/* ============================================================ */
/* Instance field operations                                     */
/* ============================================================ */

Value instance_get_field(ObjInstance *inst, const char *name);
void  instance_set_field(ObjInstance *inst, const char *name, Value value);

/* ============================================================ */
/* Hashing                                                       */
/* ============================================================ */

uint32_t hash_value(Value value);

/* ============================================================ */
/* Error helpers                                                 */
/* ============================================================ */

void runtime_error(const char *fmt, ...);

#endif /* LUNA_VALUE_H */

