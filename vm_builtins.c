/* vm_builtins.c — VM global table, native functions, collection method dispatch.
 * Included as a compilation unit alongside vm.c.
 *
 * FFI CONTRACT: Any builtin returning a double MUST use make_double() to
 * sanitize hardware NaNs. Raw NaNs can collide with object type signatures.
 * Use RETURN_EXT_DOUBLE() for external C function results.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "vm.h"
#include "value.h"
#include "chunk.h"

/* ============================================================ */
/* Global variable table                                         */
/* ============================================================ */

static uint32_t hash_cstr(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

void vm_set_global(VM *vm, const char *name, Value value, bool is_const) {
    uint32_t h = hash_cstr(name);
    uint32_t bucket = h & (VM_GLOBAL_BUCKETS - 1);
    for (GlobalEntry *e = vm->globals[bucket]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            if (e->is_const) { fprintf(stderr, "vm: cannot reassign const '%s'\n", name); return; }
            e->value = value;
            /* invalidate possible cache entry */
            int ic_idx = h & (IC_CACHE_SIZE - 1);
            if (vm->global_ic[ic_idx].entry == e) vm->global_ic[ic_idx].key = NULL;
            return;
        }
    }
    GlobalEntry *ne = malloc(sizeof(GlobalEntry));
    ne->name     = strdup(name);
    ne->value    = value;
    ne->is_const = is_const;
    ne->next     = vm->globals[bucket];
    vm->globals[bucket] = ne;
}

bool vm_get_global(VM *vm, const char *name, Value *out) {
    uint32_t bucket = hash_cstr(name) & (VM_GLOBAL_BUCKETS - 1);
    for (GlobalEntry *e = vm->globals[bucket]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) { *out = e->value; return true; }
    }
    return false;
}

bool vm_get_global_fast(VM *vm, ObjString *name, Value *out) {
    uint32_t h = name->hash;
    int idx = h & (IC_CACHE_SIZE - 1);
    IC_GlobalEntry *ic = &vm->global_ic[idx];
    if (ic->key == name) {
        *out = ic->entry->value;
        return true;
    }
    uint32_t bucket = h & (VM_GLOBAL_BUCKETS - 1);
    for (GlobalEntry *e = vm->globals[bucket]; e; e = e->next) {
        if (e->name == name->chars || strcmp(e->name, name->chars) == 0) {
            *out = e->value;
            ic->key = name;
            ic->entry = e;
            return true;
        }
    }
    return false;
}

GlobalEntry *vm_resolve_global(VM *vm, const char *name) {
    uint32_t bucket = hash_cstr(name) & (VM_GLOBAL_BUCKETS - 1);
    for (GlobalEntry *e = vm->globals[bucket]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

void vm_define_native(VM *vm, const char *name, NativeFn fn) {
    ObjFunction *f = new_native_function(name, fn);
    Value v = make_obj((Object *)f);
    vm_set_global(vm, name, v, true);
}

/* ============================================================ */
/* Built-in native functions                                     */
/* ============================================================ */

static Value bn_print(VM *vm, Value *args, int n) {
    (void)vm;
    for (int i = 0; i < n; i++) {
        char *s = value_to_string(args[i]); printf("%s", s); free(s);
        if (i < n-1) printf(" ");
    }
    printf("\n"); return make_null();
}

static Value bn_input(VM *vm, Value *args, int n) {
    (void)vm;
    if (n > 0) { char *p = value_to_string(args[0]); printf("%s", p); free(p); }
    char buf[1024];
    if (fgets(buf, sizeof(buf), stdin)) {
        size_t l = strlen(buf);
        if (l && buf[l-1]=='\n') buf[--l]='\0';
        return make_obj((Object *)new_string(buf,(int)l));
    }
    return make_null();
}

static Value bn_range(VM *vm, Value *args, int n) {
    (void)vm;
    int64_t start=0, end=0;
    if (n==1 && IS_NUMBER(args[0])) end = as_int64(args[0]);
    else if (n>=2) {
        if (IS_NUMBER(args[0])) start = as_int64(args[0]);
        if (IS_NUMBER(args[1])) end   = as_int64(args[1]);
    }
    ObjList *l = new_list((int)(end - start));
    for (int64_t i=start; i<end; i++) list_add(l, make_int((int32_t)i));
    return make_obj((Object *)l);
}

static Value bn_str(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_obj((Object *)new_string("",0));
    char *s = value_to_string(args[0]);
    Value v = make_obj((Object *)new_string(s,(int)strlen(s)));
    free(s); return v;
}

static Value bn_int(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_int(0);
    if (IS_INT(args[0])) return args[0];
    if (IS_INT64(args[0])) {
        int64_t v = as_int64(args[0]);
        return (v >= INT32_MIN && v <= INT32_MAX) ? make_int((int32_t)v) : make_int64(v);
    }
    if (IS_DOUBLE(args[0])) {
        int64_t v = (int64_t)AS_DOUBLE(args[0]);
        return (v >= INT32_MIN && v <= INT32_MAX) ? make_int((int32_t)v) : make_int64(v);
    }
    if (IS_BOOL(args[0])) return make_int(AS_BOOL(args[0]) ? 1 : 0);
    if (IS_STRING(args[0]))
    {
        int64_t v = atoll(((ObjString*)AS_OBJ(args[0]))->chars);
        return (v >= INT32_MIN && v <= INT32_MAX) ? make_int((int32_t)v) : make_int64(v);
    }
    return make_int(0);
}

static Value bn_float(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_double(0.0);
    if (IS_DOUBLE(args[0])) return args[0];
    if (IS_INT(args[0])) return make_double((double)AS_INT(args[0]));
    if (IS_INT64(args[0])) return make_double((double)as_int64(args[0]));
    if (IS_BOOL(args[0])) return make_double(AS_BOOL(args[0]) ? 1.0 : 0.0);
    if (IS_STRING(args[0])) {
        const char *s = ((ObjString*)AS_OBJ(args[0]))->chars;
        if (strcmp(s, "nan") == 0 || strcmp(s, "NaN") == 0) return make_double(0.0/0.0);
        if (strcmp(s, "inf") == 0 || strcmp(s, "infinity") == 0 || strcmp(s, "Infinity") == 0) return make_pos_inf();
        if (strcmp(s, "-inf") == 0 || strcmp(s, "-infinity") == 0 || strcmp(s, "-Infinity") == 0) return make_neg_inf();
        return make_double(atof(s));
    }
    return make_double(0.0);
}

static Value bn_len(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n || !IS_OBJ(args[0])) return make_int(0);
    switch (AS_OBJ(args[0])->type) {
        case OBJ_STRING:  return make_int(((ObjString*)AS_OBJ(args[0]))->length);
        case OBJ_LIST:    return make_int(((ObjList*)  AS_OBJ(args[0]))->count);
        case OBJ_DICT:    return make_int(((ObjDict*)  AS_OBJ(args[0]))->entry_count);
        default:          return make_int(0);
    }
}

static Value bn_type(VM *vm, Value *args, int n) {
    (void)vm;
    const char *t = "null";
    if (n) {
        if (IS_NIL(args[0])) t = "null";
        else if (IS_BOOL(args[0])) t = "bool";
        else if (IS_INT(args[0])) t = "int";
        else if (IS_INT64(args[0])) t = "int64";
        else if (IS_POS_INF(args[0])) t = "inf";
        else if (IS_NEG_INF(args[0])) t = "-inf";
        else if (IS_NAN(args[0])) t = "nan";
        else if (IS_DOUBLE(args[0])) t = "float";
        else if (IS_OBJ(args[0])) {
            switch (AS_OBJ(args[0])->type) {
                case OBJ_STRING:     t = "string";     break;
                case OBJ_LIST:       t = "list";       break;
                case OBJ_DICT:       t = "dict";       break;
                case OBJ_INSTANCE: {
                    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
                    if (inst->klass) {
                        return make_obj((Object*)inst->klass);
                    }
                    t = "instance";
                    break;
                }
                case OBJ_FUNCTION:   t = "function";   break;
                case OBJ_CLOSURE:    t = "closure";    break;
                case OBJ_ENUM:       t = "enum";       break;
                case OBJ_CLASS:      t = "class";      break;
                case OBJ_USERDATA:   t = "userdata";   break;
                default:             t = "object";     break;
            }
        }
    }
    return make_obj((Object *)new_string(t,(int)strlen(t)));
}

static Value bn_clock(VM *vm, Value *args, int n) {
    (void)vm; (void)args; (void)n;
    return make_double((double)clock() / CLOCKS_PER_SEC);
}

extern Object *all_objects;
extern int allocated_objects;

static Value bn_gc_info(VM *vm, Value *args, int n) {
    (void)vm; (void)args; (void)n;
    int count = 0;
    Object *obj = all_objects;
    while(obj) {
        count++;
        obj = obj->next;
    }
    printf("Allocated objects via counter: %d, via list: %d\n", allocated_objects, count);
    return make_int(count);
}

void mark_and_sweep(VM *vm);

static Value bn_gc(VM *vm, Value *args, int n) {
    (void)args; (void)n;
    mark_and_sweep(vm);
    return make_null();
}

static Value bn_isnan(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_bool(false);
    return make_bool(IS_NAN(args[0]));
}

static Value bn_isinf(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_bool(false);
    return make_bool(IS_INF(args[0]));
}

/* ============================================================ */
/* Vector constructors — global vec2, vec3, vec4                */
/* ============================================================ */

static Value bn_vec2(VM *vm, Value *args, int n) {
    if (n != 2) {
        luna_throw(vm, vm->argument_error_class, "vec2() expects exactly 2 arguments (x, y)");
    }
    if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
        luna_throw(vm, vm->type_error_class, "vec2() arguments must be numeric");
    }
    float x = (float)value_to_double(args[0]);
    float y = (float)value_to_double(args[1]);
    return make_obj((Object*)new_vector(x, y, 0.0f, 0.0f));
}

static Value bn_vec3(VM *vm, Value *args, int n) {
    if (n != 3) {
        luna_throw(vm, vm->argument_error_class, "vec3() expects exactly 3 arguments (x, y, z)");
    }
    if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1]) || !IS_NUMBER(args[2])) {
        luna_throw(vm, vm->type_error_class, "vec3() arguments must be numeric");
    }
    float x = (float)value_to_double(args[0]);
    float y = (float)value_to_double(args[1]);
    float z = (float)value_to_double(args[2]);
    return make_obj((Object*)new_vector(x, y, z, 0.0f));
}

static Value bn_vec4(VM *vm, Value *args, int n) {
    if (n != 4) {
        luna_throw(vm, vm->argument_error_class, "vec4() expects exactly 4 arguments (x, y, z, w)");
    }
    if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1]) || !IS_NUMBER(args[2]) || !IS_NUMBER(args[3])) {
        luna_throw(vm, vm->type_error_class, "vec4() arguments must be numeric");
    }
    float x = (float)value_to_double(args[0]);
    float y = (float)value_to_double(args[1]);
    float z = (float)value_to_double(args[2]);
    float w = (float)value_to_double(args[3]);
    return make_obj((Object*)new_vector(x, y, z, w));
}

static Value bn_mat4(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, vm->argument_error_class, "mat4() takes no arguments");
    }
    return make_obj((Object*)new_matrix());
}

/* ============================================================ */
/* Dict method native functions — args[0] = dict (self)         */
/* ============================================================ */

static Value dict_method_keys(VM *vm, Value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || !IS_DICT(args[0])) return make_null();
    return dict_keys((ObjDict*)AS_OBJ(args[0]));
}

static Value dict_method_has(VM *vm, Value *args, int nargs) {
    (void)vm;
    if (nargs < 2 || !IS_DICT(args[0])) return make_null();
    return make_bool(dict_has((ObjDict*)AS_OBJ(args[0]), args[1]));
}

static Value dict_method_remove(VM *vm, Value *args, int nargs) {
    (void)vm;
    if (nargs < 2 || !IS_DICT(args[0])) return make_null();
    return dict_remove((ObjDict*)AS_OBJ(args[0]), args[1]);
}

static Value dict_method_values(VM *vm, Value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || !IS_DICT(args[0])) return make_null();
    return dict_values((ObjDict*)AS_OBJ(args[0]));
}

static Value dict_method_clear(VM *vm, Value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || !IS_DICT(args[0])) return make_null();
    dict_clear((ObjDict*)AS_OBJ(args[0]));
    return make_null();
}

static Value dict_method_length(VM *vm, Value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || !IS_DICT(args[0])) return make_null();
    return make_int(dict_length((ObjDict*)AS_OBJ(args[0])));
}

/* Lookup table mapping method name -> pre-created ObjFunction */
typedef struct {
    const char  *name;
    int          len;
    ObjFunction *fn;
} DictMethodEntry;

static DictMethodEntry dict_method_table[] = {
    {"keys",   4, NULL},
    {"has",    3, NULL},
    {"remove", 6, NULL},
    {"values", 6, NULL},
    {"clear",  5, NULL},
    {"length", 6, NULL},
};
#define DICT_METHOD_COUNT (sizeof(dict_method_table) / sizeof(dict_method_table[0]))

ObjFunction *vm_dict_method_lookup(const char *name, int len) {
    for (int i = 0; i < (int)DICT_METHOD_COUNT; i++) {
        if (dict_method_table[i].len == len &&
            memcmp(dict_method_table[i].name, name, len) == 0) {
            return dict_method_table[i].fn;
        }
    }
    return NULL;
}

void vm_register_builtins(VM *vm) {
    vm_define_native(vm, "print", bn_print);
    vm_define_native(vm, "input", bn_input);
    vm_define_native(vm, "range", bn_range);
    vm_define_native(vm, "str",   bn_str);
    vm_define_native(vm, "int",   bn_int);
    vm_define_native(vm, "float", bn_float);
    vm_define_native(vm, "len",   bn_len);
    vm_define_native(vm, "type",  bn_type);
    vm_define_native(vm, "clock", bn_clock);
    vm_define_native(vm, "gc_info", bn_gc_info);
    vm_define_native(vm, "gc", bn_gc);
    vm_define_native(vm, "isnan", bn_isnan);
    vm_define_native(vm, "isinf", bn_isinf);
    vm_define_native(vm, "vec2",  bn_vec2);
    vm_define_native(vm, "vec3",  bn_vec3);
    vm_define_native(vm, "vec4",  bn_vec4);
    vm_define_native(vm, "mat4",  bn_mat4);

    /* Pre-create dict method native function objects (live for process lifetime) */
    dict_method_table[0].fn = new_native_function("dict.keys",   dict_method_keys);
    dict_method_table[1].fn = new_native_function("dict.has",    dict_method_has);
    dict_method_table[2].fn = new_native_function("dict.remove", dict_method_remove);
    dict_method_table[3].fn = new_native_function("dict.values", dict_method_values);
    dict_method_table[4].fn = new_native_function("dict.clear",  dict_method_clear);
    dict_method_table[5].fn = new_native_function("dict.length", dict_method_length);
}

/* ============================================================ */
/* Collection method dispatch                                    */
/* ============================================================ */

bool vm_invoke_list(VM *vm, ObjList *list, const char *method,
                    Value *args, int nargs, Value *result) {
    (void)vm;
    if ((!strcmp(method,"add") || !strcmp(method,"append")) && nargs>=1) { list_add(list,args[0]); *result=make_null(); return true; }
    if (!strcmp(method,"insert") && nargs>=2 && IS_INT(args[0])) { list_insert(list, AS_INT(args[0]), args[1]); *result=make_null(); return true; }
    if (!strcmp(method,"remove") && nargs>=1 && IS_INT(args[0])) { *result=list_remove(list, AS_INT(args[0])); return true; }
    if (!strcmp(method,"pop"))   { *result=list_pop(list);    return true; }
    if (!strcmp(method,"clear")) { list_clear(list); *result=make_null(); return true; }
    if (!strcmp(method,"length") || !strcmp(method,"size")) { *result=make_int(list_length(list)); return true; }
    return false;
}

bool vm_invoke_dict(VM *vm, ObjDict *dict, const char *method,
                    Value *args, int nargs, Value *result) {
    ObjFunction *fn = vm_dict_method_lookup(method, (int)strlen(method));
    if (!fn) return false;
    Value scratch[256];
    scratch[0] = make_obj((Object*)dict);
    for (int i = 0; i < nargs; i++) scratch[i + 1] = args[i];
    return vm_call_native(vm, fn->native_fn, scratch, nargs + 1, result);
}

bool vm_invoke_enum(VM *vm, ObjEnum *enm, const char *method,
                    Value *args, int nargs, Value *result) {
    (void)vm; (void)args; (void)nargs;
    if (!strcmp(method, "values")) {
        ObjList *lst = new_list(enm->count);
        for (int i = 0; i < enm->count; i++) list_add(lst, make_int(enm->values[i]));
        *result = make_obj((Object*)lst);
        return true;
    }
    if (!strcmp(method, "keys")) {
        ObjList *lst = new_list(enm->count);
        for (int i = 0; i < enm->count; i++)
            list_add(lst, make_obj((Object*)new_string(enm->names[i], (int)strlen(enm->names[i]))));
        *result = make_obj((Object*)lst);
        return true;
    }
    if (!strcmp(method, "count") || !strcmp(method, "length") || !strcmp(method, "size")) {
        *result = make_int(enm->count);
        return true;
    }
    return false;
}
