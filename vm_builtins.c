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
    uint32_t bucket = hash_cstr(name) & (VM_GLOBAL_BUCKETS - 1);
    for (GlobalEntry *e = vm->globals[bucket]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            if (e->is_const) { fprintf(stderr, "vm: cannot reassign const '%s'\n", name); return; }
            release_obj(e->value.type == VAL_OBJ ? e->value.as.obj : NULL);
            e->value = value;
            if (value.type == VAL_OBJ && value.as.obj) retain_obj(value.as.obj);
            return;
        }
    }
    GlobalEntry *ne = malloc(sizeof(GlobalEntry));
    ne->name     = strdup(name);
    ne->value    = value;
    ne->is_const = is_const;
    ne->next     = vm->globals[bucket];
    vm->globals[bucket] = ne;
    if (value.type == VAL_OBJ && value.as.obj) retain_obj(value.as.obj);
}

bool vm_get_global(VM *vm, const char *name, Value *out) {
    uint32_t bucket = hash_cstr(name) & (VM_GLOBAL_BUCKETS - 1);
    for (GlobalEntry *e = vm->globals[bucket]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) { *out = e->value; return true; }
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
    if (n==1 && args[0].type==VAL_INT) end=args[0].as.integer;
    else if (n>=2) {
        if (args[0].type==VAL_INT) start=args[0].as.integer;
        if (args[1].type==VAL_INT) end  =args[1].as.integer;
    }
    ObjList *l = new_list();
    for (int64_t i=start; i<end; i++) list_add(l, make_int(i));
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
    switch (args[0].type) {
        case VAL_INT:    return args[0];
        case VAL_UINT:   return make_int((int64_t)args[0].as.uint_val);
        case VAL_FLOAT:  return make_int((int64_t)args[0].as.float_val);
        case VAL_DOUBLE: return make_int((int64_t)args[0].as.double_val);
        case VAL_BOOL:   return make_int(args[0].as.boolean ? 1 : 0);
        case VAL_CHAR:   return make_int((int64_t)args[0].as.char_val);
        case VAL_OBJ:
            if (args[0].as.obj && args[0].as.obj->type==OBJ_STRING)
                return make_int(atoll(((ObjString*)args[0].as.obj)->chars));
            break;
        default: break;
    }
    return make_int(0);
}

static Value bn_float(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_float(0.0f);
    switch (args[0].type) {
        case VAL_INT:    return make_float((float)args[0].as.integer);
        case VAL_UINT:   return make_float((float)args[0].as.uint_val);
        case VAL_FLOAT:  return args[0];
        case VAL_DOUBLE: return make_float((float)args[0].as.double_val);
        case VAL_BOOL:   return make_float(args[0].as.boolean ? 1.f : 0.f);
        case VAL_OBJ:
            if (args[0].as.obj && args[0].as.obj->type==OBJ_STRING)
                return make_float((float)atof(((ObjString*)args[0].as.obj)->chars));
            break;
        default: break;
    }
    return make_float(0.0f);
}

static Value bn_len(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n || args[0].type!=VAL_OBJ || !args[0].as.obj) return make_int(0);
    switch (args[0].as.obj->type) {
        case OBJ_STRING:  return make_int(((ObjString*)args[0].as.obj)->length);
        case OBJ_LIST:    return make_int(((ObjList*)  args[0].as.obj)->count);
        case OBJ_DICT:    return make_int(((ObjDict*)  args[0].as.obj)->entry_count);
        default:          return make_int(0);
    }
}

static Value bn_type(VM *vm, Value *args, int n) {
    (void)vm;
    const char *t = "null";
    if (n) switch (args[0].type) {
        case VAL_BOOL:   t="bool";   break;
        case VAL_INT:    t="int";    break;
        case VAL_UINT:   t="uint";   break;
        case VAL_FLOAT:  t="float";  break;
        case VAL_DOUBLE: t="double"; break;
        case VAL_NAN:    t="NaN";    break;
        case VAL_CHAR:   t="char";   break;
        case VAL_OBJ:
            if (!args[0].as.obj) break;
            switch (args[0].as.obj->type) {
                case OBJ_STRING:   t="string";   break;
                case OBJ_LIST:     t="list";     break;
                case OBJ_DICT:     t="dict";     break;
                case OBJ_INSTANCE: t=((ObjInstance*)args[0].as.obj)->class_name; break;
                case OBJ_FUNCTION: t="function"; break;
                default:           t="object";   break;
            } break;
        default: break;
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
    if (!strcmp(method,"insert") && nargs>=2 && args[0].type==VAL_INT) { list_insert(list,(int)args[0].as.integer,args[1]); *result=make_null(); return true; }
    if (!strcmp(method,"remove") && nargs>=1 && args[0].type==VAL_INT) { *result=list_remove(list,(int)args[0].as.integer); return true; }
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
