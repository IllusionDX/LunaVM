/* value.h — Luna VM value and object types (NaN-boxed).
 *
 * Value layout (64-bit):
 *   Real doubles are stored as raw IEEE-754 bits.
 *   All other values are tagged with a quiet-NaN signature (0x7FFC in
 *   the top 16 bits) so they are never confused with valid doubles.
 *
 *   Pointer (Obj*):  QNAN | ptr          (ptr is 8-byte aligned => low 3 bits = 000)
 *   int32_t:         QNAN | TAG_INT | (i << 3)
 *   true:            QNAN | TAG_TRUE
 *   false:           QNAN | TAG_FALSE
 *   nil:             QNAN | TAG_NIL
 */

#ifndef LUNA_VALUE_H
#define LUNA_VALUE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Forward declarations */
typedef struct Object   Object;
struct Chunk;   /* defined in chunk.h — avoid circular include */
struct VM;      /* defined in vm.h */

/* ============================================================ */
/* NaN-boxed value                                               */
/* ============================================================ */

typedef uint64_t Value;

/* Quiet NaN base: sign=0, exponent=all-1s, quiet-bit=1
 * Top 16 bits = 0x7FF8 (bit 50 is now free for the 4-bit type tag). */
#define QNAN_TAG     ((uint64_t)0x7ff8000000000000)

/* Sub-tags in the lowest 3 payload bits.
 * Pointers from malloc are at least 8-byte aligned → low 3 bits = 000. */
#define TAG_NIL      1  /* 0x001 */
#define TAG_TRUE     2  /* 0x010 */
#define TAG_FALSE    3  /* 0x011 */
#define TAG_INT      4  /* 0x100 */
#define TAG_EMPTY    5  /* 0x101 (internal tombstone) */

#define IS_DOUBLE(v) (((v) & QNAN_TAG) != QNAN_TAG)
#define IS_OBJ(v)    (((v) & (QNAN_TAG | 7)) == QNAN_TAG)
#define IS_NIL(v)    ((v) == (QNAN_TAG | TAG_NIL))
#define IS_TRUE(v)   ((v) == (QNAN_TAG | TAG_TRUE))
#define IS_FALSE(v)  ((v) == (QNAN_TAG | TAG_FALSE))
#define IS_BOOL(v)   (IS_TRUE(v) || IS_FALSE(v))
#define IS_INT(v)    (((v) & (QNAN_TAG | 7)) == (QNAN_TAG | TAG_INT))
#define IS_NUMBER(v) (IS_DOUBLE(v) || IS_INT(v))

#define AS_OBJ(v)    ((Object*)(uintptr_t)((v) & 0x00007fffffffffffULL))
#define AS_BOOL(v)   IS_TRUE(v)
#define AS_INT(v)    ((int32_t)((v) >> 3))

static inline Value make_double(double d) {
    Value v;
    memcpy(&v, &d, sizeof(v));
    return v;
}
static inline double AS_DOUBLE(Value v) {
    double d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

static inline double as_double(Value v) {
    return IS_DOUBLE(v) ? AS_DOUBLE(v) : (double)AS_INT(v);
}
static inline int64_t as_int64(Value v) {
    return IS_INT(v) ? (int64_t)AS_INT(v) : (int64_t)AS_DOUBLE(v);
}

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
    OBJ_EXCEPTION,
    OBJ_UPVALUE,
    OBJ_CLOSURE,
    OBJ_ENUM
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
} Object;

/* ============================================================ */
/* Fast Object Type Checking & Creation                          */
/* ============================================================ */

/*
 * TYPE TAG ENCODING EVOLUTION:
 *
 *   3-bit (current) — bits 48, 49, 63  →  8 types.  ALL SLOTS ARE NOW OCCUPIED.
 *   4-bit (needed)  — bits 47-50        → 16 types.  Bit 47 must be reclaimed.
 *   5-bit (future)  — bits 47-50 + 63   → 32 types.  Bit 63 becomes free after
 *                     the move to contiguous 47-50 encoding.
 *
 * To expand to 4-bit:
 *   - Change QNAN_TAG from 0x7FFC... to 0x7FF8... (drop bit 50 from the signature).
 *   - Encode `t & 15` into contiguous bits 47-50 in make_obj() and TYPE_SIGNATURE().
 *   - AS_OBJ's mask 0x00007FFFFFFFFFFF already strips bit 47, so pointers stay safe.
 *   - Bit 63 is freed and can be added back later as a 5th type tag bit if needed.
 *
 * User-space pointers are restricted to 47 bits (128 TB), so bit 47 is always 0.
 */

static inline Value make_obj(void *ptr) {
    Object *obj = (Object*)ptr;
    uint64_t t = (uint64_t)obj->type;
    uint64_t type_tag = (t & 15) << 47;
    return QNAN_TAG | type_tag | (uint64_t)(uintptr_t)obj;
}
#define OBJ_VAL(obj) make_obj(obj)

#define OBJ_SIGNATURE_MASK 0xFFFF800000000007ULL
#define TYPE_SIGNATURE(t) (QNAN_TAG | (((uint64_t)(t) & 15) << 47))

#define IS_STRING(v)   (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_STRING))
#define IS_LIST(v)     (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_LIST))
#define IS_DICT(v)     (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_DICT))
#define IS_INSTANCE(v) (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_INSTANCE))
#define IS_FUNCTION(v) (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_FUNCTION))
#define IS_EXCEPTION(v) (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_EXCEPTION))
#define IS_CLOSURE(v)  (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_CLOSURE))
#define IS_ENUM(v)     (((v) & OBJ_SIGNATURE_MASK) == TYPE_SIGNATURE(OBJ_ENUM))

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

/* Class instance — fields as a parallel-array open dict */
typedef struct ObjInstance {
    Object              obj;
    char               *class_name;
    char               *base_class;   /* NULL if no parent */
    char              **field_names;
    Value              *fields;
    int                 field_count;
    int                 field_capacity;
    /* Method table: shallow array of borrowed ObjFunction pointers
     * from the class definition chunk; not owned by the instance. */
    struct ObjFunction **methods;
    int                  method_count;
    int                  method_capacity;
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

/* Exception */
typedef struct {
    Object  obj;
    char   *message;
    int     line;
    char   *file;
} ObjException;

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
ObjInstance *new_instance(const char *class_name, const char *base_class,
                          int initial_capacity);
ObjFunction *new_function(const char *name);
ObjFunction *new_native_function(const char *name, NativeFn fn);
ObjException *new_exception(const char *message);
ObjUpvalue  *new_upvalue(int stack_index);
ObjClosure  *new_closure(ObjFunction *function);
ObjEnum     *new_enum(const char *name, int count);

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
    if (--obj->refcount <= 0 && !gc_collecting) free_object(obj);
}

static inline void retain_value(Value v) {
    if (IS_OBJ(v)) retain_obj(AS_OBJ(v));
}

static inline void release_value(Value v) {
    if (IS_OBJ(v)) release_obj(AS_OBJ(v));
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
