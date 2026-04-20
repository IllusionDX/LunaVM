/* Value types for the Luna interpreter.
 * Supports all Luna primitive types and reference types.
 */

#ifndef LUNA_VALUE_H
#define LUNA_VALUE_H

#include <stdbool.h>
#include <stdint.h>
#include "ast.h"

/* Forward declarations */
typedef struct Value Value;
typedef struct Object Object;
typedef struct Environment Environment;

/* Object types for garbage collection */
typedef enum {
    OBJ_STRING,
    OBJ_ARRAY,
    OBJ_LIST,
    OBJ_MAP,
    OBJ_STRUCT,
    OBJ_CLASS,
    OBJ_INSTANCE,
    OBJ_FUNCTION,
    OBJ_NATIVE,
    OBJ_EXCEPTION
} ObjType;

/* Base object header for garbage collection */
typedef struct Object {
    ObjType type;
    int refcount;
    struct Object *next;
} Object;

/* Native function signature */
typedef Value (*NativeFn)(Environment *env, Value *args, int arg_count);

/* Function object */
typedef struct {
    Object obj;
    char *name;
    FunctionParam *params;
    int param_count;
    Type *return_type;
    Stmt **body;
    int body_count;
    Environment *closure;
    bool is_native;
    NativeFn native_fn;
} ObjFunction;

/* String object */
typedef struct {
    Object obj;
    char *chars;
    int length;
    uint32_t hash;
} ObjString;

/* Array value entry */
typedef struct {
    Type *type;
    Value *values;
    int length;
} ArrayData;

/* List object (dynamic array) */
typedef struct {
    Object obj;
    Type *element_type;
    Value *items;
    int count;
    int capacity;
    ObjFunction **methods;
    int method_count;
} ObjList;

/* Forward declaration for Map */
struct ValueMapEntry;

/* Map object - forward declared */
typedef struct {
    Object obj;
    Type *key_type;
    Type *value_type;
    struct ValueMapEntry **buckets;
    int bucket_count;
    int entry_count;
    ObjFunction **methods;
    int method_count;
} ObjMap;

/* Struct instance */
typedef struct {
    Object obj;
    char *struct_name;
    Value *fields;
    int field_count;
} ObjStruct;

/* Class instance */
typedef struct {
    Object obj;
    char *class_name;
    char *base_class;
    char **field_names;  /* parallel array of field names */
    Value *fields;
    int field_count;
    int field_capacity;
    ObjFunction **methods;
    int method_count;
} ObjInstance;

/* Exception object */
typedef struct {
    Object obj;
    char *message;
    int line;
    char *file;
} ObjException;

/* Value types */
typedef enum {
    VAL_NULL,
    VAL_BOOL,
    VAL_INT,
    VAL_LONG,
    VAL_FLOAT,
    VAL_DOUBLE,
    VAL_CHAR,
    VAL_BYTE,
    VAL_OBJ
} ValueType;

/* Value representation */
typedef struct Value {
    ValueType type;
    union {
        bool boolean;
        int32_t integer;
        int64_t long_val;
        float float_val;
        double double_val;
        char character;
        uint8_t byte_val;
        Object *obj;
    } as;
} Value;

/* ============== Value constructors ============== */

Value make_null(void);
Value make_bool(bool value);
Value make_int(int32_t value);
Value make_long(int64_t value);
Value make_float(float value);
Value make_double(double value);
Value make_char(char value);
Value make_byte(uint8_t value);
Value make_obj(Object *obj);

/* ============== Object constructors ============== */

ObjString *new_string(const char *chars, int length);
ObjString *copy_string(const char *chars, int length);
ObjList *new_list(Type *element_type);
ObjMap *new_map(Type *key_type, Type *value_type);
ObjStruct *new_struct(const char *name, int field_count);
ObjInstance *new_instance(const char *class_name, const char *base_class, int field_count);
ObjFunction *new_function(const char *name);
ObjFunction *new_native_function(const char *name, NativeFn native_fn);
ObjException *new_exception(const char *message);

/* ============== Value operations ============== */

bool is_null(Value value);
bool is_truthy(Value value);
bool values_equal(Value a, Value b);
char *value_to_string(Value value);
Type *value_to_type(Value value);

/* ============== Object operations ============== */

void retain_obj(Object *obj);
void release_obj(Object *obj);
void free_object(Object *obj);

/* ============== Collection operations ============== */

void list_add(ObjList *list, Value value);
void list_insert(ObjList *list, int index, Value value);
Value list_remove(ObjList *list, int index);
Value list_pop(ObjList *list);
void list_clear(ObjList *list);
Value list_get(ObjList *list, int index);
void list_set(ObjList *list, int index, Value value);
int list_length(ObjList *list);

void map_set(ObjMap *map, Value key, Value value);
Value map_get(ObjMap *map, Value key);
bool map_has(ObjMap *map, Value key);
Value map_remove(ObjMap *map, Value key);
void map_clear(ObjMap *map);
int map_length(ObjMap *map);

/* ============== Instance field operations ============== */

Value instance_get_field(ObjInstance *inst, const char *name);
void instance_set_field(ObjInstance *inst, const char *name, Value value);

/* ============== Hashing ============== */

uint32_t hash_value(Value value);

/* ============== Exception handling ============== */

void runtime_error(const char *format, ...);

/* ============== Map entry for runtime ============== */

typedef struct ValueMapEntry {
    Value key;
    Value value;
    struct ValueMapEntry *next;
} ValueMapEntry;
void throw_exception(Value value);

#endif /* LUNA_VALUE_H */
