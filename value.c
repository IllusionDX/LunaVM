/* value.c — Luna VM value and object implementation (NaN-boxed). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "value.h"
#include "chunk.h"

/* ============================================================ */
/* Hashing                                                       */
/* ============================================================ */

static uint32_t fnv1a(const char *key, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= (uint8_t)key[i]; h *= 16777619u; }
    return h;
}

uint32_t hash_value(Value v) {
    if (IS_NIL(v)) return 0;
    if (IS_BOOL(v)) return AS_BOOL(v) ? 1u : 2u;
    if (IS_INT(v)) {
        int32_t i = AS_INT(v);
        return (uint32_t)(i ^ (i >> 16));
    }
    if (IS_DOUBLE(v)) {
        uint64_t u;
        memcpy(&u, &v, sizeof(u));
        return (uint32_t)(u ^ (u >> 32));
    }
    if (IS_OBJ(v)) {
        Object *obj = AS_OBJ(v);
        if (!obj) return 0;
        if (obj->type == OBJ_STRING)
            return ((ObjString *)obj)->hash;
        return (uint32_t)(uintptr_t)obj;
    }
    return 0;
}

/* ============================================================ */
/* String interning                                              */
/* ============================================================ */

#define INTERN_TABLE_SIZE 1024

typedef struct StringInternEntry {
    struct StringInternEntry *next;
    uint32_t hash;
    int length;
    ObjString *string;
} StringInternEntry;

static StringInternEntry *intern_table[INTERN_TABLE_SIZE];

static void intern_add(ObjString *s) {
    uint32_t bucket = s->hash & (INTERN_TABLE_SIZE - 1);
    StringInternEntry *e = malloc(sizeof(StringInternEntry));
    if (!e) { fprintf(stderr, "OOM\n"); exit(1); }
    e->hash = s->hash;
    e->length = s->length;
    e->string = s;
    e->next = intern_table[bucket];
    intern_table[bucket] = e;
}

static ObjString *intern_find(const char *chars, int length, uint32_t hash) {
    uint32_t bucket = hash & (INTERN_TABLE_SIZE - 1);
    for (StringInternEntry *e = intern_table[bucket]; e; e = e->next) {
        if (e->hash == hash && e->length == length && memcmp(e->string->chars, chars, length) == 0) {
            return e->string;
        }
    }
    return NULL;
}

static void intern_remove(ObjString *s) {
    uint32_t bucket = s->hash & (INTERN_TABLE_SIZE - 1);
    StringInternEntry **current = &intern_table[bucket];
    while (*current) {
        if ((*current)->string == s) {
            StringInternEntry *to_free = *current;
            *current = (*current)->next;
            free(to_free);
            return;
        }
        current = &(*current)->next;
    }
}

void value_free_intern_table(void) {
    for (int i = 0; i < INTERN_TABLE_SIZE; i++) {
        StringInternEntry *e = intern_table[i];
        while (e) {
            StringInternEntry *next = e->next;
            free(e);
            e = next;
        }
        intern_table[i] = NULL;
    }
}

/* ============================================================ */
/* Object constructors                                           */
/* ============================================================ */

Object *all_objects = NULL;
int allocated_objects = 0;
size_t bytes_allocated = 0;
size_t next_gc_threshold = 64 * 1024 * 1024;
bool gc_collecting = false;

static void init_object(Object *obj, ObjType type, size_t size) {
    obj->type = type;
    obj->refcount = 0;
    obj->is_marked = false;
    obj->size = size;
    bytes_allocated += size;
    obj->next = all_objects;
    obj->prev = NULL;
    if (all_objects) all_objects->prev = obj;
    all_objects = obj;
    allocated_objects++;
}

ObjString *new_string(const char *chars, int length) {
    uint32_t hash = fnv1a(chars, length);
    ObjString *existing = intern_find(chars, length, hash);
    if (existing) {
        return existing;
    }

    ObjString *s = malloc(sizeof(ObjString));
    if (!s) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)s, OBJ_STRING, sizeof(ObjString) + length + 1);
    s->chars = malloc(length + 1);
    if (!s->chars) { fprintf(stderr, "OOM\n"); exit(1); }
    memcpy(s->chars, chars, length);
    s->chars[length] = '\0';
    s->length = length;
    s->hash   = hash;
    intern_add(s);
    return s;
}

ObjList *new_list(int capacity) {
    ObjList *l = malloc(sizeof(ObjList));
    if (!l) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)l, OBJ_LIST, sizeof(ObjList) + (capacity > 4 ? capacity * sizeof(Value) : 0));
    l->count = 0;
    if (capacity > 4) {
        l->capacity = capacity;
        l->items = malloc(sizeof(Value) * l->capacity);
        if (!l->items) { fprintf(stderr, "OOM\n"); exit(1); }
    } else {
        l->capacity = 4;
        l->items = NULL; /* use inline storage */
    }
    return l;
}

ObjDict *new_dict(void) {
    ObjDict *d = malloc(sizeof(ObjDict));
    if (!d) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)d, OBJ_DICT, sizeof(ObjDict));
    d->indices = NULL;
    d->entries = NULL;
    d->capacity = 0;
    d->entry_count = 0;
    d->next_entry = 0;
    d->deleted_count = 0;
    return d;
}

/* Transition an SOO dict (inline_entries) to full heap layout */
static void dict_transition_to_heap(ObjDict *d) {
    ObjDictEntry old[4];
    int old_count = d->entry_count;
    memcpy(old, d->inline_entries, sizeof(ObjDictEntry) * old_count);

    d->capacity = 8;
    d->indices = malloc(sizeof(int) * 8);
    if (!d->indices) { fprintf(stderr, "OOM\n"); exit(1); }
    for (int i = 0; i < 8; i++) d->indices[i] = -1;
    d->entries = malloc(sizeof(ObjDictEntry) * 8);
    if (!d->entries) { fprintf(stderr, "OOM\n"); exit(1); }
    d->next_entry = 0;
    d->entry_count = 0;
    d->deleted_count = 0;

    for (int i = 0; i < old_count; i++) {
        uint32_t perturb = old[i].hash;
        uint32_t mask = d->capacity - 1;
        uint32_t idx = old[i].hash & mask;
        while (d->indices[idx] != -1) {
            idx = ((idx << 2) + idx + perturb + 1) & mask;
            perturb >>= 5;
        }
        int e_idx = d->next_entry++;
        d->indices[idx] = e_idx;
        d->entries[e_idx] = old[i];
        d->entry_count++;
    }
}

ObjInstance *new_instance(const char *class_name, const char *base_class, int cap) {
    ObjInstance *inst = malloc(sizeof(ObjInstance));
    if (!inst) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)inst, OBJ_INSTANCE, sizeof(ObjInstance) + sizeof(Value) * cap + sizeof(ObjFunction*) * 0);
    inst->class_name    = strdup(class_name);
    inst->base_class    = base_class ? strdup(base_class) : NULL;
    inst->field_capacity = cap > 0 ? cap : 4;
    inst->field_count   = 0;
    inst->field_names   = calloc(inst->field_capacity, sizeof(char *));
    inst->fields        = calloc(inst->field_capacity, sizeof(Value));
    inst->methods       = NULL;
    inst->method_count  = 0;
    return inst;
}

ObjFunction *new_function(const char *name) {
    ObjFunction *f = malloc(sizeof(ObjFunction));
    if (!f) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)f, OBJ_FUNCTION, sizeof(ObjFunction));
    f->name        = strdup(name ? name : "<fn>");
    f->chunk       = NULL;
    f->param_names = NULL;
    f->param_count = 0;
    f->upvalue_count = 0;
    f->upvalue_descriptors = NULL;
    f->is_native   = false;
    f->native_fn   = NULL;
    return f;
}

ObjFunction *new_native_function(const char *name, NativeFn fn) {
    ObjFunction *f = new_function(name);
    f->is_native = true;
    f->native_fn = fn;
    return f;
}

ObjException *new_exception(const char *message) {
    ObjException *e = malloc(sizeof(ObjException));
    if (!e) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)e, OBJ_EXCEPTION, sizeof(ObjException) + strlen(message) + 1);
    e->message = strdup(message);
    e->line    = 0;
    e->file    = strdup("<unknown>");
    return e;
}

ObjUpvalue *new_upvalue(int stack_index) {
    ObjUpvalue *uv = malloc(sizeof(ObjUpvalue));
    if (!uv) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)uv, OBJ_UPVALUE, sizeof(ObjUpvalue));
    uv->stack_index = stack_index;
    uv->is_open     = true;
    uv->closed      = make_null();
    uv->next        = NULL;
    uv->frame_depth = 0;
    return uv;
}

ObjClosure *new_closure(ObjFunction *function) {
    ObjClosure *cl = malloc(sizeof(ObjClosure));
    if (!cl) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)cl, OBJ_CLOSURE, sizeof(ObjClosure) + sizeof(ObjUpvalue*) * function->upvalue_count);
    cl->function = function;
    retain_obj((Object*)function);
    cl->upvalue_count = function->upvalue_count;
    cl->upvalues = cl->upvalue_count > 0
        ? calloc(cl->upvalue_count, sizeof(ObjUpvalue*))
        : NULL;
    return cl;
}

/* ============================================================ */
/* ARC memory management                                         */
/* ============================================================ */


/* Free container memory without releasing children.
 * Used by the GC sweep after children have already been released. */
void free_object_container(Object *obj) {
    if (!obj) return;

    if (obj->prev) {
        obj->prev->next = obj->next;
    } else if (all_objects == obj) {
        all_objects = obj->next;
    }
    if (obj->next) {
        obj->next->prev = obj->prev;
    }
    allocated_objects--;
    bytes_allocated -= obj->size;

    switch (obj->type) {
        case OBJ_STRING: { ObjString *s = (ObjString *)obj; intern_remove(s); free(s->chars); free(s); break; }
        case OBJ_LIST: {
            ObjList *l = (ObjList *)obj;
            if (l->items) free(l->items);
            free(l); break;
        }
        case OBJ_DICT: {
            ObjDict *d = (ObjDict *)obj;
            if (d->indices) { free(d->indices); free(d->entries); }
            free(d); break;
        }
        case OBJ_INSTANCE: {
            ObjInstance *inst = (ObjInstance *)obj;
            if (inst->class_name) free(inst->class_name);
            if (inst->base_class) free(inst->base_class);
            for (int i = 0; i < inst->field_count; i++) {
                if (inst->field_names[i]) free(inst->field_names[i]);
            }
            if (inst->methods) free(inst->methods);
            free(inst->field_names); free(inst->fields); free(inst); break;
        }
        case OBJ_FUNCTION: {
            ObjFunction *f = (ObjFunction *)obj;
            free(f->name);
            if (f->param_names) {
                for (int i = 0; i < f->param_count; i++) free(f->param_names[i]);
                free(f->param_names);
            }
            if (f->chunk) {
                chunk_free(f->chunk);
                free(f->chunk);
            }
            free(f->upvalue_descriptors);
            free(f); break;
        }
        case OBJ_EXCEPTION: {
            ObjException *e = (ObjException *)obj;
            free(e->message); free(e->file); free(e); break;
        }
        case OBJ_UPVALUE: {
            free((ObjUpvalue *)obj); break;
        }
        case OBJ_CLOSURE: {
            ObjClosure *cl = (ObjClosure *)obj;
            free(cl->upvalues);
            free(cl); break;
        }
        default: free(obj); break;
    }
}

/* ARC path: release children, then free container. */
void free_object(Object *obj) {
    if (!obj) return;

    switch (obj->type) {
        case OBJ_LIST: {
            ObjList *l = (ObjList *)obj;
            if (l->items) {
                for (int i = 0; i < l->count; i++) release_value(l->items[i]);
            } else {
                for (int i = 0; i < l->count; i++) release_value(l->inline_items[i]);
            }
            break;
        }
        case OBJ_DICT: {
            ObjDict *d = (ObjDict *)obj;
            if (d->indices == NULL) {
                for (int i = 0; i < d->entry_count; i++) {
                    release_value(d->inline_entries[i].key);
                    release_value(d->inline_entries[i].value);
                }
            } else {
                for (int i = 0; i < d->next_entry; i++) {
                    if (d->entries[i].key != EMPTY_VAL) {
                        release_value(d->entries[i].key);
                        release_value(d->entries[i].value);
                    }
                }
            }
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance *inst = (ObjInstance *)obj;
            for (int i = 0; i < inst->field_count; i++) {
                release_value(inst->fields[i]);
            }
            if (inst->methods) {
                for (int i = 0; i < inst->method_count; i++) {
                    if (inst->methods[i]) release_obj((Object*)inst->methods[i]);
                }
            }
            break;
        }
        case OBJ_UPVALUE: {
            ObjUpvalue *uv = (ObjUpvalue *)obj;
            release_value(uv->closed);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure *cl = (ObjClosure *)obj;
            for (int i = 0; i < cl->upvalue_count; i++) {
                if (cl->upvalues[i]) release_obj((Object*)cl->upvalues[i]);
            }
            release_obj((Object*)cl->function);
            break;
        }
        default: break;
    }

    free_object_container(obj);
}


/* ============================================================ */
/* Value predicates                                              */
/* ============================================================ */

bool is_null(Value v) { return IS_NIL(v); }

bool is_truthy(Value v) {
    if (IS_NIL(v)) return false;
    if (IS_BOOL(v)) return AS_BOOL(v);
    if (IS_INT(v)) return AS_INT(v) != 0;
    if (IS_DOUBLE(v)) return AS_DOUBLE(v) != 0.0;
    if (IS_OBJ(v)) return AS_OBJ(v) != NULL;
    return false;
}

bool values_equal(Value a, Value b) {
    if (a == b) return true;
    if ((IS_DOUBLE(a) || IS_INT(a)) && (IS_DOUBLE(b) || IS_INT(b))) {
        return as_double(a) == as_double(b);
    }
    return false;
}

char *value_to_string(Value v) {
    char buf[64];
    if (IS_NIL(v)) return strdup("null");
    if (IS_BOOL(v)) return strdup(AS_BOOL(v) ? "true" : "false");
    if (IS_INT(v)) { snprintf(buf, sizeof(buf), "%d", AS_INT(v)); return strdup(buf); }
    if (IS_DOUBLE(v)) { snprintf(buf, sizeof(buf), "%g", AS_DOUBLE(v)); return strdup(buf); }
    if (IS_OBJ(v)) {
        Object *obj = AS_OBJ(v);
        if (!obj) return strdup("null");
        switch (obj->type) {
            case OBJ_STRING:  return strdup(((ObjString *)obj)->chars);
            case OBJ_FUNCTION: {
                ObjFunction *f = (ObjFunction *)obj;
                snprintf(buf, sizeof(buf), "<%s %s>", f->is_native ? "native fn" : "fn", f->name ? f->name : "?");
                return strdup(buf);
            }
            case OBJ_INSTANCE:
                snprintf(buf, sizeof(buf), "<instance of %s>", ((ObjInstance*)obj)->class_name);
                return strdup(buf);
            case OBJ_EXCEPTION:
                return strdup(((ObjException *)obj)->message);
            case OBJ_LIST: {
                ObjList *l = (ObjList *)obj;
                int cap = 32; char *out = malloc(cap); int pos = 0;
                out[pos++] = '[';
                for (int i = 0; i < l->count; i++) {
                    Value item = l->items ? l->items[i] : l->inline_items[i];
                    char *e = value_to_string(item);
                    bool is_str = IS_OBJ(item) && AS_OBJ(item) &&
                                  AS_OBJ(item)->type == OBJ_STRING;
                    int need = pos + (int)strlen(e) + (is_str ? 2 : 0) + 4;
                    if (need >= cap) { cap = need * 2; out = realloc(out, cap); }
                    if (i > 0) { out[pos++] = ','; out[pos++] = ' '; }
                    if (is_str) out[pos++] = '"';
                    int el = (int)strlen(e); memcpy(out + pos, e, el); pos += el;
                    if (is_str) out[pos++] = '"';
                    free(e);
                }
                if (pos + 2 >= cap) { cap = pos + 4; out = realloc(out, cap); }
                out[pos++] = ']'; out[pos] = '\0';
                char *r = strdup(out); free(out); return r;
            }
            case OBJ_DICT: {
                ObjDict *d = (ObjDict *)obj;
                int cap = 32; char *out = malloc(cap); int pos = 0; bool first = true;
                out[pos++] = '{';
                if (d->indices == NULL) {
                    for (int i = 0; i < d->entry_count; i++) {
                        ObjDictEntry *e = &d->inline_entries[i];
                        char *k = value_to_string(e->key), *val = value_to_string(e->value);
                        bool ks = IS_OBJ(e->key) && AS_OBJ(e->key) && AS_OBJ(e->key)->type == OBJ_STRING;
                        bool vs = IS_OBJ(e->value) && AS_OBJ(e->value) && AS_OBJ(e->value)->type == OBJ_STRING;
                        int need = pos + (int)strlen(k) + (int)strlen(val) + (ks ? 2 : 0) + (vs ? 2 : 0) + 8;
                        if (need >= cap) { cap = need * 2; out = realloc(out, cap); }
                        if (!first) { out[pos++] = ','; out[pos++] = ' '; } first = false;
                        if (ks) out[pos++] = '"';
                        int kl = (int)strlen(k); memcpy(out + pos, k, kl); pos += kl;
                        if (ks) out[pos++] = '"';
                        out[pos++] = ':'; out[pos++] = ' ';
                        if (vs) out[pos++] = '"';
                        int vl = (int)strlen(val); memcpy(out + pos, val, vl); pos += vl;
                        if (vs) out[pos++] = '"';
                        free(k); free(val);
                    }
                } else {
                    for (int i = 0; i < d->next_entry; i++) {
                        if (d->entries[i].key == EMPTY_VAL) continue;
                        ObjDictEntry *e = &d->entries[i];
                        char *k = value_to_string(e->key), *val = value_to_string(e->value);
                        bool ks = IS_OBJ(e->key) && AS_OBJ(e->key) && AS_OBJ(e->key)->type == OBJ_STRING;
                        bool vs = IS_OBJ(e->value) && AS_OBJ(e->value) && AS_OBJ(e->value)->type == OBJ_STRING;
                        int need = pos + (int)strlen(k) + (int)strlen(val) + (ks ? 2 : 0) + (vs ? 2 : 0) + 8;
                        if (need >= cap) { cap = need * 2; out = realloc(out, cap); }
                        if (!first) { out[pos++] = ','; out[pos++] = ' '; } first = false;
                        if (ks) out[pos++] = '"';
                        int kl = (int)strlen(k); memcpy(out + pos, k, kl); pos += kl;
                        if (ks) out[pos++] = '"';
                        out[pos++] = ':'; out[pos++] = ' ';
                        if (vs) out[pos++] = '"';
                        int vl = (int)strlen(val); memcpy(out + pos, val, vl); pos += vl;
                        if (vs) out[pos++] = '"';
                        free(k); free(val);
                    }
                }
                if (pos + 2 >= cap) { cap = pos + 4; out = realloc(out, cap); }
                out[pos++] = '}'; out[pos] = '\0';
                char *r = strdup(out); free(out); return r;
            }
            default: return strdup("<object>");
        }
    }
    return strdup("<unknown>");
}

/* ============================================================ */
/* Instance field operations                                     */
/* ============================================================ */

Value instance_get_field(ObjInstance *inst, const char *name) {
    for (int i = 0; i < inst->field_count; i++)
        if (inst->field_names[i] && strcmp(inst->field_names[i], name) == 0)
            return inst->fields[i];
    return make_null();
}

void instance_set_field(ObjInstance *inst, const char *name, Value value) {
    for (int i = 0; i < inst->field_count; i++) {
        if (inst->field_names[i] && strcmp(inst->field_names[i], name) == 0) {
            release_value(inst->fields[i]);
            if (IS_OBJ(value) && AS_OBJ(value)) retain_obj(AS_OBJ(value));
            inst->fields[i] = value;
            return;
        }
    }
    if (inst->field_count >= inst->field_capacity) {
        inst->field_capacity = inst->field_capacity < 4 ? 4 : inst->field_capacity * 2;
        inst->field_names = realloc(inst->field_names, inst->field_capacity * sizeof(char *));
        inst->fields      = realloc(inst->fields,      inst->field_capacity * sizeof(Value));
    }
    if (IS_OBJ(value) && AS_OBJ(value)) retain_obj(AS_OBJ(value));
    inst->field_names[inst->field_count] = strdup(name);
    inst->fields     [inst->field_count] = value;
    inst->field_count++;
}

/* ============================================================ */
/* List operations                                               */
/* ============================================================ */

void list_add(ObjList *list, Value value) {
    if (list->count >= list->capacity) {
        int new_cap = list->capacity < 8 ? 8 : list->capacity * 2;
        if (list->items == NULL) {
            /* Transition from inline to heap */
            Value *new_items = malloc(sizeof(Value) * new_cap);
            if (!new_items) { fprintf(stderr, "OOM\n"); exit(1); }
            memcpy(new_items, list->inline_items, list->count * sizeof(Value));
            list->items = new_items;
        } else {
            list->items = realloc(list->items, new_cap * sizeof(Value));
            if (!list->items) { fprintf(stderr, "OOM\n"); exit(1); }
        }
        list->capacity = new_cap;
    }
    if (IS_OBJ(value) && AS_OBJ(value)) retain_obj(AS_OBJ(value));
    if (list->items) {
        list->items[list->count] = value;
    } else {
        list->inline_items[list->count] = value;
    }
    list->count++;
}

void list_insert(ObjList *list, int index, Value value) {
    if (index < 0 || index > list->count) { fprintf(stderr, "list.insert: out of bounds\n"); return; }
    if (list->count >= list->capacity) {
        int new_cap = list->capacity < 8 ? 8 : list->capacity * 2;
        if (list->items == NULL) {
            Value *new_items = malloc(sizeof(Value) * new_cap);
            if (!new_items) { fprintf(stderr, "OOM\n"); exit(1); }
            memcpy(new_items, list->inline_items, list->count * sizeof(Value));
            list->items = new_items;
        } else {
            list->items = realloc(list->items, new_cap * sizeof(Value));
            if (!list->items) { fprintf(stderr, "OOM\n"); exit(1); }
        }
        list->capacity = new_cap;
    }
    if (list->items) {
        memmove(&list->items[index + 1], &list->items[index], (list->count - index) * sizeof(Value));
        list->items[index] = value;
    } else {
        memmove(&list->inline_items[index + 1], &list->inline_items[index], (list->count - index) * sizeof(Value));
        list->inline_items[index] = value;
    }
    if (IS_OBJ(value) && AS_OBJ(value)) retain_obj(AS_OBJ(value));
    list->count++;
}

Value list_remove(ObjList *list, int index) {
    if (index < 0 || index >= list->count) { fprintf(stderr, "list.remove: out of bounds\n"); return make_null(); }
    Value v = list->items ? list->items[index] : list->inline_items[index];
    if (IS_OBJ(v) && AS_OBJ(v)) retain_obj(AS_OBJ(v));
    if (list->items) {
        memmove(&list->items[index], &list->items[index + 1], (list->count - index - 1) * sizeof(Value));
    } else {
        memmove(&list->inline_items[index], &list->inline_items[index + 1], (list->count - index - 1) * sizeof(Value));
    }
    list->count--;
    release_value(v);
    return v;
}

Value list_pop(ObjList *list) {
    if (!list->count) { fprintf(stderr, "list.pop: empty\n"); return make_null(); }
    return list_remove(list, list->count - 1);
}

void list_clear(ObjList *list) {
    if (list->items) {
        for (int i = 0; i < list->count; i++) release_value(list->items[i]);
    } else {
        for (int i = 0; i < list->count; i++) release_value(list->inline_items[i]);
    }
    list->count = 0;
}

Value list_get(ObjList *list, int index) {
    if (index < 0 || index >= list->count) { fprintf(stderr, "list: index out of bounds\n"); return make_null(); }
    return list->items ? list->items[index] : list->inline_items[index];
}

void list_set(ObjList *list, int index, Value value) {
    if (index < 0 || index >= list->count) { fprintf(stderr, "list: index out of bounds\n"); return; }
    if (list->items) {
        release_value(list->items[index]);
        list->items[index] = value;
    } else {
        release_value(list->inline_items[index]);
        list->inline_items[index] = value;
    }
    if (IS_OBJ(value) && AS_OBJ(value)) retain_obj(AS_OBJ(value));
}

int list_length(ObjList *list) { return list->count; }

/* ============================================================ */
/* Dict operations                                               */
/* ============================================================ */

static void dict_resize(ObjDict *d) {
    int new_cap = d->capacity == 0 ? 8 : d->capacity * 2;
    if (d->capacity > 8 && d->entry_count <= d->capacity / 2) {
        new_cap = d->capacity;
    }
    
    int *new_indices = malloc(new_cap * sizeof(int));
    for (int i = 0; i < new_cap; i++) new_indices[i] = -1;
    
    ObjDictEntry *new_entries = malloc(new_cap * sizeof(ObjDictEntry));
    
    int new_next = 0;
    for (int i = 0; i < d->next_entry; i++) {
        if (d->entries[i].key == EMPTY_VAL) continue;
        
        ObjDictEntry *e = &d->entries[i];
        int new_e_idx = new_next++;
        new_entries[new_e_idx] = *e;
        
        uint32_t perturb = e->hash;
        uint32_t mask = new_cap - 1;
        uint32_t idx = e->hash & mask;
        while (new_indices[idx] != -1) {
            idx = ((idx << 2) + idx + perturb + 1) & mask;
            perturb >>= 5;
        }
        new_indices[idx] = new_e_idx;
    }
    
    free(d->indices);
    free(d->entries);
    d->indices = new_indices;
    d->entries = new_entries;
    d->capacity = new_cap;
    d->next_entry = new_next;
    d->deleted_count = 0;
}

void dict_set(ObjDict *d, Value key, Value value) {
    /* SOO mode: linear search on inline_entries */
    if (d->indices == NULL) {
        for (int i = 0; i < d->entry_count; i++) {
            if (values_equal(d->inline_entries[i].key, key)) {
                release_value(d->inline_entries[i].value);
                d->inline_entries[i].value = value;
                if (IS_OBJ(value) && AS_OBJ(value)) retain_obj(AS_OBJ(value));
                return;
            }
        }
        if (d->entry_count < 4) {
            d->inline_entries[d->entry_count].hash = hash_value(key);
            d->inline_entries[d->entry_count].key = key;
            d->inline_entries[d->entry_count].value = value;
            if (IS_OBJ(key) && AS_OBJ(key)) retain_obj(AS_OBJ(key));
            if (IS_OBJ(value) && AS_OBJ(value)) retain_obj(AS_OBJ(value));
            d->entry_count++;
            return;
        }
        /* Full — transition to heap and fall through */
        dict_transition_to_heap(d);
    }

    if (d->capacity == 0 || d->next_entry + d->deleted_count >= d->capacity * 2 / 3) {
        dict_resize(d);
    }

    uint32_t hash = hash_value(key);
    uint32_t perturb = hash;
    uint32_t mask = d->capacity - 1;
    uint32_t i = hash & mask;
    int target_idx = -1;

    while (d->indices[i] != -1) {
        if (d->indices[i] == -2) {
            if (target_idx == -1) target_idx = i;
        } else {
            int e_idx = d->indices[i];
            if (d->entries[e_idx].hash == hash && values_equal(d->entries[e_idx].key, key)) {
                release_value(d->entries[e_idx].value);
                if (IS_OBJ(value) && AS_OBJ(value)) retain_obj(AS_OBJ(value));
                d->entries[e_idx].value = value;
                return;
            }
        }
        i = ((i << 2) + i + perturb + 1) & mask;
        perturb >>= 5;
    }

    if (target_idx == -1) target_idx = i;

    int e_idx = d->next_entry++;
    d->indices[target_idx] = e_idx;
    d->entries[e_idx].hash = hash;
    d->entries[e_idx].key = key;
    d->entries[e_idx].value = value;
    if (IS_OBJ(key) && AS_OBJ(key)) retain_obj(AS_OBJ(key));
    if (IS_OBJ(value) && AS_OBJ(value)) retain_obj(AS_OBJ(value));
    d->entry_count++;
}

Value dict_get(ObjDict *d, Value key) {
    if (d->indices == NULL) {
        for (int i = 0; i < d->entry_count; i++) {
            if (values_equal(d->inline_entries[i].key, key))
                return d->inline_entries[i].value;
        }
        return make_null();
    }
    if (d->capacity == 0) return make_null();
    uint32_t hash = hash_value(key);
    uint32_t perturb = hash;
    uint32_t mask = d->capacity - 1;
    uint32_t i = hash & mask;

    while (d->indices[i] != -1) {
        if (d->indices[i] >= 0) {
            int e_idx = d->indices[i];
            if (d->entries[e_idx].hash == hash && values_equal(d->entries[e_idx].key, key)) {
                return d->entries[e_idx].value;
            }
        }
        i = ((i << 2) + i + perturb + 1) & mask;
        perturb >>= 5;
    }
    return make_null();
}

bool dict_has(ObjDict *d, Value key) {
    if (d->indices == NULL) {
        for (int i = 0; i < d->entry_count; i++) {
            if (values_equal(d->inline_entries[i].key, key))
                return true;
        }
        return false;
    }
    if (d->capacity == 0) return false;
    uint32_t hash = hash_value(key);
    uint32_t perturb = hash;
    uint32_t mask = d->capacity - 1;
    uint32_t i = hash & mask;

    while (d->indices[i] != -1) {
        if (d->indices[i] >= 0) {
            int e_idx = d->indices[i];
            if (d->entries[e_idx].hash == hash && values_equal(d->entries[e_idx].key, key)) {
                return true;
            }
        }
        i = ((i << 2) + i + perturb + 1) & mask;
        perturb >>= 5;
    }
    return false;
}

Value dict_remove(ObjDict *d, Value key) {
    if (d->indices == NULL) {
        for (int i = 0; i < d->entry_count; i++) {
            if (values_equal(d->inline_entries[i].key, key)) {
                Value v = d->inline_entries[i].value;
                if (IS_OBJ(v) && AS_OBJ(v)) retain_obj(AS_OBJ(v));
                release_value(d->inline_entries[i].key);
                release_value(d->inline_entries[i].value);
                memmove(&d->inline_entries[i], &d->inline_entries[i + 1],
                        (d->entry_count - i - 1) * sizeof(ObjDictEntry));
                d->entry_count--;
                return v;
            }
        }
        return make_null();
    }
    if (d->capacity == 0) return make_null();
    uint32_t hash = hash_value(key);
    uint32_t perturb = hash;
    uint32_t mask = d->capacity - 1;
    uint32_t i = hash & mask;

    while (d->indices[i] != -1) {
        if (d->indices[i] >= 0) {
            int e_idx = d->indices[i];
            if (d->entries[e_idx].hash == hash && values_equal(d->entries[e_idx].key, key)) {
                d->indices[i] = -2;
                Value v = d->entries[e_idx].value;
                if (IS_OBJ(v) && AS_OBJ(v)) retain_obj(AS_OBJ(v));
                release_value(d->entries[e_idx].key);
                release_value(d->entries[e_idx].value);
                d->entries[e_idx].key = EMPTY_VAL;
                d->entries[e_idx].value = make_null();
                d->entry_count--;
                d->deleted_count++;
                return v;
            }
        }
        i = ((i << 2) + i + perturb + 1) & mask;
        perturb >>= 5;
    }
    return make_null();
}

void dict_clear(ObjDict *d) {
    if (d->indices == NULL) {
        for (int i = 0; i < d->entry_count; i++) {
            release_value(d->inline_entries[i].key);
            release_value(d->inline_entries[i].value);
        }
        d->entry_count = 0;
        return;
    }
    for (int i = 0; i < d->next_entry; i++) {
        if (d->entries[i].key != EMPTY_VAL) {
            release_value(d->entries[i].key);
            release_value(d->entries[i].value);
        }
    }
    for (int i = 0; i < d->capacity; i++) d->indices[i] = -1;
    d->entry_count = 0;
    d->next_entry = 0;
    d->deleted_count = 0;
}

int dict_length(ObjDict *d) { return d->entry_count; }

Value dict_keys(ObjDict *d) {
    ObjList *list = new_list(d->entry_count);
    if (d->indices == NULL) {
        for (int i = 0; i < d->entry_count; i++)
            list_add(list, d->inline_entries[i].key);
    } else {
        for (int i = 0; i < d->next_entry; i++) {
            if (d->entries[i].key != EMPTY_VAL)
                list_add(list, d->entries[i].key);
        }
    }
    return make_obj((Object *)list);
}

Value dict_values(ObjDict *d) {
    ObjList *list = new_list(d->entry_count);
    if (d->indices == NULL) {
        for (int i = 0; i < d->entry_count; i++)
            list_add(list, d->inline_entries[i].value);
    } else {
        for (int i = 0; i < d->next_entry; i++) {
            if (d->entries[i].key != EMPTY_VAL)
                list_add(list, d->entries[i].value);
        }
    }
    return make_obj((Object *)list);
}

/* ============================================================ */
/* Error helpers                                                 */
/* ============================================================ */

void runtime_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}
