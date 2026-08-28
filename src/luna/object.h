#ifndef LUNA_OBJECT_H
#define LUNA_OBJECT_H

#include "value.h"

/* ============================================================
 * Luna object model (frontend-specific).
 *
 * The VM core is language-agnostic; this file owns Luna's types, their
 * kind predicates, and the operations the VM currently dispatches to.
 * When the MOP vtable lands (docs/architecture.md), the per-kind switches
 * move into vtable implementations and the core stops switching on ObjType.
 * ============================================================ */

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
    OBJ_VECTOR = 15,
    OBJ_MATRIX = 16
} ObjType;

typedef struct Object {
    Type          *type;   /* MOP vtable pointer (frontend-owned); replaces the inline ObjType tag */
    uint8_t        gc_color;
    size_t         size;
    struct Object *next;
    struct Object *prev;
    struct Object *finalizer_next;
    struct Object *finalizer_prev;
} Object;

/* Heap object kind checks: compare the object's Type* against the table entry
 * for kind `t`. No inline type tag; the type lives in the heap object. */
#define IS_OBJ_KIND(v, t) (IS_OBJ(v) && AS_OBJ(v)->type == luna_types[(t)])
#define IS_STRING(v)   IS_OBJ_KIND(v, OBJ_STRING)
#define IS_LIST(v)     IS_OBJ_KIND(v, OBJ_LIST)
#define IS_DICT(v)     IS_OBJ_KIND(v, OBJ_DICT)
#define IS_INSTANCE(v) IS_OBJ_KIND(v, OBJ_INSTANCE)
#define IS_FUNCTION(v) IS_OBJ_KIND(v, OBJ_FUNCTION)
#define IS_CLOSURE(v)  IS_OBJ_KIND(v, OBJ_CLOSURE)
#define IS_ENUM(v)     IS_OBJ_KIND(v, OBJ_ENUM)
#define IS_CLASS(v)    IS_OBJ_KIND(v, OBJ_CLASS)
#define IS_BOUND_METHOD(v) IS_OBJ_KIND(v, OBJ_BOUND_METHOD)
#define IS_MODULE(v)       IS_OBJ_KIND(v, OBJ_MODULE)
#define IS_BUFFER(v)       IS_OBJ_KIND(v, OBJ_BUFFER)
#define IS_USERDATA(v)     IS_OBJ_KIND(v, OBJ_USERDATA)
#define IS_VECTOR(v)       IS_OBJ_KIND(v, OBJ_VECTOR)
#define IS_MATRIX(v)       IS_OBJ_KIND(v, OBJ_MATRIX)
#define IS_INT64(v)    IS_OBJ_KIND(v, OBJ_INT64)

/* ============================================================
 * MOP: language-agnostic operation dispatch via vtable.
 *
 * The frontend (Luna) defines one `Type` instance per heap-object kind and
 * fills its vtable. In Part 1 only `name`/`kind` are used; the function
 * pointers are wired in later parts (arith/cmp, index, attr, call). The VM
 * core reaches operations through `Object.type` and never names a concrete
 * Luna type. Immediate int/double are handled natively by the core.
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

typedef struct Type {
    const char *name;   /* human-readable type name (e.g. "list") */
    ObjType     kind;   /* frontend discriminator: ObjType value, for fast-path caches / GC */
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
} Type;

/* Indexed by ObjType so the core can map a kind to its Type* (used by the
 * IS_X predicates and by constructors). Defined in luna/object.c. */
extern Type *luna_types[];

struct VM;
struct ObjClass;
struct ObjClass *get_class(struct VM *vm, Value val);

typedef Value (*NativeFn)(struct VM *vm, Value *args, int arg_count);

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

typedef struct ObjInt64 {
    Object   obj;
    uint64_t value;
} ObjInt64;

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
    float  data[4];
} ObjVector;

typedef struct ObjMatrix {
    Object obj;
    float  m[16];
} ObjMatrix;

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
    int (*cfunc)(struct luna_State *L);
    int upvalue_count;
    UpvalueDesc *upvalue_descriptors;
    struct FunctionParam *params;
    struct Stmt **body;
    int body_count;
    struct Environment *closure;
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

typedef struct {
    Object   obj;
    char    *name;
    char   **names;
    int64_t *values;
    int      count;
} ObjEnum;

/* Object constructors / operations (defined in value.c, temporarily). */
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
ObjVector      *new_vector(float x, float y, float z, float w);
ObjMatrix      *new_matrix(void);
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
static inline double value_to_double(Value v) {
    if (IS_INT(v)) return (double)AS_INT(v);
    if (IS_INT64(v)) return (double)((int64_t)((ObjInt64*)AS_OBJ(v))->value);
    return AS_DOUBLE(v);
}

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
void  dict_set(ObjDict *dict, Value key, Value value);
Value dict_get(ObjDict *dict, Value key);
bool  dict_has(ObjDict *dict, Value key);
Value dict_remove(ObjDict *dict, Value key);
void  dict_clear(ObjDict *dict);
int   dict_length(ObjDict *dict);
Value dict_keys(ObjDict *dict);
Value dict_values(ObjDict *dict);

/* Instance field operations */
Value instance_get_field(ObjInstance *inst, const char *name);
void  instance_set_field(ObjInstance *inst, const char *name, Value value);

/* Class method helpers */
void class_add_native_method(ObjClass *cls, const char *name, NativeFn fn);
struct ObjFunction *class_find_method(ObjClass *cls, const char *name);

void runtime_error(const char *fmt, ...);
int utf8_code_point_count(const char *s, int byte_len);

#endif /* LUNA_OBJECT_H */
