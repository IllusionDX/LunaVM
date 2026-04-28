/* value.c — Luna VM value and object implementation (NaN-boxed). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <math.h>
#include "value.h"
#include "chunk.h"
#include "vm.h"

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
    if (IS_INT64(v)) {
        uint64_t x = ((ObjInt64*)AS_OBJ(v))->value;
        return (uint32_t)(x ^ (x >> 32));
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
        if (obj->type == OBJ_USERDATA)
            return (uint32_t)(uintptr_t)((ObjUserdata*)obj)->data;
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
Object *userdata_objects = NULL;

static void init_object(Object *obj, ObjType type, size_t size) {
    obj->type = type;
    obj->refcount = 0;
    obj->is_marked = false;
    obj->size = size;
    bytes_allocated += size;
    obj->next = all_objects;
    obj->prev = NULL;
    obj->finalizer_next = NULL;
    obj->finalizer_prev = NULL;
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

    /* Do NOT release the old inline references — the dict still owns them,
     * they've just moved from the inline array to the heap array. */
}

ObjClass *new_class(const char *name, const char *base_name) {
    (void)base_name;
    ObjClass *cls = malloc(sizeof(ObjClass));
    if (!cls) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)cls, OBJ_CLASS, sizeof(ObjClass));
    cls->name = strdup(name);
    cls->base = NULL;
    cls->prototype = NULL;
    cls->methods = NULL;
    cls->method_names = NULL;
    cls->method_count = 0;
    cls->method_capacity = 0;
    cls->fields = new_dict();
    retain_obj((Object*)cls->fields);
    return cls;
}

ObjInstance *new_instance(ObjClass *klass, int cap) {
    ObjInstance *inst = malloc(sizeof(ObjInstance));
    if (!inst) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)inst, OBJ_INSTANCE, sizeof(ObjInstance) + sizeof(Value) * cap);
    inst->class_name    = strdup(klass->name);
    inst->klass         = klass;
    retain_obj((Object*)klass);
    inst->field_capacity = cap > 0 ? cap : 4;
    inst->field_count   = 0;
    inst->field_names   = calloc(inst->field_capacity, sizeof(char *));
    inst->fields        = calloc(inst->field_capacity, sizeof(Value));

    /* Copy prototype fields if they exist */
    if (klass->prototype) {
        ObjInstance *proto = klass->prototype;
        for (int i = 0; i < proto->field_count; i++) {
            if (inst->field_count >= inst->field_capacity) {
                inst->field_capacity = inst->field_capacity * 2;
                inst->field_names = realloc(inst->field_names, inst->field_capacity * sizeof(char *));
                inst->fields      = realloc(inst->fields,      inst->field_capacity * sizeof(Value));
            }
            inst->field_names[inst->field_count] = strdup(proto->field_names[i]);
            inst->fields[inst->field_count] = proto->fields[i];
            if (IS_OBJ(proto->fields[i]) && AS_OBJ(proto->fields[i]))
                retain_obj(AS_OBJ(proto->fields[i]));
            inst->field_count++;
        }
    }
    return inst;
}

ObjFunction *new_function(const char *name) {
    ObjFunction *f = malloc(sizeof(ObjFunction));
    if (!f) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)f, OBJ_FUNCTION, sizeof(ObjFunction));
    f->name        = strdup(name ? name : "<fn>");
    f->chunk       = NULL;
    f->param_names = NULL;
    f->param_name_objs = NULL;
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

ObjUserdata *new_userdata_tagged(const char *tag, void *data, UserdataFinalizer finalizer) {
    ObjUserdata *ud = malloc(sizeof(ObjUserdata));
    if (!ud) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)ud, OBJ_USERDATA, sizeof(ObjUserdata));
    ud->tag = strdup(tag ? tag : "userdata");
    ud->data = data;
    ud->finalizer = finalizer;
    ud->finalized = false;
    ud->obj.finalizer_next = userdata_objects;
    if (userdata_objects) userdata_objects->finalizer_prev = (Object*)ud;
    userdata_objects = (Object*)ud;
    return ud;
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

ObjBoundMethod *new_bound_method(Value self, ObjFunction *fn) {
    ObjBoundMethod *bm = malloc(sizeof(ObjBoundMethod));
    if (!bm) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)bm, OBJ_BOUND_METHOD, sizeof(ObjBoundMethod));
    bm->self = self;
    if (IS_OBJ(self) && AS_OBJ(self)) retain_obj(AS_OBJ(self));
    bm->fn = fn;
    if (fn) retain_obj((Object*)fn);
    return bm;
}

ObjEnum *new_enum(const char *name, int count) {
    ObjEnum *e = malloc(sizeof(ObjEnum));
    if (!e) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)e, OBJ_ENUM, sizeof(ObjEnum));
    e->name   = strdup(name);
    e->count  = count;
    e->names  = count > 0 ? calloc(count, sizeof(char*)) : NULL;
    e->values = count > 0 ? calloc(count, sizeof(int64_t)) : NULL;
    return e;
}

ObjModule *new_module(const char *name) {
    ObjModule *mod = malloc(sizeof(ObjModule));
    if (!mod) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)mod, OBJ_MODULE, sizeof(ObjModule));
    mod->name = new_string(name, (int)strlen(name));
    retain_obj((Object*)mod->name);   /* ObjModule owns its name string */
    mod->exports = new_dict();
    retain_obj((Object*)mod->exports); /* ObjModule owns its exports dict */
    return mod;
}

ObjBuffer *new_buffer(size_t capacity) {
    ObjBuffer *buf = malloc(sizeof(ObjBuffer));
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)buf, OBJ_BUFFER, sizeof(ObjBuffer) + capacity);
    buf->size = 0;
    buf->capacity = capacity;
    buf->cursor = 0;
    buf->data = capacity ? malloc(capacity) : NULL;
    if (capacity && !buf->data) { fprintf(stderr, "OOM\n"); exit(1); }
    return buf;
}

ObjInt64 *new_int64(int64_t value) {
    ObjInt64 *obj = malloc(sizeof(ObjInt64));
    if (!obj) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)obj, OBJ_INT64, sizeof(ObjInt64));
    obj->value = (uint64_t)value;
    return obj;
}

ObjVector *new_vector(float x, float y, float z, float w) {
    ObjVector *obj = malloc(sizeof(ObjVector));
    if (!obj) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)obj, OBJ_VECTOR, sizeof(ObjVector));
    obj->data[0] = x;
    obj->data[1] = y;
    obj->data[2] = z;
    obj->data[3] = w;
    return obj;
}

ObjMatrix *new_matrix(void) {
    ObjMatrix *obj = malloc(sizeof(ObjMatrix));
    if (!obj) { fprintf(stderr, "OOM\n"); exit(1); }
    init_object((Object*)obj, OBJ_MATRIX, sizeof(ObjMatrix));
    for (int i = 0; i < 16; i++) obj->m[i] = 0.0f;
    obj->m[0] = obj->m[5] = obj->m[10] = obj->m[15] = 1.0f;
    return obj;
}

void buffer_reserve(ObjBuffer *buf, size_t capacity) {
    if (!buf) return;
    if (capacity <= buf->capacity) return;
    size_t new_cap = buf->capacity ? buf->capacity : 16;
    while (new_cap < capacity) {
        new_cap *= 2;
    }
    uint8_t *new_data = realloc(buf->data, new_cap);
    if (!new_data) { fprintf(stderr, "OOM\n"); exit(1); }
    if (new_cap > buf->capacity) {
        size_t delta = new_cap - buf->capacity;
        buf->obj.size += delta;
        bytes_allocated += delta;
    }
    buf->data = new_data;
    buf->capacity = new_cap;
}

void buffer_resize(ObjBuffer *buf, size_t size) {
    if (!buf) return;
    buffer_reserve(buf, size);
    buf->size = size;
    if (buf->cursor > buf->size) buf->cursor = buf->size;
}

void buffer_append_byte(ObjBuffer *buf, uint8_t byte) {
    if (!buf) return;
    buffer_reserve(buf, buf->size + 1);
    buf->data[buf->size++] = byte;
}

void buffer_append_data(ObjBuffer *buf, const uint8_t *data, size_t len) {
    if (!buf || !data || !len) return;
    buffer_reserve(buf, buf->size + len);
    memcpy(buf->data + buf->size, data, len);
    buf->size += len;
}

Value make_exception_instance(struct VM *vm, ObjClass *cls, const char *message) {
    if (!cls) cls = vm->exception_class;
    if (!cls) return make_null();
    ObjInstance *inst = new_instance(cls, 4);
    instance_set_field(inst, "message", make_obj((Object*)new_string(message, strlen(message))));
    return make_obj((Object*)inst);
}

/* ============================================================ */
/* ARC memory management                                         */
/* ============================================================ */

static void userdata_run_finalizer(ObjUserdata *ud) {
    if (!ud || ud->finalized) return;
    ud->finalized = true;
    if (ud->finalizer && ud->data) {
        ud->finalizer(ud->data);
    }
    ud->data = NULL;
}


/* Free container memory without releasing children.
 * Used by the GC sweep after children have already been released. */
void free_object_container(Object *obj) {
    /* WARNING: This function MUST NOT call release_obj on child objects.
     * Children are released by free_object() (ARC path) or by the GC sweep
     * phase 1 in mark_and_sweep(). This function only frees raw memory. */
    if (!obj) return;

    if (obj->prev) {
        obj->prev->next = obj->next;
    } else if (all_objects == obj) {
        all_objects = obj->next;
    }
    if (obj->next) {
        obj->next->prev = obj->prev;
    }
    if (obj->finalizer_prev) {
        obj->finalizer_prev->finalizer_next = obj->finalizer_next;
    } else if (userdata_objects == obj) {
        userdata_objects = obj->finalizer_next;
    }
    if (obj->finalizer_next) {
        obj->finalizer_next->finalizer_prev = obj->finalizer_prev;
    }
    obj->finalizer_next = NULL;
    obj->finalizer_prev = NULL;
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
            for (int i = 0; i < inst->field_count; i++) {
                if (inst->field_names[i]) free(inst->field_names[i]);
            }
            free(inst->field_names); free(inst->fields); free(inst); break;
        }
        case OBJ_FUNCTION: {
            ObjFunction *f = (ObjFunction *)obj;
            free(f->name);
            if (f->param_names) {
                for (int i = 0; i < f->param_count; i++) free(f->param_names[i]);
                free(f->param_names);
            }
            free(f->param_name_objs);
            if (f->chunk) {
                chunk_free(f->chunk);
                free(f->chunk);
            }
            free(f->upvalue_descriptors);
            free(f); break;
        }
        case OBJ_UPVALUE: {
            free((ObjUpvalue *)obj); break;
        }
        case OBJ_CLOSURE: {
            ObjClosure *cl = (ObjClosure *)obj;
            free(cl->upvalues);
            free(cl); break;
        }
        case OBJ_ENUM: {
            ObjEnum *e = (ObjEnum *)obj;
            free(e->name);
            for (int i = 0; i < e->count; i++) free(e->names[i]);
            free(e->names); free(e->values); free(e); break;
        }
        case OBJ_CLASS: {
            ObjClass *cls = (ObjClass *)obj;
            free(cls->name);
            if (cls->method_names) {
                for (int i = 0; i < cls->method_count; i++)
                    if (cls->method_names[i]) free(cls->method_names[i]);
                free(cls->method_names);
            }
            free(cls->methods);
            free(cls); break;
        }
        case OBJ_BOUND_METHOD: {
            free(obj); break;
        }
        case OBJ_MODULE: {
            free(obj); break;
        }
        case OBJ_BUFFER: {
            ObjBuffer *buf = (ObjBuffer*)obj;
            free(buf->data);
            free(buf);
            break;
        }
        case OBJ_INT64: {
            free(obj);
            break;
        }
        case OBJ_USERDATA: {
            ObjUserdata *ud = (ObjUserdata*)obj;
            userdata_run_finalizer(ud);
            free(ud->tag);
            free(ud);
            break;
        }
        case OBJ_VECTOR: {
            free(obj);
            break;
        }
        case OBJ_MATRIX: {
            free(obj);
            break;
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
            if (inst->klass) release_obj((Object*)inst->klass);
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
        case OBJ_ENUM: break; /* no child Values to release */
        case OBJ_CLASS: {
            ObjClass *cls = (ObjClass *)obj;
            if (cls->prototype) release_obj((Object*)cls->prototype);
            if (cls->base) release_obj((Object*)cls->base);
            for (int i = 0; i < cls->method_count; i++) {
                if (cls->methods[i]) release_obj((Object*)cls->methods[i]);
            }
            if (cls->fields) release_obj((Object*)cls->fields);
            cls->fields = NULL;
            break;
        }
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod *bm = (ObjBoundMethod *)obj;
            release_value(bm->self);
            if (bm->fn) release_obj((Object*)bm->fn);
            break;
        }
        case OBJ_MODULE: {
            ObjModule *mod = (ObjModule *)obj;
            if (mod->name) release_obj((Object*)mod->name);
            if (mod->exports) release_obj((Object*)mod->exports);
            break;
        }
        case OBJ_BUFFER: break;
        case OBJ_INT64: break;
        case OBJ_USERDATA: break;
        case OBJ_VECTOR: break;
        case OBJ_MATRIX: break;
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
    if (IS_INT64(v)) return ((ObjInt64*)AS_OBJ(v))->value != 0;
    if (IS_DOUBLE(v)) return AS_DOUBLE(v) != 0.0;
    if (IS_OBJ(v)) return AS_OBJ(v) != NULL;
    return false;
}

bool values_equal(Value a, Value b) {
    if (a == b) return true;
    if ((IS_DOUBLE(a) || IS_INT(a) || IS_INT64(a)) && (IS_DOUBLE(b) || IS_INT(b) || IS_INT64(b))) {
        return as_double(a) == as_double(b);
    }
    return false;
}

char *value_to_string(Value v) {
    char buf[64];
    if (IS_NIL(v)) return strdup("null");
    if (IS_BOOL(v)) return strdup(AS_BOOL(v) ? "true" : "false");
    if (IS_INT(v)) { snprintf(buf, sizeof(buf), "%d", AS_INT(v)); return strdup(buf); }
    if (IS_INT64(v)) { snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)((ObjInt64*)AS_OBJ(v))->value); return strdup(buf); }
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
            case OBJ_INSTANCE: {
                ObjInstance *inst = (ObjInstance*)obj;
                Value msgv = instance_get_field(inst, "message");
                if (IS_STRING(msgv)) {
                    snprintf(buf, sizeof(buf), "<%s>", ((ObjString*)AS_OBJ(msgv))->chars);
                    return strdup(buf);
                } else {
                    snprintf(buf, sizeof(buf), "<instance of %s>", inst->class_name);
                    return strdup(buf);
                }
            }
            case OBJ_LIST: {
                ObjList *l = (ObjList *)obj;
                int cap = 32; char *out = malloc(cap); int pos = 0;
                out[pos++] = '[';
                for (int i = 0; i < l->count; i++) {
                    Value item = l->items ? l->items[i] : l->inline_items[i];
                    char *e = value_to_string(item);
                    bool is_str = IS_STRING(item);
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
                        bool ks = IS_STRING(e->key);
                        bool vs = IS_STRING(e->value);
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
                        bool ks = IS_STRING(e->key);
                        bool vs = IS_STRING(e->value);
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
            case OBJ_ENUM: {
                ObjEnum *e = (ObjEnum*)obj;
                int cap = 64; char *out = malloc(cap);
                int n = snprintf(out, cap, "<enum %s (%d variants)>", e->name, e->count);
                if (n >= cap) { cap = n + 1; out = realloc(out, cap); snprintf(out, cap, "<enum %s (%d variants)>", e->name, e->count); }
                char *r = strdup(out); free(out); return r;
            }
            case OBJ_BOUND_METHOD: {
                ObjBoundMethod *bm = (ObjBoundMethod*)obj;
                ObjFunction *f = bm->fn;
                snprintf(buf, sizeof(buf), "<bound method %s>", f && f->name ? f->name : "?");
                return strdup(buf);
            }
            case OBJ_MODULE: {
                ObjModule *mod = (ObjModule*)obj;
                snprintf(buf, sizeof(buf), "<module %s>", mod->name ? mod->name->chars : "?");
                return strdup(buf);
            }
            case OBJ_BUFFER: {
                ObjBuffer *bufv = (ObjBuffer*)obj;
                snprintf(buf, sizeof(buf), "<buffer %zu bytes>", bufv->size);
                return strdup(buf);
            }
            case OBJ_INT64: {
                ObjInt64 *i64 = (ObjInt64*)obj;
                snprintf(buf, sizeof(buf), "<int64 %" PRId64 ">", (int64_t)i64->value);
                return strdup(buf);
            }
            case OBJ_USERDATA: {
                ObjUserdata *ud = (ObjUserdata*)obj;
                snprintf(buf, sizeof(buf), "<userdata %s>", ud->tag ? ud->tag : "?");
                return strdup(buf);
            }
            case OBJ_VECTOR: {
                ObjVector *vec = (ObjVector*)obj;
                snprintf(buf, sizeof(buf), "vec4(%.6g, %.6g, %.6g, %.6g)",
                         vec->data[0], vec->data[1], vec->data[2], vec->data[3]);
                return strdup(buf);
            }
            case OBJ_MATRIX: {
                return strdup("<mat4>");
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

Value buffer_read_byte(const ObjBuffer *buf, size_t offset) {
    if (!buf || !buf->data || offset + 1 > buf->size) return make_null();
    return make_int((int32_t)buf->data[offset]);
}

Value buffer_read_short(const ObjBuffer *buf, size_t offset) {
    if (!buf || !buf->data || offset + 2 > buf->size) return make_null();
    uint16_t raw = (uint16_t)buf->data[offset]
                 | ((uint16_t)buf->data[offset + 1] << 8);
    return make_int((int32_t)raw);
}

Value buffer_read_int(const ObjBuffer *buf, size_t offset) {
    if (!buf || !buf->data || offset + 4 > buf->size) return make_null();
    uint32_t raw = (uint32_t)buf->data[offset]
                 | ((uint32_t)buf->data[offset + 1] << 8)
                 | ((uint32_t)buf->data[offset + 2] << 16)
                 | ((uint32_t)buf->data[offset + 3] << 24);
    return make_int((int32_t)raw);
}

Value buffer_read_long(const ObjBuffer *buf, size_t offset) {
    if (!buf || !buf->data || offset + 8 > buf->size) return make_null();
    uint64_t raw = 0;
    for (size_t i = 0; i < 8; i++) {
        raw |= ((uint64_t)buf->data[offset + i]) << (i * 8);
    }
    return make_int64((int64_t)raw);
}

/* ============================================================ */
/* List operations                                               */
/* ============================================================ */

void list_add(ObjList *list, Value value) {
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
    if (index < 0 || index >= list->count) return make_null();
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

bool list_contains(ObjList *list, Value value) {
    int len = list_length(list);
    for (int i = 0; i < len; i++) {
        if (values_equal(list_get(list, i), value)) return true;
    }
    return false;
}

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
