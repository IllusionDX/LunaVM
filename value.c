/* Value implementation for Luna interpreter.
 * Includes memory management and ARC (Automatic Reference Counting).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "value.h"

/* ============== Value constructors ============== */

Value make_null(void) {
    Value v;
    v.type = VAL_NULL;
    return v;
}

Value make_bool(bool value) {
    Value v;
    v.type = VAL_BOOL;
    v.as.boolean = value;
    return v;
}

Value make_int(int32_t value) {
    Value v;
    v.type = VAL_INT;
    v.as.integer = value;
    return v;
}

Value make_long(int64_t value) {
    Value v;
    v.type = VAL_LONG;
    v.as.long_val = value;
    return v;
}

Value make_float(float value) {
    Value v;
    v.type = VAL_FLOAT;
    v.as.float_val = value;
    return v;
}

Value make_double(double value) {
    Value v;
    v.type = VAL_DOUBLE;
    v.as.double_val = value;
    return v;
}

Value make_char(char value) {
    Value v;
    v.type = VAL_CHAR;
    v.as.character = value;
    return v;
}

Value make_byte(uint8_t value) {
    Value v;
    v.type = VAL_BYTE;
    v.as.byte_val = value;
    return v;
}

Value make_obj(Object *obj) {
    Value v;
    v.type = VAL_OBJ;
    v.as.obj = obj;
    return v;
}

/* ============== String interning hash function ============== */

static uint32_t hash_string(const char *key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

/* ============== Object constructors ============== */

ObjString *new_string(const char *chars, int length) {
    ObjString *string = (ObjString *)malloc(sizeof(ObjString));
    if (!string) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    string->obj.type = OBJ_STRING;
    string->obj.refcount = 1;
    string->obj.next = NULL;
    string->length = length;
    string->chars = (char *)malloc(length + 1);
    if (!string->chars) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memcpy(string->chars, chars, length);
    string->chars[length] = '\0';
    string->hash = hash_string(chars, length);
    
    return string;
}

ObjString *copy_string(const char *chars, int length) {
    return new_string(chars, length);
}

ObjList *new_list(Type *element_type) {
    ObjList *list = (ObjList *)malloc(sizeof(ObjList));
    if (!list) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    list->obj.type = OBJ_LIST;
    list->obj.refcount = 1;
    list->obj.next = NULL;
    list->element_type = element_type;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    
    extern ObjFunction **global_list_methods;
    extern int global_list_method_count;
    list->methods = global_list_methods;
    list->method_count = global_list_method_count;
    
    return list;
}

ObjMap *new_map(Type *key_type, Type *value_type) {
    ObjMap *map = (ObjMap *)malloc(sizeof(ObjMap));
    if (!map) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    map->obj.type = OBJ_MAP;
    map->obj.refcount = 1;
    map->obj.next = NULL;
    map->key_type = key_type;
    map->value_type = value_type;
    map->bucket_count = 16;
    map->entry_count = 0;
    map->buckets = (ValueMapEntry **)calloc(map->bucket_count, sizeof(ValueMapEntry *));
    if (!map->buckets) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    extern ObjFunction **global_map_methods;
    extern int global_map_method_count;
    map->methods = global_map_methods;
    map->method_count = global_map_method_count;
    
    return map;
}

ObjStruct *new_struct(const char *name, int field_count) {
    ObjStruct *strct = (ObjStruct *)malloc(sizeof(ObjStruct));
    if (!strct) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    strct->obj.type = OBJ_STRUCT;
    strct->obj.refcount = 1;
    strct->obj.next = NULL;
    strct->struct_name = strdup(name);
    strct->field_count = field_count;
    strct->fields = (Value *)calloc(field_count, sizeof(Value));
    if (!strct->fields) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    return strct;
}

ObjInstance *new_instance(const char *class_name, const char *base_class, int field_count) {
    ObjInstance *inst = (ObjInstance *)malloc(sizeof(ObjInstance));
    if (!inst) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    inst->obj.type = OBJ_INSTANCE;
    inst->obj.refcount = 1;
    inst->obj.next = NULL;
    inst->class_name = strdup(class_name);
    inst->base_class = base_class ? strdup(base_class) : NULL;
    inst->field_count = 0;
    inst->field_capacity = field_count > 0 ? field_count : 4;
    inst->field_names = (char **)calloc(inst->field_capacity, sizeof(char *));
    inst->fields = (Value *)calloc(inst->field_capacity, sizeof(Value));
    inst->methods = NULL;
    inst->method_count = 0;
    
    if (!inst->fields || !inst->field_names) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    return inst;
}

ObjFunction *new_function(const char *name) {
    ObjFunction *func = (ObjFunction *)malloc(sizeof(ObjFunction));
    if (!func) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    func->obj.type = OBJ_FUNCTION;
    func->obj.refcount = 1;
    func->obj.next = NULL;
    func->name = strdup(name);
    func->params = NULL;
    func->param_count = 0;
    func->return_type = NULL;
    func->body = NULL;
    func->body_count = 0;
    func->closure = NULL;
    func->is_native = false;
    func->native_fn = NULL;
    
    return func;
}

ObjFunction *new_native_function(const char *name, NativeFn native_fn) {
    ObjFunction *func = new_function(name);
    func->is_native = true;
    func->native_fn = native_fn;
    return func;
}

ObjException *new_exception(const char *message) {
    ObjException *exc = (ObjException *)malloc(sizeof(ObjException));
    if (!exc) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    exc->obj.type = OBJ_EXCEPTION;
    exc->obj.refcount = 1;
    exc->obj.next = NULL;
    exc->message = strdup(message);
    exc->line = 0;
    exc->file = strdup("<unknown>");
    
    return exc;
}

/* ============== Value operations ============== */

bool is_null(Value value) {
    return value.type == VAL_NULL;
}

bool is_truthy(Value value) {
    switch (value.type) {
        case VAL_NULL: return false;
        case VAL_BOOL: return value.as.boolean;
        case VAL_INT: return value.as.integer != 0;
        case VAL_LONG: return value.as.long_val != 0;
        case VAL_FLOAT: return value.as.float_val != 0.0f;
        case VAL_DOUBLE: return value.as.double_val != 0.0;
        case VAL_CHAR: return value.as.character != '\0';
        case VAL_BYTE: return value.as.byte_val != 0;
        case VAL_OBJ: return true;
        default: return false;
    }
}

bool values_equal(Value a, Value b) {
    if (a.type != b.type) return false;
    
    switch (a.type) {
        case VAL_NULL: return true;
        case VAL_BOOL: return a.as.boolean == b.as.boolean;
        case VAL_INT: return a.as.integer == b.as.integer;
        case VAL_LONG: return a.as.long_val == b.as.long_val;
        case VAL_FLOAT: return a.as.float_val == b.as.float_val;
        case VAL_DOUBLE: return a.as.double_val == b.as.double_val;
        case VAL_CHAR: return a.as.character == b.as.character;
        case VAL_BYTE: return a.as.byte_val == b.as.byte_val;
        case VAL_OBJ: {
            if (a.as.obj->type != b.as.obj->type) return false;
            if (a.as.obj->type == OBJ_STRING) {
                ObjString *sa = (ObjString *)a.as.obj;
                ObjString *sb = (ObjString *)b.as.obj;
                return sa->length == sb->length && memcmp(sa->chars, sb->chars, sa->length) == 0;
            }
            return a.as.obj == b.as.obj;
        }
        default: return false;
    }
}

char *value_to_string(Value value) {
    char buffer[256];
    
    switch (value.type) {
        case VAL_NULL:
            return strdup("null");
        case VAL_BOOL:
            return strdup(value.as.boolean ? "true" : "false");
        case VAL_INT:
            snprintf(buffer, sizeof(buffer), "%d", value.as.integer);
            return strdup(buffer);
        case VAL_LONG:
            snprintf(buffer, sizeof(buffer), "%lld", (long long)value.as.long_val);
            return strdup(buffer);
        case VAL_FLOAT:
            snprintf(buffer, sizeof(buffer), "%g", value.as.float_val);
            return strdup(buffer);
        case VAL_DOUBLE:
            snprintf(buffer, sizeof(buffer), "%g", value.as.double_val);
            return strdup(buffer);
        case VAL_CHAR:
            buffer[0] = value.as.character;
            buffer[1] = '\0';
            return strdup(buffer);
        case VAL_BYTE:
            snprintf(buffer, sizeof(buffer), "%u", value.as.byte_val);
            return strdup(buffer);
        case VAL_OBJ: {
            switch (value.as.obj->type) {
                case OBJ_STRING:
                    return strdup(((ObjString *)value.as.obj)->chars);
                case OBJ_FUNCTION:
                    snprintf(buffer, sizeof(buffer), "<fn %s>", 
                             ((ObjFunction *)value.as.obj)->name);
                    return strdup(buffer);
                case OBJ_LIST: {
                    ObjList *list = (ObjList *)value.as.obj;
                    /* Build [elem, elem, ...] */
                    int buf_cap = 64;
                    char *buf = (char *)malloc(buf_cap);
                    int pos = 0;
                    buf[pos++] = '[';
                    for (int i = 0; i < list->count; i++) {
                        char *elem = value_to_string(list->items[i]);
                        int elen = (int)strlen(elem);
                        /* check if value is a string type to add quotes */
                        bool is_str = (list->items[i].type == VAL_OBJ &&
                                       list->items[i].as.obj->type == OBJ_STRING);
                        int needed = pos + elen + (is_str ? 2 : 0) + 3;
                        if (needed >= buf_cap) {
                            buf_cap = needed * 2;
                            buf = (char *)realloc(buf, buf_cap);
                        }
                        if (i > 0) { buf[pos++] = ','; buf[pos++] = ' '; }
                        if (is_str) buf[pos++] = '"';
                        memcpy(buf + pos, elem, elen); pos += elen;
                        if (is_str) buf[pos++] = '"';
                        free(elem);
                    }
                    buf[pos++] = ']';
                    buf[pos] = '\0';
                    char *result = strdup(buf);
                    free(buf);
                    return result;
                }
                case OBJ_MAP: {
                    ObjMap *map = (ObjMap *)value.as.obj;
                    int buf_cap = 64;
                    char *buf = (char *)malloc(buf_cap);
                    int pos = 0;
                    buf[pos++] = '{';
                    bool first = true;
                    for (int i = 0; i < map->bucket_count; i++) {
                        ValueMapEntry *entry = map->buckets[i];
                        while (entry) {
                            char *k = value_to_string(entry->key);
                            char *v = value_to_string(entry->value);
                            bool k_str = (entry->key.type == VAL_OBJ &&
                                          entry->key.as.obj->type == OBJ_STRING);
                            bool v_str = (entry->value.type == VAL_OBJ &&
                                          entry->value.as.obj->type == OBJ_STRING);
                            int klen = (int)strlen(k);
                            int vlen = (int)strlen(v);
                            int needed = pos + klen + vlen + (k_str?2:0) + (v_str?2:0) + 6;
                            if (needed >= buf_cap) {
                                buf_cap = needed * 2;
                                buf = (char *)realloc(buf, buf_cap);
                            }
                            if (!first) { buf[pos++] = ','; buf[pos++] = ' '; }
                            first = false;
                            if (k_str) buf[pos++] = '"';
                            memcpy(buf+pos, k, klen); pos += klen;
                            if (k_str) buf[pos++] = '"';
                            buf[pos++] = ':'; buf[pos++] = ' ';
                            if (v_str) buf[pos++] = '"';
                            memcpy(buf+pos, v, vlen); pos += vlen;
                            if (v_str) buf[pos++] = '"';
                            free(k); free(v);
                            entry = entry->next;
                        }
                    }
                    buf[pos++] = '}';
                    buf[pos] = '\0';
                    char *result = strdup(buf);
                    free(buf);
                    return result;
                }
                case OBJ_INSTANCE:
                    snprintf(buffer, sizeof(buffer), "<instance of %s>",
                             ((ObjInstance *)value.as.obj)->class_name);
                    return strdup(buffer);
                case OBJ_EXCEPTION:
                    return strdup(((ObjException *)value.as.obj)->message);
                default:
                    return strdup("<object>");
            }
        }
        default:
            return strdup("<unknown>");
    }
}

Type *value_to_type(Value value) {
    Type *type = (Type *)malloc(sizeof(Type));
    if (!type) return NULL;
    
    switch (value.type) {
        case VAL_NULL:
            type->kind = TYPE_BASE;
            type->data.base.name = strdup("null");
            break;
        case VAL_BOOL:
            type->kind = TYPE_BASE;
            type->data.base.name = strdup("bool");
            break;
        case VAL_INT:
            type->kind = TYPE_BASE;
            type->data.base.name = strdup("int");
            break;
        case VAL_LONG:
            type->kind = TYPE_BASE;
            type->data.base.name = strdup("long");
            break;
        case VAL_FLOAT:
            type->kind = TYPE_BASE;
            type->data.base.name = strdup("float");
            break;
        case VAL_DOUBLE:
            type->kind = TYPE_BASE;
            type->data.base.name = strdup("double");
            break;
        case VAL_CHAR:
            type->kind = TYPE_BASE;
            type->data.base.name = strdup("char");
            break;
        case VAL_BYTE:
            type->kind = TYPE_BASE;
            type->data.base.name = strdup("byte");
            break;
        case VAL_OBJ:
            type->kind = TYPE_BASE;
            switch (value.as.obj->type) {
                case OBJ_STRING:
                    type->data.base.name = strdup("string");
                    break;
                case OBJ_LIST: {
                    ObjList *list = (ObjList *)value.as.obj;
                    type->kind = TYPE_LIST;
                    type->data.list.element_type = list->element_type;
                    break;
                }
                case OBJ_MAP: {
                    ObjMap *map = (ObjMap *)value.as.obj;
                    type->kind = TYPE_MAP;
                    type->data.map.key_type = map->key_type;
                    type->data.map.value_type = map->value_type;
                    break;
                }
                default:
                    type->data.base.name = strdup("object");
                    break;
            }
            break;
        default:
            type->kind = TYPE_BASE;
            type->data.base.name = strdup("unknown");
            break;
    }
    
    return type;
}

/* ============== ARC Memory Management ============== */

void retain_obj(Object *obj) {
    if (obj == NULL) return;
    obj->refcount++;
}

void release_obj(Object *obj) {
    if (obj == NULL) return;
    
    obj->refcount--;
    if (obj->refcount <= 0) {
        free_object(obj);
    }
}

static void release_value(Value value);

static void free_map(ObjMap *map) {
    for (int i = 0; i < map->bucket_count; i++) {
        ValueMapEntry *entry = map->buckets[i];
        while (entry != NULL) {
            ValueMapEntry *next = entry->next;
            release_value(entry->key);
            release_value(entry->value);
            free(entry);
            entry = next;
        }
    }
    free(map->buckets);
    free(map);
}

static void free_list(ObjList *list) {
    for (int i = 0; i < list->count; i++) {
        release_value(list->items[i]);
    }
    free(list->items);
    free(list);
}

static void free_function(ObjFunction *func) {
    free(func->name);
    free(func->params);
    free(func);
}

void free_object(Object *obj) {
    if (obj == NULL) return;
    
    switch (obj->type) {
        case OBJ_STRING: {
            ObjString *str = (ObjString *)obj;
            free(str->chars);
            free(str);
            break;
        }
        case OBJ_LIST: {
            free_list((ObjList *)obj);
            break;
        }
        case OBJ_MAP: {
            free_map((ObjMap *)obj);
            break;
        }
        case OBJ_STRUCT: {
            ObjStruct *strct = (ObjStruct *)obj;
            free(strct->struct_name);
            for (int i = 0; i < strct->field_count; i++) {
                release_value(strct->fields[i]);
            }
            free(strct->fields);
            free(strct);
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance *inst = (ObjInstance *)obj;
            free(inst->class_name);
            if (inst->base_class) free(inst->base_class);
            for (int i = 0; i < inst->field_count; i++) {
                release_value(inst->fields[i]);
                if (inst->field_names && inst->field_names[i]) free(inst->field_names[i]);
            }
            free(inst->field_names);
            free(inst->fields);
            free(inst->methods);
            free(inst);
            break;
        }
        case OBJ_FUNCTION: {
            free_function((ObjFunction *)obj);
            break;
        }
        case OBJ_EXCEPTION: {
            ObjException *exc = (ObjException *)obj;
            free(exc->message);
            free(exc->file);
            free(exc);
            break;
        }
        default:
            free(obj);
            break;
    }
}

static void release_value(Value value) {
    if (value.type == VAL_OBJ) {
        release_obj(value.as.obj);
    }
}

/* ============== Instance field operations ============== */

Value instance_get_field(ObjInstance *inst, const char *name) {
    for (int i = 0; i < inst->field_count; i++) {
        if (inst->field_names[i] && strcmp(inst->field_names[i], name) == 0) {
            return inst->fields[i];
        }
    }
    return make_null();
}

void instance_set_field(ObjInstance *inst, const char *name, Value value) {
    /* Update existing field */
    for (int i = 0; i < inst->field_count; i++) {
        if (inst->field_names[i] && strcmp(inst->field_names[i], name) == 0) {
            inst->fields[i] = value;
            return;
        }
    }
    /* Add new field */
    if (inst->field_count >= inst->field_capacity) {
        inst->field_capacity = inst->field_capacity < 4 ? 4 : inst->field_capacity * 2;
        inst->field_names = (char **)realloc(inst->field_names, inst->field_capacity * sizeof(char *));
        inst->fields = (Value *)realloc(inst->fields, inst->field_capacity * sizeof(Value));
    }
    inst->field_names[inst->field_count] = strdup(name);
    inst->fields[inst->field_count] = value;
    inst->field_count++;
}

/* ============== Collection operations ============== */


void list_add(ObjList *list, Value value) {
    if (list->count >= list->capacity) {
        int new_capacity = list->capacity < 8 ? 8 : list->capacity * 2;
        Value *new_items = (Value *)realloc(list->items, new_capacity * sizeof(Value));
        if (!new_items) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }
    
    list->items[list->count] = value;
    if (value.type == VAL_OBJ) {
        retain_obj(value.as.obj);
    }
    list->count++;
}

void list_insert(ObjList *list, int index, Value value) {
    if (index < 0 || index > list->count) {
        fprintf(stderr, "List index out of bounds\n");
        return;
    }
    
    if (list->count >= list->capacity) {
        int new_capacity = list->capacity < 8 ? 8 : list->capacity * 2;
        Value *new_items = (Value *)realloc(list->items, new_capacity * sizeof(Value));
        if (!new_items) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }
    
    memmove(&list->items[index + 1], &list->items[index], 
            (list->count - index) * sizeof(Value));
    list->items[index] = value;
    if (value.type == VAL_OBJ) {
        retain_obj(value.as.obj);
    }
    list->count++;
}

Value list_remove(ObjList *list, int index) {
    if (index < 0 || index >= list->count) {
        fprintf(stderr, "List index out of bounds\n");
        return make_null();
    }
    
    Value value = list->items[index];
    memmove(&list->items[index], &list->items[index + 1],
            (list->count - index - 1) * sizeof(Value));
    list->count--;
    return value;
}

Value list_pop(ObjList *list) {
    if (list->count == 0) {
        fprintf(stderr, "Cannot pop from empty list\n");
        return make_null();
    }
    return list_remove(list, list->count - 1);
}

void list_clear(ObjList *list) {
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].type == VAL_OBJ) {
            release_obj(list->items[i].as.obj);
        }
    }
    list->count = 0;
}

Value list_get(ObjList *list, int index) {
    if (index < 0 || index >= list->count) {
        fprintf(stderr, "List index out of bounds\n");
        return make_null();
    }
    return list->items[index];
}

void list_set(ObjList *list, int index, Value value) {
    if (index < 0 || index >= list->count) {
        fprintf(stderr, "List index out of bounds\n");
        return;
    }
    
    if (list->items[index].type == VAL_OBJ) {
        release_obj(list->items[index].as.obj);
    }
    list->items[index] = value;
    if (value.type == VAL_OBJ) {
        retain_obj(value.as.obj);
    }
}

int list_length(ObjList *list) {
    return list->count;
}

/* ============== Hashing ============== */

uint32_t hash_value(Value value) {
    switch (value.type) {
        case VAL_NULL: return 0;
        case VAL_BOOL: return value.as.boolean ? 1 : 0;
        case VAL_INT: return (uint32_t)value.as.integer;
        case VAL_LONG: return (uint32_t)value.as.long_val;
        case VAL_FLOAT: {
            union { float f; uint32_t i; } u;
            u.f = value.as.float_val;
            return u.i;
        }
        case VAL_DOUBLE: {
            union { double d; uint64_t i; } u;
            u.d = value.as.double_val;
            return (uint32_t)u.i;
        }
        case VAL_CHAR: return (uint32_t)value.as.character;
        case VAL_BYTE: return (uint32_t)value.as.byte_val;
        case VAL_OBJ: {
            if (value.as.obj->type == OBJ_STRING) {
                return ((ObjString *)value.as.obj)->hash;
            }
            return (uint32_t)(uintptr_t)value.as.obj;
        }
        default: return 0;
    }
}

/* ============== Map operations ============== */

static void resize_map(ObjMap *map) {
    int new_bucket_count = map->bucket_count * 2;
    ValueMapEntry **new_buckets = (ValueMapEntry **)calloc(new_bucket_count, sizeof(ValueMapEntry *));
    if (!new_buckets) {
        fprintf(stderr, "Out of memory\n");
        return;
    }
    
    for (int i = 0; i < map->bucket_count; i++) {
        ValueMapEntry *entry = map->buckets[i];
        while (entry != NULL) {
            ValueMapEntry *next = entry->next;
            uint32_t hash = hash_value(entry->key);
            int index = hash & (new_bucket_count - 1);
            entry->next = new_buckets[index];
            new_buckets[index] = entry;
            entry = next;
        }
    }
    
    free(map->buckets);
    map->buckets = new_buckets;
    map->bucket_count = new_bucket_count;
}

void map_set(ObjMap *map, Value key, Value value) {
    if (map->entry_count >= map->bucket_count * 0.75) {
        resize_map(map);
    }
    
    uint32_t hash = hash_value(key);
    int index = hash & (map->bucket_count - 1);
    
    ValueMapEntry *entry = map->buckets[index];
    while (entry != NULL) {
        if (values_equal(entry->key, key)) {
            release_value(entry->value);
            entry->value = value;
            if (value.type == VAL_OBJ) {
                retain_obj(value.as.obj);
            }
            return;
        }
        entry = entry->next;
    }
    
    entry = (ValueMapEntry *)malloc(sizeof(ValueMapEntry));
    if (!entry) {
        fprintf(stderr, "Out of memory\n");
        return;
    }
    
    entry->key = key;
    entry->value = value;
    entry->next = map->buckets[index];
    map->buckets[index] = entry;
    
    if (key.type == VAL_OBJ) {
        retain_obj(key.as.obj);
    }
    if (value.type == VAL_OBJ) {
        retain_obj(value.as.obj);
    }
    
    map->entry_count++;
}

Value map_get(ObjMap *map, Value key) {
    uint32_t hash = hash_value(key);
    int index = hash & (map->bucket_count - 1);
    
    ValueMapEntry *entry = map->buckets[index];
    while (entry != NULL) {
        if (values_equal(entry->key, key)) {
            return entry->value;
        }
        entry = entry->next;
    }
    
    return make_null();
}

bool map_has(ObjMap *map, Value key) {
    uint32_t hash = hash_value(key);
    int index = hash & (map->bucket_count - 1);
    
    ValueMapEntry *entry = map->buckets[index];
    while (entry != NULL) {
        if (values_equal(entry->key, key)) {
            return true;
        }
        entry = entry->next;
    }
    
    return false;
}

Value map_remove(ObjMap *map, Value key) {
    uint32_t hash = hash_value(key);
    int index = hash & (map->bucket_count - 1);
    
    ValueMapEntry **current = &map->buckets[index];
    while (*current != NULL) {
        ValueMapEntry *entry = *current;
        if (values_equal(entry->key, key)) {
            *current = entry->next;
            Value value = entry->value;
            release_value(entry->key);
            release_value(entry->value);
            free(entry);
            map->entry_count--;
            return value;
        }
        current = &entry->next;
    }
    
    return make_null();
}

void map_clear(ObjMap *map) {
    for (int i = 0; i < map->bucket_count; i++) {
        ValueMapEntry *entry = map->buckets[i];
        while (entry != NULL) {
            ValueMapEntry *next = entry->next;
            release_value(entry->key);
            release_value(entry->value);
            free(entry);
            entry = next;
        }
        map->buckets[i] = NULL;
    }
    map->entry_count = 0;
}

int map_length(ObjMap *map) {
    return map->entry_count;
}

/* ============== Exception handling ============== */

void runtime_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

void throw_exception(Value value) {
    (void)value;
    /* Exception handling is managed by the evaluator */
}
