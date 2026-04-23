/* value.h — Luna VM value and object types.
 *
 * Matches IMPLEMENTATION.md V1 type tags exactly.
 * No AST dependency — this file is self-contained.
 *
 * Value type tags:
 *   0  VAL_NULL    null
 *   1  VAL_BOOL    true / false
 *   2  VAL_INT     int64_t
 *   3  VAL_UINT    uint64_t
 *   4  VAL_FLOAT   float (32-bit)
 *   5  VAL_DOUBLE  double (64-bit)
 *   6  VAL_NAN     IEEE-754 NaN sentinel
 *   7  VAL_OBJ → OBJ_STRING
 *   8  VAL_OBJ → OBJ_LIST
 *   9  VAL_OBJ → OBJ_DICT
 *   10 VAL_OBJ → OBJ_INSTANCE
 *   11 VAL_OBJ → OBJ_FUNCTION
 *   12 VAL_OBJ → OBJ_NATIVE   (same struct, is_native == true)
 *   13 VAL_CHAR   uint32_t (Unicode code-point)
 */

#ifndef LUNA_VALUE_H
#define LUNA_VALUE_H

#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
typedef struct Object   Object;
struct Chunk;   /* defined in chunk.h — avoid circular include */
struct VM;      /* defined in vm.h */

/* ============================================================ */
/* Value type enum                                               */
/* ============================================================ */

typedef enum {
    VAL_NULL,    /* 0  */
    VAL_BOOL,    /* 1  */
    VAL_INT,     /* 2  — int64_t  */
    VAL_UINT,    /* 3  — uint64_t */
    VAL_FLOAT,   /* 4  — float    */
    VAL_DOUBLE,  /* 5  — double   */
    VAL_NAN,     /* 6  — IEEE 754 NaN sentinel (no payload) */
    VAL_CHAR,    /* 13 — Unicode code-point uint32_t */
    VAL_OBJ      /* heap pointer — ObjType sub-tag identifies 7-12 */
} ValueType;

/* ============================================================ */
/* Tagged value                                                  */
/* ============================================================ */

typedef struct Value {
    ValueType type;
    union {
        bool     boolean;
        int64_t  integer;    /* VAL_INT    */
        uint64_t uint_val;   /* VAL_UINT   */
        float    float_val;  /* VAL_FLOAT  */
        double   double_val; /* VAL_DOUBLE */
        uint32_t char_val;   /* VAL_CHAR   (Unicode code-point) */
        Object  *obj;        /* VAL_OBJ    */
    } as;
} Value;

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

/* List — dynamic array (no embedded type info — dynamic typing) */
typedef struct {
    Object  obj;
    Value  *items;
    int     count;
    int     capacity;
} ObjList;

/* Dict entry (internal chaining node) */
typedef struct DictNode {
    Value             key;
    Value             value;
    struct DictNode *next;
} DictNode;

/* Dict — hash-map with Value keys and Value values */
typedef struct {
    Object     obj;
    DictNode **buckets;
    int         bucket_count;
    int         entry_count;
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
    Value       *location;      /* pointer to the live value */
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
/* Value constructors                                            */
/* ============================================================ */

Value make_null(void);
Value make_bool(bool value);
Value make_int(int64_t value);
Value make_uint(uint64_t value);
Value make_float(float value);
Value make_double(double value);
Value make_nan(void);
Value make_char(uint32_t codepoint);
Value make_obj(Object *obj);

/* ============================================================ */
/* Object constructors                                           */
/* ============================================================ */

ObjString   *new_string(const char *chars, int length);
ObjList     *new_list(void);
ObjDict     *new_dict(void);
ObjInstance *new_instance(const char *class_name, const char *base_class,
                          int initial_capacity);
ObjFunction *new_function(const char *name);
ObjFunction *new_native_function(const char *name, NativeFn fn);
ObjException *new_exception(const char *message);
ObjUpvalue  *new_upvalue(Value *slot);
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

void retain_obj(Object *obj);
void release_obj(Object *obj);
void free_object(Object *obj);
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
