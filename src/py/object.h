#ifndef LUNA_OBJECT_H
#define LUNA_OBJECT_H

#include "value.h"
#include "bigint.h"

struct py_State; /* forward decl: the py frontend's API state (see py.h) */
struct APIState; /* embeddable C API state (see api.h) */

/* ============================================================
 * Python Implementation Object Model (frontend-specific).
 *
 * The VM core is language-agnostic; this file owns the Python
 * subset's types, their kind predicates, and the operations the
 * VM currently dispatches to.
 * When the MOP vtable lands (docs/architecture.md), the per-kind
 * switches move into vtable implementations and the core stops
 * switching on ObjType.
 * ============================================================ */

typedef enum {
    OBJ_STRING,
    OBJ_LIST,
    OBJ_TUPLE,
    OBJ_DICT,
    OBJ_INSTANCE,
    OBJ_FUNCTION,  /* native flag lives in is_native field, not a separate type */
    OBJ_UPVALUE,
    OBJ_CLOSURE,
    OBJ_CLASS,
    OBJ_BOUND_METHOD,
    OBJ_MODULE,
    OBJ_BUFFER = 12,
    OBJ_BIGINT = 13,  /* arbitrary-precision int (PyLong-style, base 2^30) */
    OBJ_RANGE = 14,   /* lazy range (start/stop/step only, O(1)) */
    OBJ_RANGEITER = 15, /* lazy range iterator */
    OBJ_SLICE = 16,   /* slice(start, stop, step) object used as a getitem key */
} ObjType;

/* Object and Type are defined in the core value model (src/value.h).
 * This file owns only Luna's concrete kinds, their predicates, and the
 * operations the VM dispatches to via the MOP vtable. */

/* Heap object kind checks: compare the object's Type* against the table entry
 * for kind `t`. No inline type tag; the type lives in the heap object. */
#define IS_OBJ_KIND(v, t) (IS_OBJ(v) && AS_OBJ(v)->type == py_types[(t)])
#define IS_STRING(v)   IS_OBJ_KIND(v, OBJ_STRING)
#define IS_LIST(v)     IS_OBJ_KIND(v, OBJ_LIST)
#define IS_TUPLE(v)    IS_OBJ_KIND(v, OBJ_TUPLE)
#define IS_DICT(v)     IS_OBJ_KIND(v, OBJ_DICT)
#define IS_INSTANCE(v) IS_OBJ_KIND(v, OBJ_INSTANCE)
#define IS_FUNCTION(v) IS_OBJ_KIND(v, OBJ_FUNCTION)
#define IS_CLOSURE(v)  IS_OBJ_KIND(v, OBJ_CLOSURE)
#define IS_CLASS(v)    IS_OBJ_KIND(v, OBJ_CLASS)
#define IS_BOUND_METHOD(v) IS_OBJ_KIND(v, OBJ_BOUND_METHOD)
#define IS_MODULE(v)       IS_OBJ_KIND(v, OBJ_MODULE)
#define IS_BUFFER(v)       IS_OBJ_KIND(v, OBJ_BUFFER)
#define IS_BIGINT(v)   IS_OBJ_KIND(v, OBJ_BIGINT)
#define IS_RANGE(v)    IS_OBJ_KIND(v, OBJ_RANGE)
#define IS_RANGEITER(v) IS_OBJ_KIND(v, OBJ_RANGEITER)
#define IS_SLICE(v)    IS_OBJ_KIND(v, OBJ_SLICE)

/* MOP typedefs and the `Type` vtable struct are defined in the core value
 * model (src/value.h). Luna fills the vtable instances in luna/object.c. */

/* Indexed by ObjType so the core can map a kind to its Type* (used by the
 * IS_X predicates and by constructors). Defined in luna/object.c. */
extern Type *py_types[];

/* One-time wiring of the lifecycle/formatting vtable methods. Called
 * from the frontend's state init (py_new_state) after vm_init. */
void py_wire_lifecycle(void);

struct VM;
struct ObjClass;
struct ObjClass *get_class(struct VM *vm, Value val);

typedef Value (*NativeFn)(struct VM *vm, Value *args, int arg_count);
typedef Value (*NativeKwFn)(struct VM *vm, Value *args, int arg_count, Value kw_names);

/* Heap object structs */
typedef struct {
    Object   obj;
    char    *chars;
    int      length;
    uint32_t hash;
} ObjString;

/* List — dynamic array (Small Object Optimization: up to 4 inline elements). */
typedef struct {
    Object  obj;
    Value  *items;
    int     count;
    int     capacity;
    Value   inline_items[4];
} ObjList;

/* Tuple — immutable, fixed-size, contiguous array (Single Allocation). */
typedef struct {
    Object  obj;
    int     count;
    uint32_t hash;
    Value   items[];
} ObjTuple;

typedef struct {
    uint32_t hash;
    Value    key;
    Value    value;
} ObjDictEntry;

/* Dict — open-addressing, insertion-ordered (up to 4 inline entries). */
typedef struct {
    Object     obj;
    ObjDictEntry *entries;
    int       *order;
    int        capacity;
    int        entry_count;
    int        order_count;
    int        order_capacity;
    int        tombstone_count;
    ObjDictEntry inline_entries[4];
} ObjDict;

typedef struct ObjClass {
    Object              obj;
    char               *name;
    struct ObjClass    *base;
    struct ObjInstance *prototype;
    struct ObjFunction **methods;
    char               **method_names;
    int                  method_count;
    int                  method_capacity;
    ObjDict            *fields;
    ObjDict            *field_slot_map;
    int                 field_count;
} ObjClass;

typedef struct {
    Object              obj;
    Value               self;
    struct ObjFunction *fn;
} ObjBoundMethod;

typedef struct ObjModule {
    Object    obj;
    ObjString *name;
    ObjDict   *exports;
} ObjModule;

typedef struct ObjBuffer {
    Object  obj;
    uint8_t *data;
    size_t  size;
    size_t  capacity;
    size_t  cursor;
} ObjBuffer;

/* Lazy range — stores only (start, stop, step); no materialized elements.
 * start/stop/step are int32 immediates or bigint Values. O(1) memory. */
typedef struct {
    Object obj;
    Value  start;
    Value  stop;
    Value  step;
} ObjRange;

/* Lazy range iterator — one cursor `current` advancing by `step`. */
typedef struct {
    Object obj;
    Value  current;
    Value  stop;
    Value  step;
} ObjRangeIter;

/* Slice object — used as a key in OP_INDEXGET/__getitem__ for a[start:stop:step].
 * start/stop/step are int32/bigint Values, or nil (make_null) when omitted. */
typedef struct {
    Object obj;
    Value  start;
    Value  stop;
    Value  step;
} ObjSlice;

typedef struct ObjInstance {
    Object              obj;
    char               *class_name;
    char              **field_names;
    Value              *fields;
    int                 field_count;
    int                 field_capacity;
    struct ObjClass    *klass;
} ObjInstance;

typedef struct {
    uint8_t index;
    bool    is_local;
} UpvalueDesc;

typedef struct ObjFunction {
    Object obj;
    char *name;
    struct Chunk *chunk;
    char **param_names;
    ObjString **param_name_objs;
    int param_count;
    bool is_native;
    bool is_leaf;
    NativeFn native_fn;
    NativeKwFn native_kw;   /* kw-aware native; non-null enables keyword args */
    int (*cfunc)(struct APIState *L);
    int upvalue_count;
    UpvalueDesc *upvalue_descriptors;
    struct FunctionParam *params;
    struct Stmt **body;
    int body_count;
    struct Environment *closure;
    /* Default-value thunks (one per parameter, NIL where no default).  Applied
     * by bind_keyword_arguments when a parameter is not supplied positionally
     * or by keyword.  default_count == param_count. */
    Value *defaults;
    int default_count;
} ObjFunction;

typedef struct ObjUpvalue {
    Object       obj;
    int          stack_index;
    bool         is_open;
    Value        closed;
    struct ObjUpvalue *next;
    int          frame_depth;
} ObjUpvalue;

typedef struct {
    Object        obj;
    struct ObjFunction *function;
    ObjUpvalue  **upvalues;
    int           upvalue_count;
} ObjClosure;

/* Object constructors / operations (defined in value.c, temporarily). */
ObjString   *new_string(const char *chars, int length);
ObjList     *new_list(int capacity);
ObjTuple    *new_tuple(int count);
ObjDict     *new_dict(void);
ObjInstance *new_instance(struct ObjClass *klass, int field_capacity);
ObjClass    *new_class(const char *name, const char *base_name);
ObjFunction *new_function(const char *name);
ObjFunction *new_native_function(const char *name, NativeFn fn);
ObjFunction *new_native_kw_function(const char *name, NativeKwFn fn);
Value make_exception_instance(struct VM *vm, void *cls, const char *message);

ObjUpvalue  *new_upvalue(int stack_index);
ObjClosure  *new_closure(ObjFunction *function);
ObjBoundMethod *new_bound_method(Value self, struct ObjFunction *fn);
ObjModule     *new_module(const char *name);
Value          buffer_read_byte(const ObjBuffer *buf, size_t offset);
Value          buffer_read_short(const ObjBuffer *buf, size_t offset);
Value          buffer_read_int(const ObjBuffer *buf, size_t offset);
Value          buffer_read_long(const ObjBuffer *buf, size_t offset);
ObjBuffer     *new_buffer(size_t capacity);
void           buffer_reserve(ObjBuffer *buf, size_t capacity);
void           buffer_resize(ObjBuffer *buf, size_t size);
void           buffer_append_byte(ObjBuffer *buf, uint8_t byte);
void           buffer_append_data(ObjBuffer *buf, const uint8_t *data, size_t len);

static inline double as_double(Value v) {
    if (IS_DOUBLE(v)) return AS_DOUBLE(v);
    if (IS_BIGINT(v)) return bigint_to_f64((ObjBigInt*)AS_OBJ(v));
    return (double)AS_INT(v);
}

/* Exact int64 view of an integer value (int32 immediate or bigint).
 * Returns false when the bigint does not fit; doubles are excluded so callers
 * that accept floats keep their own truncation policy. */
static inline bool int64_exact(Value v, int64_t *out) {
    if (IS_INT(v)) { *out = AS_INT(v); return true; }
    if (IS_BIGINT(v)) return bigint_get_i64((ObjBigInt*)AS_OBJ(v), out);
    return false;
}

/* Truncating int64 view (doubles truncate toward zero). Only safe for the
 * call sites audited to hold small values; use int64_exact for input paths. */
static inline int64_t as_int64(Value v) {
    if (IS_INT(v)) return (int64_t)AS_INT(v);
    if (IS_BIGINT(v)) {
        int64_t out = 0;
        bigint_get_i64((ObjBigInt*)AS_OBJ(v), &out);
        return out;
    }
    return (int64_t)AS_DOUBLE(v);
}
static inline double value_to_double(Value v) {
    if (IS_INT(v)) return (double)AS_INT(v);
    if (IS_BIGINT(v)) return bigint_to_f64((ObjBigInt*)AS_OBJ(v));
    return AS_DOUBLE(v);
}

/* GC entry point used by frontend object constructors (see object.c). */
void py_init_object(Object *obj, ObjType type, size_t size);

/* List operations */
void  list_add(ObjList *list, Value value);
void  list_insert(ObjList *list, int index, Value value);
Value list_remove(ObjList *list, int index);
Value list_pop(ObjList *list);
void  list_clear(ObjList *list);
Value list_get(ObjList *list, int index);
void  list_set(ObjList *list, int index, Value value);
int   list_length(ObjList *list);
bool  list_contains(ObjList *list, Value value);

/* Dict operations */
void  dict_set(void *dict, Value key, Value value);
Value dict_get(void *dict, Value key);
bool  dict_has(void *dict, Value key);
Value dict_remove(void *dict, Value key);
void  dict_clear(void *dict);
int   dict_length(void *dict);
Value dict_keys(void *dict);
Value dict_values(void *dict);

/* Instance field operations */
Value instance_get_field(ObjInstance *inst, const char *name);
void  instance_set_field(ObjInstance *inst, const char *name, Value value);

/* Class method helpers */
void class_add_native_method(void *cls, const char *name, NativeFn fn);
struct ObjFunction *class_find_method(ObjClass *cls, const char *name);

void runtime_error(const char *fmt, ...);
int utf8_code_point_count(const char *s, int byte_len);

#endif /* LUNA_OBJECT_H */
