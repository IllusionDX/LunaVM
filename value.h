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

/* Quiet NaN base: sign=0, exponent=all-1s, quiet-bit=1, bit50=1
 * Top 16 bits = 0x7FFC. */
#define QNAN_TAG     ((uint64_t)0x7ffc000000000000)

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

#define AS_OBJ(v)    ((Object*)(uintptr_t)((v) & 0x0000ffffffffffff))
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

#define OBJ_VAL(obj) (QNAN_TAG | (uint64_t)(uintptr_t)(obj))
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
#define make_obj(obj) OBJ_VAL(obj)

/* ============================================================ */
/* Object type tags                                              */
/* ============================================================ */

typedef enum {
    OBJ_STRING,    /* tag 7  */
    OBJ_LIST,      /* tag 8  */
    OBJ_DICT,      /* tag 9  */
    OBJ_INSTANCE,  /* tag 10 */
    OBJ_FUNCTION,  /* tag 11 / 12 (is_native flag) */
    OBJ_EXCEPTION,
    OBJ_UPVALUE,
    OBJ_CLOSURE
} ObjType;

/* ============================================================ */
/* GC object header — embedded first in every heap object        */
/* ============================================================ */

typedef struct Object {
    ObjType        type;
    int            refcount;
    struct Object *next;    /* intrusive GC linked list */
} Object;

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

void free_object(Object *obj);

static inline void retain_obj(Object *obj) {
    if (obj) obj->refcount++;
}

static inline void release_obj(Object *obj) {
    if (!obj) return;
    if (--obj->refcount <= 0) free_object(obj);
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
