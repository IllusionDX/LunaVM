/* value.c — Luna VM value and object implementation. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "value.h"

/* ============================================================ */
/* Value constructors                                            */
/* ============================================================ */

Value make_null(void)              { Value v; v.type = VAL_NULL; v.as.integer = 0; return v; }
Value make_bool(bool b)            { Value v; v.type = VAL_BOOL;   v.as.boolean   = b;     return v; }
Value make_int(int64_t i)          { Value v; v.type = VAL_INT;    v.as.integer   = i;     return v; }
Value make_uint(uint64_t u)        { Value v; v.type = VAL_UINT;   v.as.uint_val  = u;     return v; }
Value make_float(float f)          { Value v; v.type = VAL_FLOAT;  v.as.float_val = f;     return v; }
Value make_double(double d)        { Value v; v.type = VAL_DOUBLE; v.as.double_val= d;     return v; }
Value make_nan(void)               { Value v; v.type = VAL_NAN;    v.as.integer   = 0;     return v; }
Value make_char(uint32_t cp)       { Value v; v.type = VAL_CHAR;   v.as.char_val  = cp;    return v; }
Value make_obj(Object *obj)        { Value v; v.type = VAL_OBJ;    v.as.obj       = obj;   return v; }

/* ============================================================ */
/* Hashing                                                       */
/* ============================================================ */

static uint32_t fnv1a(const char *key, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= (uint8_t)key[i]; h *= 16777619u; }
    return h;
}

uint32_t hash_value(Value v) {
    switch (v.type) {
        case VAL_NULL:   return 0;
        case VAL_BOOL:   return v.as.boolean ? 1u : 2u;
        case VAL_INT:    return (uint32_t)(v.as.integer ^ (v.as.integer >> 32));
        case VAL_UINT:   return (uint32_t)(v.as.uint_val ^ (v.as.uint_val >> 32));
        case VAL_FLOAT:  { uint32_t u; memcpy(&u, &v.as.float_val,  4); return u; }
        case VAL_DOUBLE: { uint64_t u; memcpy(&u, &v.as.double_val, 8); return (uint32_t)(u^(u>>32)); }
        case VAL_NAN:    return 3u;
        case VAL_CHAR:   return v.as.char_val * 2654435761u;
        case VAL_OBJ:
            if (!v.as.obj) return 0;
            if (v.as.obj->type == OBJ_STRING)
                return ((ObjString *)v.as.obj)->hash;
            return (uint32_t)(uintptr_t)v.as.obj;
        default: return 0;
    }
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

ObjString *new_string(const char *chars, int length) {
    uint32_t hash = fnv1a(chars, length);
    ObjString *existing = intern_find(chars, length, hash);
    if (existing) {
        retain_obj((Object*)existing);
        return existing;
    }

    ObjString *s = malloc(sizeof(ObjString));
    if (!s) { fprintf(stderr, "OOM\n"); exit(1); }
    s->obj.type = OBJ_STRING; s->obj.refcount = 1; s->obj.next = NULL;
    s->chars = malloc(length + 1);
    if (!s->chars) { fprintf(stderr, "OOM\n"); exit(1); }
    memcpy(s->chars, chars, length);
    s->chars[length] = '\0';
    s->length = length;
    s->hash   = hash;
    intern_add(s);
    return s;
}

ObjList *new_list(void) {
    ObjList *l = malloc(sizeof(ObjList));
    if (!l) { fprintf(stderr, "OOM\n"); exit(1); }
    l->obj.type = OBJ_LIST; l->obj.refcount = 1; l->obj.next = NULL;
    l->items = NULL; l->count = 0; l->capacity = 0;
    return l;
}

ObjDict *new_dict(void) {
    ObjDict *d = malloc(sizeof(ObjDict));
    if (!d) { fprintf(stderr, "OOM\n"); exit(1); }
    d->obj.type = OBJ_DICT; d->obj.refcount = 1; d->obj.next = NULL;
    d->bucket_count = 16;
    d->entry_count  = 0;
    d->buckets = calloc(d->bucket_count, sizeof(DictNode *));
    if (!d->buckets) { fprintf(stderr, "OOM\n"); exit(1); }
    return d;
}

ObjInstance *new_instance(const char *class_name, const char *base_class, int cap) {
    ObjInstance *inst = malloc(sizeof(ObjInstance));
    if (!inst) { fprintf(stderr, "OOM\n"); exit(1); }
    inst->obj.type = OBJ_INSTANCE; inst->obj.refcount = 1; inst->obj.next = NULL;
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
    f->obj.type = OBJ_FUNCTION; f->obj.refcount = 1; f->obj.next = NULL;
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
    e->obj.type = OBJ_EXCEPTION; e->obj.refcount = 1; e->obj.next = NULL;
    e->message = strdup(message);
    e->line    = 0;
    e->file    = strdup("<unknown>");
    return e;
}

ObjUpvalue *new_upvalue(Value *slot) {
    ObjUpvalue *uv = malloc(sizeof(ObjUpvalue));
    if (!uv) { fprintf(stderr, "OOM\n"); exit(1); }
    uv->obj.type = OBJ_UPVALUE; uv->obj.refcount = 1; uv->obj.next = NULL;
    uv->location = slot;
    uv->closed   = make_null();
    uv->next     = NULL;
    uv->frame_depth = 0;
    return uv;
}

ObjClosure *new_closure(ObjFunction *function) {
    ObjClosure *cl = malloc(sizeof(ObjClosure));
    if (!cl) { fprintf(stderr, "OOM\n"); exit(1); }
    cl->obj.type = OBJ_CLOSURE; cl->obj.refcount = 1; cl->obj.next = NULL;
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

void retain_obj(Object *obj) {
    if (obj) obj->refcount++;
}

static void release_value_inline(Value v) {
    if (v.type == VAL_OBJ && v.as.obj) release_obj(v.as.obj);
}

static void free_dict_internal(ObjDict *d) {
    for (int i = 0; i < d->bucket_count; i++) {
        DictNode *e = d->buckets[i];
        while (e) { DictNode *nx = e->next; release_value_inline(e->key); release_value_inline(e->value); free(e); e = nx; }
    }
    free(d->buckets); free(d);
}

void free_object(Object *obj) {
    if (!obj) return;
    switch (obj->type) {
        case OBJ_STRING: { ObjString *s = (ObjString *)obj; intern_remove(s); free(s->chars); free(s); break; }
        case OBJ_LIST: {
            ObjList *l = (ObjList *)obj;
            for (int i = 0; i < l->count; i++) release_value_inline(l->items[i]);
            free(l->items); free(l); break;
        }
        case OBJ_DICT:     free_dict_internal((ObjDict *)obj); break;
        case OBJ_INSTANCE: {
            ObjInstance *inst = (ObjInstance *)obj;
            free(inst->class_name);
            if (inst->base_class) free(inst->base_class);
            for (int i = 0; i < inst->field_count; i++) {
                release_value_inline(inst->fields[i]);
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
            free(f->upvalue_descriptors);
            /* chunk is owned by the compiler/program, not the function object */
            free(f); break;
        }
        case OBJ_EXCEPTION: {
            ObjException *e = (ObjException *)obj;
            free(e->message); free(e->file); free(e); break;
        }
        case OBJ_UPVALUE: {
            ObjUpvalue *uv = (ObjUpvalue *)obj;
            release_value_inline(uv->closed);
            free(uv); break;
        }
        case OBJ_CLOSURE: {
            ObjClosure *cl = (ObjClosure *)obj;
            for (int i = 0; i < cl->upvalue_count; i++) {
                if (cl->upvalues[i]) release_obj((Object*)cl->upvalues[i]);
            }
            free(cl->upvalues);
            release_obj((Object*)cl->function);
            free(cl); break;
        }
        default: free(obj); break;
    }
}

void release_obj(Object *obj) {
    if (!obj) return;
    obj->refcount--;
    if (obj->refcount <= 0) free_object(obj);
}

/* ============================================================ */
/* Value predicates                                              */
/* ============================================================ */

bool is_null(Value v) { return v.type == VAL_NULL; }

bool is_truthy(Value v) {
    switch (v.type) {
        case VAL_NULL:   return false;
        case VAL_BOOL:   return v.as.boolean;
        case VAL_INT:    return v.as.integer != 0;
        case VAL_UINT:   return v.as.uint_val != 0;
        case VAL_FLOAT:  return v.as.float_val != 0.0f;
        case VAL_DOUBLE: return v.as.double_val != 0.0;
        case VAL_NAN:    return false;
        case VAL_CHAR:   return v.as.char_val != 0;
        case VAL_OBJ:    return v.as.obj != NULL;
        default:         return false;
    }
}

bool values_equal(Value a, Value b) {
    if (a.type != b.type) {
        /* cross-type numeric equality */
        if ((a.type == VAL_INT || a.type == VAL_UINT || a.type == VAL_FLOAT || a.type == VAL_DOUBLE) &&
            (b.type == VAL_INT || b.type == VAL_UINT || b.type == VAL_FLOAT || b.type == VAL_DOUBLE)) {
            double da = (a.type==VAL_INT)?    (double)a.as.integer :
                        (a.type==VAL_UINT)?   (double)a.as.uint_val :
                        (a.type==VAL_FLOAT)?  (double)a.as.float_val : a.as.double_val;
            double db = (b.type==VAL_INT)?    (double)b.as.integer :
                        (b.type==VAL_UINT)?   (double)b.as.uint_val :
                        (b.type==VAL_FLOAT)?  (double)b.as.float_val : b.as.double_val;
            return da == db;
        }
        return false;
    }
    switch (a.type) {
        case VAL_NULL:   return true;
        case VAL_BOOL:   return a.as.boolean   == b.as.boolean;
        case VAL_INT:    return a.as.integer   == b.as.integer;
        case VAL_UINT:   return a.as.uint_val  == b.as.uint_val;
        case VAL_FLOAT:  return a.as.float_val == b.as.float_val;
        case VAL_DOUBLE: return a.as.double_val== b.as.double_val;
        case VAL_NAN:    return false;  /* NaN != NaN */
        case VAL_CHAR:   return a.as.char_val  == b.as.char_val;
        case VAL_OBJ:
            if (!a.as.obj || !b.as.obj) return a.as.obj == b.as.obj;
            if (a.as.obj->type == OBJ_STRING && b.as.obj->type == OBJ_STRING) {
                /* Interned strings: same content == same pointer */
                return a.as.obj == b.as.obj;
            }
            return a.as.obj == b.as.obj;
        default: return false;
    }
}

char *value_to_string(Value v) {
    char buf[64];
    switch (v.type) {
        case VAL_NULL:   return strdup("null");
        case VAL_BOOL:   return strdup(v.as.boolean ? "true" : "false");
        case VAL_INT:    snprintf(buf,sizeof(buf),"%lld",(long long)v.as.integer);  return strdup(buf);
        case VAL_UINT:   snprintf(buf,sizeof(buf),"%llu",(unsigned long long)v.as.uint_val); return strdup(buf);
        case VAL_FLOAT:  snprintf(buf,sizeof(buf),"%g",  v.as.float_val);  return strdup(buf);
        case VAL_DOUBLE: snprintf(buf,sizeof(buf),"%g",  v.as.double_val); return strdup(buf);
        case VAL_NAN:    return strdup("NaN");
        case VAL_CHAR: {
            /* encode Unicode codepoint to UTF-8 */
            uint32_t cp = v.as.char_val;
            char tmp[5] = {0};
            if      (cp < 0x80)   { tmp[0]=(char)cp; }
            else if (cp < 0x800)  { tmp[0]=0xC0|(cp>>6); tmp[1]=0x80|(cp&0x3F); }
            else if (cp < 0x10000){ tmp[0]=0xE0|(cp>>12); tmp[1]=0x80|((cp>>6)&0x3F); tmp[2]=0x80|(cp&0x3F); }
            else { tmp[0]=0xF0|(cp>>18); tmp[1]=0x80|((cp>>12)&0x3F); tmp[2]=0x80|((cp>>6)&0x3F); tmp[3]=0x80|(cp&0x3F); }
            return strdup(tmp);
        }
        case VAL_OBJ: {
            if (!v.as.obj) return strdup("null");
            switch (v.as.obj->type) {
                case OBJ_STRING:  return strdup(((ObjString *)v.as.obj)->chars);
                case OBJ_FUNCTION: {
                    ObjFunction *f = (ObjFunction *)v.as.obj;
                    snprintf(buf,sizeof(buf),"<%s %s>", f->is_native?"native fn":"fn", f->name?f->name:"?");
                    return strdup(buf);
                }
                case OBJ_INSTANCE:
                    snprintf(buf,sizeof(buf),"<instance of %s>",((ObjInstance*)v.as.obj)->class_name);
                    return strdup(buf);
                case OBJ_EXCEPTION:
                    return strdup(((ObjException *)v.as.obj)->message);
                case OBJ_LIST: {
                    ObjList *l = (ObjList *)v.as.obj;
                    int cap = 32; char *out = malloc(cap); int pos = 0;
                    out[pos++] = '[';
                    for (int i = 0; i < l->count; i++) {
                        char *e = value_to_string(l->items[i]);
                        bool is_str = l->items[i].type==VAL_OBJ && l->items[i].as.obj &&
                                      l->items[i].as.obj->type==OBJ_STRING;
                        int need = pos+(int)strlen(e)+(is_str?2:0)+4;
                        if (need >= cap) { cap=need*2; out=realloc(out,cap); }
                        if (i > 0) { out[pos++]=','; out[pos++]=' '; }
                        if (is_str) out[pos++]='"';
                        int el=(int)strlen(e); memcpy(out+pos,e,el); pos+=el;
                        if (is_str) out[pos++]='"';
                        free(e);
                    }
                    if (pos+2>=cap) { cap=pos+4; out=realloc(out,cap); }
                    out[pos++]=']'; out[pos]='\0';
                    char *r=strdup(out); free(out); return r;
                }
                case OBJ_DICT: {
                    ObjDict *d = (ObjDict *)v.as.obj;
                    int cap=32; char *out=malloc(cap); int pos=0; bool first=true;
                    out[pos++]='{';
                    for (int i=0;i<d->bucket_count;i++) {
                        for (DictNode *e=d->buckets[i];e;e=e->next) {
                            char *k=value_to_string(e->key), *val=value_to_string(e->value);
                            bool ks=e->key.type==VAL_OBJ&&e->key.as.obj&&e->key.as.obj->type==OBJ_STRING;
                            bool vs=e->value.type==VAL_OBJ&&e->value.as.obj&&e->value.as.obj->type==OBJ_STRING;
                            int need=pos+(int)strlen(k)+(int)strlen(val)+(ks?2:0)+(vs?2:0)+8;
                            if (need>=cap){cap=need*2;out=realloc(out,cap);}
                            if (!first){out[pos++]=',';out[pos++]=' ';} first=false;
                            if(ks)out[pos++]='"';
                            int kl=(int)strlen(k);memcpy(out+pos,k,kl);pos+=kl;
                            if(ks)out[pos++]='"';
                            out[pos++]=':';out[pos++]=' ';
                            if(vs)out[pos++]='"';
                            int vl=(int)strlen(val);memcpy(out+pos,val,vl);pos+=vl;
                            if(vs)out[pos++]='"';
                            free(k);free(val);
                        }
                    }
                    if(pos+2>=cap){cap=pos+4;out=realloc(out,cap);}
                    out[pos++]='}';out[pos]='\0';
                    char *r=strdup(out);free(out);return r;
                }
                default: return strdup("<object>");
            }
        }
        default: return strdup("<unknown>");
    }
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
            release_value_inline(inst->fields[i]);
            if (value.type == VAL_OBJ && value.as.obj) retain_obj(value.as.obj);
            inst->fields[i] = value;
            return;
        }
    }
    if (inst->field_count >= inst->field_capacity) {
        inst->field_capacity = inst->field_capacity < 4 ? 4 : inst->field_capacity * 2;
        inst->field_names = realloc(inst->field_names, inst->field_capacity * sizeof(char *));
        inst->fields      = realloc(inst->fields,      inst->field_capacity * sizeof(Value));
    }
    if (value.type == VAL_OBJ && value.as.obj) retain_obj(value.as.obj);
    inst->field_names[inst->field_count] = strdup(name);
    inst->fields     [inst->field_count] = value;
    inst->field_count++;
}

/* ============================================================ */
/* List operations                                               */
/* ============================================================ */

void list_add(ObjList *list, Value value) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity < 8 ? 8 : list->capacity * 2;
        list->items = realloc(list->items, list->capacity * sizeof(Value));
        if (!list->items) { fprintf(stderr,"OOM\n"); exit(1); }
    }
    if (value.type == VAL_OBJ && value.as.obj) retain_obj(value.as.obj);
    list->items[list->count++] = value;
}

void list_insert(ObjList *list, int index, Value value) {
    if (index < 0 || index > list->count) { fprintf(stderr,"list.insert: out of bounds\n"); return; }
    if (list->count >= list->capacity) {
        list->capacity = list->capacity < 8 ? 8 : list->capacity * 2;
        list->items = realloc(list->items, list->capacity * sizeof(Value));
    }
    memmove(&list->items[index+1], &list->items[index], (list->count-index)*sizeof(Value));
    if (value.type == VAL_OBJ && value.as.obj) retain_obj(value.as.obj);
    list->items[index] = value;
    list->count++;
}

Value list_remove(ObjList *list, int index) {
    if (index < 0 || index >= list->count) { fprintf(stderr,"list.remove: out of bounds\n"); return make_null(); }
    Value v = list->items[index];
    memmove(&list->items[index], &list->items[index+1], (list->count-index-1)*sizeof(Value));
    list->count--;
    return v;
}

Value list_pop(ObjList *list) {
    if (!list->count) { fprintf(stderr,"list.pop: empty\n"); return make_null(); }
    return list_remove(list, list->count - 1);
}

void list_clear(ObjList *list) {
    for (int i = 0; i < list->count; i++) release_value_inline(list->items[i]);
    list->count = 0;
}

Value list_get(ObjList *list, int index) {
    if (index < 0 || index >= list->count) { fprintf(stderr,"list: index out of bounds\n"); return make_null(); }
    return list->items[index];
}

void list_set(ObjList *list, int index, Value value) {
    if (index < 0 || index >= list->count) { fprintf(stderr,"list: index out of bounds\n"); return; }
    release_value_inline(list->items[index]);
    if (value.type == VAL_OBJ && value.as.obj) retain_obj(value.as.obj);
    list->items[index] = value;
}

int list_length(ObjList *list) { return list->count; }

/* ============================================================ */
/* Dict operations                                               */
/* ============================================================ */

static void dict_resize(ObjDict *d) {
    int new_cap = d->bucket_count * 2;
    DictNode **nb = calloc(new_cap, sizeof(DictNode *));
    if (!nb) return;
    for (int i = 0; i < d->bucket_count; i++) {
        DictNode *e = d->buckets[i];
        while (e) {
            DictNode *nx = e->next;
            int idx = hash_value(e->key) & (new_cap - 1);
            e->next = nb[idx]; nb[idx] = e;
            e = nx;
        }
    }
    free(d->buckets); d->buckets = nb; d->bucket_count = new_cap;
}

void dict_set(ObjDict *d, Value key, Value value) {
    if (d->entry_count >= d->bucket_count * 3 / 4) dict_resize(d);
    int idx = hash_value(key) & (d->bucket_count - 1);
    for (DictNode *e = d->buckets[idx]; e; e = e->next) {
        if (values_equal(e->key, key)) {
            release_value_inline(e->value);
            if (value.type==VAL_OBJ&&value.as.obj) retain_obj(value.as.obj);
            e->value = value; return;
        }
    }
    DictNode *ne = malloc(sizeof(DictNode));
    if (!ne) { fprintf(stderr,"OOM\n"); return; }
    ne->key = key; ne->value = value;
    ne->next = d->buckets[idx]; d->buckets[idx] = ne;
    if (key.type==VAL_OBJ&&key.as.obj) retain_obj(key.as.obj);
    if (value.type==VAL_OBJ&&value.as.obj) retain_obj(value.as.obj);
    d->entry_count++;
}

Value dict_get(ObjDict *d, Value key) {
    int idx = hash_value(key) & (d->bucket_count - 1);
    for (DictNode *e = d->buckets[idx]; e; e = e->next)
        if (values_equal(e->key, key)) return e->value;
    return make_null();
}

bool dict_has(ObjDict *d, Value key) {
    int idx = hash_value(key) & (d->bucket_count - 1);
    for (DictNode *e = d->buckets[idx]; e; e = e->next)
        if (values_equal(e->key, key)) return true;
    return false;
}

Value dict_remove(ObjDict *d, Value key) {
    int idx = hash_value(key) & (d->bucket_count - 1);
    DictNode **cur = &d->buckets[idx];
    while (*cur) {
        DictNode *e = *cur;
        if (values_equal(e->key, key)) {
            *cur = e->next;
            Value v = e->value;
            release_value_inline(e->key);
            /* don't release value — caller takes ownership */
            free(e); d->entry_count--; return v;
        }
        cur = &e->next;
    }
    return make_null();
}

void dict_clear(ObjDict *d) {
    for (int i = 0; i < d->bucket_count; i++) {
        DictNode *e = d->buckets[i];
        while (e) { DictNode *nx=e->next; release_value_inline(e->key); release_value_inline(e->value); free(e); e=nx; }
        d->buckets[i] = NULL;
    }
    d->entry_count = 0;
}

int dict_length(ObjDict *d) { return d->entry_count; }

Value dict_keys(ObjDict *d) {
    ObjList *list = new_list();
    for (int i = 0; i < d->bucket_count; i++)
        for (DictNode *e = d->buckets[i]; e; e = e->next)
            list_add(list, e->key);
    return make_obj((Object *)list);
}

Value dict_values(ObjDict *d) {
    ObjList *list = new_list();
    for (int i = 0; i < d->bucket_count; i++)
        for (DictNode *e = d->buckets[i]; e; e = e->next)
            list_add(list, e->value);
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
