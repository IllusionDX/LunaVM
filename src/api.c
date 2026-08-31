/* api.c — Embeddable C API for the Luna VM (language-agnostic).
 *
 * A stack-based API for driving a VM.  The object model is reached only
 * through the FrontendDef hooks (vm.h) and the Type MOP vtable, so this file
 * never names a concrete frontend type.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "api.h"

#define API_STACK_INIT 64

struct APIState {
    VM    *vm;
    Value *stack;
    int    cap;
    int    top;
};

/* ============================================================ */
/* State lifecycle                                               */
/* ============================================================ */

APIState *api_open(VM *vm) {
    if (!vm || vm->api_state) return NULL;
    APIState *L = (APIState *)calloc(1, sizeof(APIState));
    if (!L) return NULL;
    L->vm = vm;
    L->cap = API_STACK_INIT;
    L->stack = (Value *)calloc((size_t)L->cap, sizeof(Value));
    if (!L->stack) { free(L); return NULL; }
    L->top = 0;
    vm->api_state = L;
    return L;
}

void api_close(APIState *L) {
    if (!L) return;
    VM *vm = L->vm;
    vm->api_state = NULL;
    free(L->stack);
    free(L);
}

/* Mark the API stack so its values survive collection.  Called from a
 * frontend's mark_roots hook. */
void api_mark_roots(VM *vm) {
    APIState *L = (APIState *)vm->api_state;
    if (!L) return;
    for (int i = 0; i < L->top; i++) {
        vm_mark_value(vm, L->stack[i]);
    }
}

/* ============================================================ */
/* Stack manipulation                                            */
/* ============================================================ */

static Value *stack_ptr(APIState *L, int idx) {
    if (idx < 0) idx = L->top + idx;
    if (idx < 0 || idx >= L->top) return NULL;
    return &L->stack[idx];
}

static bool grow_stack(APIState *L, int min) {
    if (min <= L->cap) return true;
    int new_cap = L->cap * 2;
    while (new_cap < min) new_cap *= 2;
    Value *new_stack = (Value *)realloc(L->stack, (size_t)new_cap * sizeof(Value));
    if (!new_stack) return false;
    for (int i = L->cap; i < new_cap; i++) new_stack[i] = make_null();
    L->stack = new_stack;
    L->cap = new_cap;
    return true;
}

int api_get_top(APIState *L) { return L ? L->top : 0; }

void api_set_top(APIState *L, int n) {
    if (!L) return;
    if (n < 0) n = L->top + n + 1; /* api_pop semantics: -1 -> top-1 */
    if (n < 0) n = 0;
    if (n > L->top) n = L->top;
    L->top = n;
}

void api_push_value(APIState *L, int idx) {
    Value *v = stack_ptr(L, idx);
    if (!v) return api_push_nil(L);
    if (grow_stack(L, L->top + 1)) L->stack[L->top++] = *v;
}

void api_remove(APIState *L, int idx) {
    Value *v = stack_ptr(L, idx);
    if (!v) return;
    for (int i = (int)(v - L->stack); i < L->top - 1; i++) L->stack[i] = L->stack[i + 1];
    L->top--;
}

void api_insert(APIState *L, int idx) {
    Value *v = stack_ptr(L, idx);
    if (!v) return;
    int pos = (int)(v - L->stack);
    if (grow_stack(L, L->top + 1)) {
        for (int i = L->top; i > pos; i--) L->stack[i] = L->stack[i - 1];
        L->stack[pos] = L->stack[L->top];
        L->top++;
    }
}

void api_replace(APIState *L, int idx) {
    Value *v = stack_ptr(L, idx);
    if (!v || L->top < 1) return;
    *v = L->stack[L->top - 1];
}

int api_check_stack(APIState *L, int n) {
    if (!L) return 0;
    if (n >= 0 && L->cap - L->top < n) {
        if (!grow_stack(L, L->top + n)) return 0;
    }
    return 1;
}

/* ============================================================ */
/* Type checks and access                                        */
/* ============================================================ */

int api_type(APIState *L, int idx) {
    Value *v = stack_ptr(L, idx);
    if (!v) return LUNA_TNIL;
    if (L->vm && L->vm->frontend_def && L->vm->frontend_def->object &&
        L->vm->frontend_def->object->type_of)
        return L->vm->frontend_def->object->type_of(*v);
    return LUNA_TNIL;
}

bool api_is_nil(APIState *L, int idx) { Value *v = stack_ptr(L, idx); return v && IS_NIL(*v); }
bool api_is_boolean(APIState *L, int idx) { Value *v = stack_ptr(L, idx); return v && IS_BOOL(*v); }

bool api_is_number(APIState *L, int idx) {
    int t = api_type(L, idx);
    return t == LUNA_TINTEGER || t == LUNA_TNUMBER;
}

bool api_is_integer(APIState *L, int idx) {
    return api_type(L, idx) == LUNA_TINTEGER;
}

bool api_is_string(APIState *L, int idx) {
    return api_type(L, idx) == LUNA_TSTRING;
}

bool api_is_userdata(APIState *L, int idx) {
    return api_type(L, idx) == LUNA_TUSERDATA;
}

bool api_is_function(APIState *L, int idx) {
    return api_type(L, idx) == LUNA_TFUNCTION;
}

bool api_is_cfunction(APIState *L, int idx) {
    Value *v = stack_ptr(L, idx);
    if (!v || !IS_OBJ(*v)) return false;
    if (L->vm && L->vm->frontend_def && L->vm->frontend_def->object &&
        L->vm->frontend_def->object->is_cfunction)
        return L->vm->frontend_def->object->is_cfunction(*v);
    return false;
}

bool api_to_boolean(APIState *L, int idx) {
    Value *v = stack_ptr(L, idx);
    if (!v) return false;
    if (IS_BOOL(*v)) return AS_BOOL(*v);
    return is_truthy(*v);
}

double api_to_number(APIState *L, int idx) {
    Value *v = stack_ptr(L, idx);
    if (!v) return 0.0;
    if (IS_DOUBLE(*v)) return AS_DOUBLE(*v);
    return (double)api_to_integer(L, idx);
}

/* Exact integer extraction through the frontend bridge. Returns false when
 * the value is a frontend integer that does not fit in an int64_t (the API
 * cannot represent it); callers decide how to report that. */
static bool integer_of(APIState *L, Value v, int64_t *out) {
    if (L->vm && L->vm->frontend_def && L->vm->frontend_def->object &&
        L->vm->frontend_def->object->integer_value)
        return L->vm->frontend_def->object->integer_value(v, out);
    return false;
}

int64_t api_to_integer(APIState *L, int idx) {
    Value *v = stack_ptr(L, idx);
    if (!v) return 0;
    if (IS_INT(*v)) return AS_INT(*v);
    if (IS_DOUBLE(*v)) return (int64_t)AS_DOUBLE(*v);
    if (IS_OBJ(*v) && api_type(L, idx) == LUNA_TINTEGER) {
        int64_t out;
        if (integer_of(L, *v, &out)) return out;
    }
    return 0;
}

const char *api_to_string(APIState *L, int idx, size_t *len) {
    Value *v = stack_ptr(L, idx);
    if (!v || !IS_OBJ(*v) || api_type(L, idx) != LUNA_TSTRING) {
        if (len) *len = 0;
        return NULL;
    }
    if (L->vm && L->vm->frontend_def && L->vm->frontend_def->object &&
        L->vm->frontend_def->object->cstring)
        return L->vm->frontend_def->object->cstring(*v, len);
    if (len) *len = 0;
    return NULL;
}

/* ============================================================ */
/* Push values                                                   */
/* ============================================================ */

void api_push_nil(APIState *L) { if (L && grow_stack(L, L->top + 1)) L->stack[L->top++] = make_null(); }
void api_push_boolean(APIState *L, bool b) { if (L && grow_stack(L, L->top + 1)) L->stack[L->top++] = make_bool(b); }
void api_push_number(APIState *L, double n) { if (L && grow_stack(L, L->top + 1)) L->stack[L->top++] = make_double(n); }

void api_push_integer(APIState *L, int64_t n) {
    if (!L || !grow_stack(L, L->top + 1)) return;
    if (n >= INT32_MIN && n <= INT32_MAX)
        L->stack[L->top++] = make_int((int32_t)n);
    else if (L->vm->frontend_def && L->vm->frontend_def->object &&
             L->vm->frontend_def->object->make_integer)
        L->stack[L->top++] = L->vm->frontend_def->object->make_integer(n);
    else
        L->stack[L->top++] = make_null();
}

void api_push_string(APIState *L, const char *s) {
    if (!L || !grow_stack(L, L->top + 1)) return;
    L->stack[L->top++] = L->vm->frontend && L->vm->frontend->new_string
        ? L->vm->frontend->new_string(L->vm, s, (int)strlen(s)) : make_null();
}

void api_push_lstring(APIState *L, const char *s, size_t len) {
    if (!L || !grow_stack(L, L->top + 1)) return;
    L->stack[L->top++] = L->vm->frontend && L->vm->frontend->new_string
        ? L->vm->frontend->new_string(L->vm, s, (int)len) : make_null();
}

void api_push_cfunction(APIState *L, api_CFunction fn) {
    if (!L || !grow_stack(L, L->top + 1)) return;
    if (L->vm->frontend_def && L->vm->frontend_def->object &&
        L->vm->frontend_def->object->new_cfunction)
        L->stack[L->top++] = L->vm->frontend_def->object->new_cfunction(fn);
    else
        L->stack[L->top++] = make_null();
}

/* ============================================================ */
/* Dict operations (via getattr/setattr)                         */
/* ============================================================ */

void api_new_dict(APIState *L) {
    if (!L || !grow_stack(L, L->top + 1)) return;
    Value out; bool ok = vm_new_dict(L->vm, &out);
    L->stack[L->top++] = ok ? out : make_null();
}

void api_set_field(APIState *L, int idx, const char *key) {
    Value *dv = stack_ptr(L, idx);
    if (!dv || L->top < 1) return;
    Value name = L->vm->frontend && L->vm->frontend->new_string
        ? L->vm->frontend->new_string(L->vm, key, (int)strlen(key)) : make_null();
    vm_setattr(L->vm, *dv, name, L->stack[L->top - 1]);
    L->top--;
}

int api_get_field(APIState *L, int idx, const char *key) {
    Value *dv = stack_ptr(L, idx);
    if (!dv || !IS_OBJ(*dv)) return LUNA_TNIL;
    Value name = L->vm->frontend && L->vm->frontend->new_string
        ? L->vm->frontend->new_string(L->vm, key, (int)strlen(key)) : make_null();
    Value out;
    if (!vm_getattr(L->vm, *dv, name, false, &out)) out = make_null();
    if (!grow_stack(L, L->top + 1)) return LUNA_TNIL;
    L->stack[L->top++] = out;
    return api_type(L, L->top - 1);
}

/* ============================================================ */
/* List operations (via hooks + Type MOP)                         */
/* ============================================================ */

void api_new_list(APIState *L) {
    if (!L || !grow_stack(L, L->top + 1)) return;
    Value out; bool ok = vm_new_list(L->vm, 0, &out);
    L->stack[L->top++] = ok ? out : make_null();
}

void api_list_append(APIState *L, int idx) {
    Value *lv = stack_ptr(L, idx);
    if (!lv || L->top < 1 || !IS_OBJ(*lv)) return;
    vm_list_append(L->vm, *lv, L->stack[L->top - 1]);
    L->top--;
}

void api_get_index(APIState *L, int idx, int n) {
    Value *lv = stack_ptr(L, idx);
    if (!lv || !IS_OBJ(*lv)) return api_push_nil(L);
    Value out;
    if (!vm_getitem(L->vm, *lv, make_int(n), false, &out)) out = make_null();
    if (grow_stack(L, L->top + 1)) L->stack[L->top++] = out;
}

void api_set_index(APIState *L, int idx, int n) {
    Value *lv = stack_ptr(L, idx);
    if (!lv || L->top < 1 || !IS_OBJ(*lv)) return;
    vm_setitem(L->vm, *lv, make_int(n), L->stack[L->top - 1]);
    L->top--;
}

/* ============================================================ */
/* Userdata                                                      */
/* ============================================================ */

static const FrontendObject *feo(APIState *L) {
    return (L && L->vm->frontend_def) ? L->vm->frontend_def->object : NULL;
}

void api_new_userdata(APIState *L, void *data, const char *tag, void (*finalizer)(void *)) {
    if (!L || !grow_stack(L, L->top + 1)) return;
    const FrontendObject *o = feo(L);
    L->stack[L->top++] = (o && o->new_userdata) ? o->new_userdata(data, tag, finalizer) : make_null();
}

void *api_to_userdata(APIState *L, int idx) {
    Value *v = stack_ptr(L, idx);
    if (!v || !IS_OBJ(*v) || api_type(L, idx) != LUNA_TUSERDATA) return NULL;
    const FrontendObject *o = feo(L);
    return (o && o->userdata_data) ? o->userdata_data(*v) : NULL;
}

bool api_is_userdata_tag(APIState *L, int idx, const char *tag) {
    Value *v = stack_ptr(L, idx);
    if (!v || !IS_OBJ(*v) || api_type(L, idx) != LUNA_TUSERDATA) return false;
    const FrontendObject *o = feo(L);
    const char *t = (o && o->userdata_tag) ? o->userdata_tag(*v) : NULL;
    return t && tag && strcmp(t, tag) == 0;
}

const char *api_get_userdata_tag(APIState *L, int idx) {
    Value *v = stack_ptr(L, idx);
    if (!v || !IS_OBJ(*v) || api_type(L, idx) != LUNA_TUSERDATA) return NULL;
    const FrontendObject *o = feo(L);
    return (o && o->userdata_tag) ? o->userdata_tag(*v) : NULL;
}

void api_push_lightuserdata(APIState *L, void *ptr) {
    api_new_userdata(L, ptr, "lightuserdata", NULL);
}

void *api_to_lightuserdata(APIState *L, int idx) {
    return api_to_userdata(L, idx);
}

/* ============================================================ */
/* Global access                                                 */
/* ============================================================ */

int api_get_global(APIState *L, const char *name) {
    Value val;
    if (!vm_get_global(L->vm, name, &val)) {
        api_push_nil(L);
        return LUNA_TNIL;
    }
    if (!grow_stack(L, L->top + 1)) return LUNA_TNIL;
    L->stack[L->top++] = val;
    return api_type(L, L->top - 1);
}

void api_set_global(APIState *L, const char *name) {
    if (L->top < 1) return;
    vm_set_global(L->vm, name, L->stack[L->top - 1], false);
    L->top--;
}

void api_set_system_global(APIState *L, const char *name) {
    if (L->top < 1) return;
    vm_set_system_global(L->vm, name, L->stack[L->top - 1]);
    L->top--;
}

int api_get_system_global(APIState *L, const char *name) {
    Value val;
    if (!vm_get_global(L->vm, name, &val)) {
        api_push_nil(L);
        return LUNA_TNIL;
    }
    if (!grow_stack(L, L->top + 1)) return LUNA_TNIL;
    L->stack[L->top++] = val;
    return api_type(L, L->top - 1);
}

/* ============================================================ */
/* Call                                                          */
/* ============================================================ */

api_Status api_pcall(APIState *L, int nargs, int nresults) {
    VM *vm = L->vm;
    int func_idx = L->top - nargs - 1;
    if (func_idx < 0) return LUNA_ERRRUN;

    Value fn_val = L->stack[func_idx];
    Value *args = nargs > 0 ? &L->stack[func_idx + 1] : NULL;
    L->top = func_idx;

    Value result;
    VMResult vm_result = vm_call_value(vm, fn_val, args, nargs, &result);

    if (vm_result == VM_EXCEPTION) {
        char *msg = value_to_string(vm->last_exception);
        if (msg) { api_push_string(L, msg); free(msg); }
        else api_push_string(L, "unknown error");
        return LUNA_ERRRUN;
    }

    if (nresults != 0) {
        grow_stack(L, L->top + 1);
        L->stack[L->top++] = result;
    }

    return LUNA_OK;
}

/* Native C function dispatch — the frontend object model routes a native
 * function call here with the cfunc pointer and argument array. */
Value api_cfunc_dispatch(VM *vm, api_CFunction fn, Value *args, int arg_count) {
    APIState *L = (APIState *)vm->api_state;
    if (!L || !fn) return make_null();

    if (!grow_stack(L, L->top + arg_count)) return make_null();
    for (int i = 0; i < arg_count; i++) L->stack[L->top++] = args[i];

    int nresults = fn(L);
    if (nresults < 0) nresults = 0;
    if (nresults > L->top) nresults = L->top;

    Value result = make_null();
    if (nresults > 0) result = L->stack[L->top - nresults];
    L->top -= (arg_count + nresults);
    return result;
}

/* ============================================================ */
/* Load and run source                                           */
/* ============================================================ */

static api_Status do_load(APIState *L, const char *str, const char *path) {
    VM *vm = L->vm;
    if (!vm->frontend_def || !vm->frontend_def->compile_source) {
        api_push_string(L, "no language frontend installed");
        return LUNA_ERRRUN;
    }
    Value fn_val;
    const char *err = vm->frontend_def->compile_source(vm, str, path, false, &fn_val);
    if (err) {
        api_push_string(L, err);
        return LUNA_ERRSYNTAX;
    }
    if (!grow_stack(L, L->top + 1)) return LUNA_ERRMEM;
    L->stack[L->top++] = fn_val;
    return LUNA_OK;
}

api_Status api_load_string(APIState *L, const char *str) {
    return do_load(L, str, NULL);
}

api_Status api_load_file(APIState *L, const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) { api_push_string(L, "cannot open file"); return LUNA_ERRRUN; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); api_push_string(L, "cannot read file"); return LUNA_ERRRUN; }
    char *buf = (char *)malloc((size_t)(len + 1));
    if (!buf) { fclose(f); return LUNA_ERRMEM; }
    size_t read = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[read] = '\0';
    api_Status s = do_load(L, buf, filename);
    free(buf);
    return s;
}

api_Status api_dostring(APIState *L, const char *str) {
    api_Status s = api_load_string(L, str);
    if (s != LUNA_OK) return s;
    return api_pcall(L, 0, 1);
}

api_Status api_dofile(APIState *L, const char *filename) {
    api_Status s = api_load_file(L, filename);
    if (s != LUNA_OK) return s;
    return api_pcall(L, 0, 1);
}

/* ============================================================ */
/* GC and error handling                                         */
/* ============================================================ */

int api_gc(APIState *L, int what) {
    (void)what;
    mark_and_sweep(L->vm);
    return 0;
}

void api_error(APIState *L, const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    L->vm->last_exception = vm_make_exception(L->vm, VM_EXCEPTION_GENERIC, buf);
    if (L->vm->native_jump) {
        longjmp(L->vm->native_jump->env, 1);
    }
}

double api_checknumber(APIState *L, int arg) {
    if (!api_is_number(L, arg))
        api_error(L, "bad argument #%d (number expected)", arg + (arg < 0 ? L->top + 1 : 1));
    return api_to_number(L, arg);
}

int64_t api_checkinteger(APIState *L, int arg) {
    if (!api_is_integer(L, arg))
        api_error(L, "bad argument #%d (integer expected)", arg + (arg < 0 ? L->top + 1 : 1));
    Value *v = stack_ptr(L, arg);
    int64_t out;
    if (v && IS_INT(*v)) return AS_INT(*v);
    if (v && IS_DOUBLE(*v)) return (int64_t)AS_DOUBLE(*v);
    if (v && integer_of(L, *v, &out)) return out;
    api_error(L, "bad argument #%d (integer too large)", arg + (arg < 0 ? L->top + 1 : 1));
    return 0;
}

const char *api_checkstring(APIState *L, int arg) {
    if (!api_is_string(L, arg))
        api_error(L, "bad argument #%d (string expected)", arg + (arg < 0 ? L->top + 1 : 1));
    return api_to_string(L, arg, NULL);
}

void *api_checkuserdata(APIState *L, int arg, const char *tag) {
    if (!api_is_userdata_tag(L, arg, tag))
        api_error(L, "bad argument #%d (%s expected)", arg + (arg < 0 ? L->top + 1 : 1), tag);
    return api_to_userdata(L, arg);
}