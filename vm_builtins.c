/* vm_builtins.c — VM global table, native functions, collection method dispatch.
 * Included as a compilation unit alongside vm.c.
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
            if (IS_OBJ(e->value) && AS_OBJ(e->value)) release_obj(AS_OBJ(e->value));
            e->value = value;
            if (IS_OBJ(value) && AS_OBJ(value)) retain_obj(AS_OBJ(value));
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
    if (IS_OBJ(value) && AS_OBJ(value)) retain_obj(AS_OBJ(value));
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
    release_obj((Object *)f);
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
    if (n==1 && IS_INT(args[0])) end = AS_INT(args[0]);
    else if (n>=2) {
        if (IS_INT(args[0])) start = AS_INT(args[0]);
        if (IS_INT(args[1])) end   = AS_INT(args[1]);
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
    if (IS_DOUBLE(args[0])) return make_int((int64_t)AS_DOUBLE(args[0]));
    if (IS_BOOL(args[0])) return make_int(AS_BOOL(args[0]) ? 1 : 0);
    if (IS_OBJ(args[0]) && AS_OBJ(args[0]) && AS_OBJ(args[0])->type == OBJ_STRING)
        return make_int(atoll(((ObjString*)AS_OBJ(args[0]))->chars));
    return make_int(0);
}

static Value bn_float(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_double(0.0);
    if (IS_DOUBLE(args[0])) return args[0];
    if (IS_INT(args[0])) return make_double((double)AS_INT(args[0]));
    if (IS_BOOL(args[0])) return make_double(AS_BOOL(args[0]) ? 1.0 : 0.0);
    if (IS_OBJ(args[0]) && AS_OBJ(args[0]) && AS_OBJ(args[0])->type == OBJ_STRING)
        return make_double(atof(((ObjString*)AS_OBJ(args[0]))->chars));
    return make_double(0.0);
}

static Value bn_len(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n || !IS_OBJ(args[0]) || !AS_OBJ(args[0])) return make_int(0);
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
        else if (IS_DOUBLE(args[0])) t = "double";
        else if (IS_OBJ(args[0]) && AS_OBJ(args[0])) {
            switch (AS_OBJ(args[0])->type) {
                case OBJ_STRING:   t = "string";   break;
                case OBJ_LIST:     t = "list";     break;
                case OBJ_DICT:     t = "dict";     break;
                case OBJ_INSTANCE: t = ((ObjInstance*)AS_OBJ(args[0]))->class_name; break;
                case OBJ_FUNCTION: t = "function"; break;
                default:           t = "object";   break;
            }
        }
    }
    return make_obj((Object *)new_string(t,(int)strlen(t)));
}

static Value bn_clock(VM *vm, Value *args, int n) {
    (void)vm; (void)args; (void)n;
    return make_double((double)clock() / CLOCKS_PER_SEC);
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
}

/* ============================================================ */
/* Collection method dispatch                                    */
/* ============================================================ */

bool vm_invoke_list(VM *vm, ObjList *list, const char *method,
                    Value *args, int nargs, Value *result) {
    (void)vm;
    if (!strcmp(method,"add")    && nargs>=1) { list_add(list,args[0]);                       *result=make_null(); return true; }
    if (!strcmp(method,"insert") && nargs>=2 && IS_INT(args[0])) { list_insert(list, AS_INT(args[0]), args[1]); *result=make_null(); return true; }
    if (!strcmp(method,"remove") && nargs>=1 && IS_INT(args[0])) { *result=list_remove(list, AS_INT(args[0])); return true; }
    if (!strcmp(method,"pop"))   { *result=list_pop(list);    return true; }
    if (!strcmp(method,"clear")) { list_clear(list); *result=make_null(); return true; }
    if (!strcmp(method,"length"))    { *result=make_int(list_length(list)); return true; }
    return false;
}

bool vm_invoke_dict(VM *vm, ObjDict *dict, const char *method,
                    Value *args, int nargs, Value *result) {
    (void)vm;
    if (!strcmp(method,"has")    && nargs>=1) { *result=make_bool(dict_has(dict,args[0]));   return true; }
    if (!strcmp(method,"remove") && nargs>=1) { *result=dict_remove(dict,args[0]);           return true; }
    if (!strcmp(method,"keys"))              { *result=dict_keys(dict);                      return true; }
    if (!strcmp(method,"values"))            { *result=dict_values(dict);                    return true; }
    if (!strcmp(method,"clear"))             { dict_clear(dict); *result=make_null();         return true; }
    if (!strcmp(method,"length"))            { *result=make_int(dict_length(dict));           return true; }
    return false;
}
