/* luna/object.c — Luna MOP Type instances + vtable implementations.
 *
 * Part 1: Type definitions + py_types[] table.
 * Part 2 (this file): arithmetic / comparison / index vtables wired.
 * The core reaches these through Object.type (Type*) so vm.c never
 * names a concrete Luna kind in dispatch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>

#include "value.h"
#include "py/object.h"
#include "py/frontend_state.h"
#include "chunk.h"
#include "vm.h"
#include "api.h"

/* Forward declarations for helpers used by the lifecycle/formatting vtable
 * (defined later in this file, but referenced by the per-type free funcs). */
static void intern_remove(ObjString *s);
static void userdata_run_finalizer(ObjUserdata *ud);

/* ---- helpers available to vtable functions ---- */

/* ============================================================
 * String operations (vtable)
 * ============================================================ */
static Value py_string_add(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_STRING(a) || !IS_OBJ(b)) { /* string concat with anything -> convert b */
        /* This replicates the current do_arith behavior: L is string,
         * R is converted to string and concatenated. */
        if (!IS_STRING(a)) { return make_null(); }
    }
    /* Current inline logic: L=string, R=any -> concat.
     * We assume a is always string (called from vtable). */
    ObjString *ls = (ObjString*)AS_OBJ(a);
    const char *rs; int rs_len; char *rs_tmp = NULL;
    if (IS_STRING(b)) {
        ObjString *rs_str = (ObjString*)AS_OBJ(b);
        rs = rs_str->chars; rs_len = rs_str->length;
    } else {
        rs_tmp = value_to_string(b);
        rs = rs_tmp; rs_len = (int)strlen(rs_tmp);
    }
    int len = ls->length + rs_len;
    char *buf = malloc(len + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    memcpy(buf, ls->chars, ls->length);
    memcpy(buf + ls->length, rs, rs_len + 1);
    ObjString *s = new_string(buf, len);
    free(rs_tmp); free(buf);
    return make_obj((Object*)s);
}

static Value py_string_mul(struct VM *vm, Value a, Value b) {
    (void)vm;
    /* Replication: string * int OR int * string */
    if (IS_STRING(a) && IS_INT(b)) {
        ObjString *str = (ObjString*)AS_OBJ(a);
        int64_t times = AS_INT(b);
        if (times < 0) { return make_obj((Object*)new_string("", 0)); }
        int len = str->length * (int)times;
        char *buf = malloc(len + 1);
        if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
        buf[0] = '\0';
        for (int t = 0; t < times; t++) memcpy(buf + t * str->length, str->chars, str->length);
        buf[len] = '\0';
        ObjString *res = new_string(buf, len);
        free(buf);
        return make_obj((Object*)res);
    }
    if (IS_STRING(b) && IS_INT(a)) {
        return py_string_mul(vm, b, a);
    }
    return make_null();
}

static int py_string_cmp(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_STRING(a) || !IS_STRING(b)) return 2; /* unsupported -> do_cmp returns null */
    ObjString *as = (ObjString*)AS_OBJ(a);
    ObjString *bs = (ObjString*)AS_OBJ(b);
    if (as == bs) return 0;
    if (as->length != bs->length) return (as->length < bs->length) ? -1 : 1;
    int cmp = memcmp(as->chars, bs->chars, (size_t)as->length);
    return (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
}

static Value py_string_neg(struct VM *vm, Value a) { (void)vm; return make_null(); }
static Value py_string_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_string_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_string_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }

static Value py_string_getitem(struct VM *vm, Value self, Value key) {
    (void)vm;
    if (!IS_STRING(self) || !IS_INT(key)) return make_null();
    ObjString *s = (ObjString*)AS_OBJ(self);
    int idx = (int)AS_INT(key);
    if (idx < 0) idx += s->length;
    if (idx < 0 || idx >= s->length) return make_null();
    return make_obj((Object*)new_string(&s->chars[idx], 1));
}

static void py_string_setitem(struct VM *vm, Value self, Value key, Value val) {
    (void)vm; (void)self; (void)key; (void)val; /* unsupported */
}

static Value py_string_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int py_string_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value py_string_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }

/* ---- MOP call vtables (Part 5) ----
 * Bytecode callables (function/closure/bound_method) run synchronously through
 * vm_call_value, which manages real Luna call frames via a nested execute loop.
 * vm_call_value returns a VMResult (VM_OK == 0 on success), so the check is
 * `!= VM_OK`, not `!`. On failure the exception is already in vm->last_exception;
 * we re-propagate via longjmp exactly like op_throw (goto op_throw), preserving
 * Luna's frame-based exception unwinding instead of swallowing it as null. */
static Value py_function_call(struct VM *vm, Value self, Value *args, int argc) {
    if (!IS_FUNCTION(self)) return make_null();
    ObjFunction *fn = (ObjFunction*)AS_OBJ(self);
    if (fn->is_native) {
        Value scratch[256];
        for (int i = 0; i < argc; i++) scratch[i] = args[i];
        Value result;
        if (fn->cfunc) {
            result = api_cfunc_dispatch(vm, fn->cfunc, scratch, argc);
        } else {
            if (!vm_call_native(vm, fn->native_fn, scratch, argc, &result)) {
                if (vm->native_jump) longjmp(vm->native_jump->env, 1);
                return make_null();
            }
        }
        return result;
    }
    Value out;
    if (vm_call_value(vm, self, args, argc, &out) != VM_OK) {
        if (vm->native_jump) longjmp(vm->native_jump->env, 1);
        return make_null();
    }
    return out;
}

static Value py_closure_call(struct VM *vm, Value self, Value *args, int argc) {
    if (!IS_CLOSURE(self)) return make_null();
    ObjClosure *cl = (ObjClosure *)AS_OBJ(self);
    if (cl->function->is_native) {
        /* Native closure: dispatch like py_function_call.  This mirrors the
         * old inline OP_INVOKE native path (which inspected fn->is_native) and
         * avoids an infinite loop when the core routes a native closure through
         * t->call (get_chunk returns NULL, so vm_call_value would re-enter). */
        Value scratch[256];
        for (int i = 0; i < argc; i++) scratch[i] = args[i];
        Value result;
        if (cl->function->cfunc) return api_cfunc_dispatch(vm, cl->function->cfunc, scratch, argc);
        if (!vm_call_native(vm, cl->function->native_fn, scratch, argc, &result)) {
            if (vm->native_jump) longjmp(vm->native_jump->env, 1);
            return make_null();
        }
        return result;
    }
    Value out;
    if (vm_call_value(vm, self, args, argc, &out) != VM_OK) {
        if (vm->native_jump) longjmp(vm->native_jump->env, 1);
        return make_null();
    }
    return out;
}

static Value py_bound_method_call(struct VM *vm, Value self, Value *args, int argc) {
    if (!IS_BOUND_METHOD(self)) return make_null();
    ObjBoundMethod *bm = (ObjBoundMethod*)AS_OBJ(self);
    Value scratch[256];
    scratch[0] = bm->self; /* implicit self */
    for (int i = 0; i < argc; i++) scratch[i + 1] = args[i];
    Value out;
    if (vm_call_value(vm, make_obj((Object*)bm->fn), scratch, argc + 1, &out) != VM_OK) {
        if (vm->native_jump) longjmp(vm->native_jump->env, 1);
        return make_null();
    }
    return out;
}

/* ============================================================
 * Callable / closure protocol
 *
 * The core (op_call / op_ret / OP_GETUPVAL / OP_SETUPVAL) reaches a callable's
 * bytecode chunk, bound self, display name and upvalue storage purely through
 * these vtable methods — it never names ObjClosure / ObjFunction /
 * ObjBoundMethod / ObjInstance. Native callables return NULL from get_chunk and
 * are dispatched synchronously via `call`.
 * ============================================================ */
static Chunk* py_function_get_chunk(Value self) {
    ObjFunction *fn = (ObjFunction*)AS_OBJ(self);
    return fn->is_native ? NULL : fn->chunk;
}
static Value py_function_get_self(Value self) { (void)self; return make_null(); }
static const char* py_function_name_of(Value self) { return ((ObjFunction*)AS_OBJ(self))->name; }
static int py_function_param_count(Value self) { return ((ObjFunction*)AS_OBJ(self))->param_count; }
static Value py_function_get_param_name(Value self, int i) {
    ObjFunction *fn = (ObjFunction*)AS_OBJ(self);
    if (i < 0 || i >= fn->param_count) return make_null();
    return make_obj((Object*)fn->param_name_objs[i]);
}
static int py_closure_param_count(Value self) { return ((ObjClosure*)AS_OBJ(self))->function->param_count; }
static Value py_closure_get_param_name(Value self, int i) {
    ObjFunction *fn = ((ObjClosure*)AS_OBJ(self))->function;
    if (i < 0 || i >= fn->param_count) return make_null();
    return make_obj((Object*)fn->param_name_objs[i]);
}

static Chunk* py_closure_get_chunk(Value self) {
    ObjClosure *cl = (ObjClosure*)AS_OBJ(self);
    ObjFunction *fn = cl->function;
    return fn->is_native ? NULL : fn->chunk;
}
static Value py_closure_get_self(Value self) { (void)self; return make_null(); }
static const char* py_closure_name_of(Value self) { return ((ObjClosure*)AS_OBJ(self))->function->name; }
static Value py_closure_get_upvalue(struct VM *vm, Value self, int i) {
    ObjClosure *cl = (ObjClosure*)AS_OBJ(self);
    if (i < 0 || i >= cl->upvalue_count) return make_null();
    ObjUpvalue *uv = cl->upvalues[i];
    if (!uv) return make_null();
    return uv->is_open ? vm->stack[uv->stack_index] : uv->closed;
}
static void py_closure_set_upvalue(struct VM *vm, Value self, int i, Value v) {
    ObjClosure *cl = (ObjClosure*)AS_OBJ(self);
    if (i < 0 || i >= cl->upvalue_count) return;
    ObjUpvalue *uv = cl->upvalues[i];
    if (!uv) return;
    if (uv->is_open) vm->stack[uv->stack_index] = v; else uv->closed = v;
}
static Value py_closure_get_upvalue_ref(Value self, int i) {
    ObjClosure *cl = (ObjClosure*)AS_OBJ(self);
    if (i < 0 || i >= cl->upvalue_count) return make_null();
    ObjUpvalue *uv = cl->upvalues[i];
    return uv ? make_obj((Object*)uv) : make_null();
}

static Chunk* py_bound_method_get_chunk(Value self) {
    ObjBoundMethod *bm = (ObjBoundMethod*)AS_OBJ(self);
    return bm->fn->is_native ? NULL : bm->fn->chunk;
}
static Value py_bound_method_get_self(Value self) { return ((ObjBoundMethod*)AS_OBJ(self))->self; }
static const char* py_bound_method_name_of(Value self) { return ((ObjBoundMethod*)AS_OBJ(self))->fn->name; }

static Chunk* py_instance_get_chunk(Value self) {
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    if (!inst->klass) return NULL;
    for (int i = 0; i < inst->klass->method_count; i++)
        if (strcmp(inst->klass->method_names[i], "_call") == 0) {
            ObjFunction *fn = inst->klass->methods[i];
            return fn->is_native ? NULL : fn->chunk;
        }
    return NULL;
}
static Value py_instance_get_self(Value self) { return self; }
static const char* py_instance_name_of(Value self) {
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    return inst->klass ? inst->klass->name : "instance";
}

static bool py_bind_keyword_arguments(struct VM *vm, Value fn_val, uint8_t nargs,
                                         Value kw_names) {
    ObjFunction *fn = NULL;
    int self_skip = 0;
    if (IS_FUNCTION(fn_val)) {
        fn = (ObjFunction*)AS_OBJ(fn_val);
    } else if (IS_CLOSURE(fn_val)) {
        fn = ((ObjClosure*)AS_OBJ(fn_val))->function;
    } else if (IS_BOUND_METHOD(fn_val)) {
        fn = ((ObjBoundMethod*)AS_OBJ(fn_val))->fn;
        self_skip = 1;
    } else {
        return true; /* native / instance callable — core handles it */
    }

    int param_count = fn->param_count;
    int eff_count = param_count - self_skip;
    if (eff_count < 0) eff_count = 0;

    int kw_count = (IS_LIST(kw_names)) ? list_length((ObjList*)AS_OBJ(kw_names)) : 0;
    int positional = (int)nargs - kw_count;
    if (positional < 0) positional = 0;

    /* Fast path: no keywords and every parameter already supplied positionally. */
    if (kw_count == 0 && positional >= eff_count) return true;

    /* Operate on the callee frame's locals (PUSH_FRAME already ran). */
    int base = vm->frames[vm->frame_count - 1].base;
    uint8_t first = (uint8_t)self_skip;

    /* Snapshot kwarg values before any nested default evaluation (which may
       reallocate vm->stack). */
    Value kw_vals[256];
    for (int m = 0; m < kw_count && m < 256; m++)
        kw_vals[m] = vm->stack[base + first + positional + m];

    Value results[256];
    for (int e = 0; e < eff_count && e < 256; e++) {
        if (e < positional) {
            results[e] = vm->stack[base + first + e];
            continue;
        }
        int p = e + self_skip;
        ObjString *want = fn->param_name_objs[p];
        int m = -1;
        for (int k = 0; k < kw_count; k++) {
            Value kv = list_get((ObjList*)AS_OBJ(kw_names), k);
            ObjString *ks = (ObjString*)AS_OBJ(kv);
            if (ks == want) { m = k; break; }
            if (ks && want && ks->length == want->length &&
                memcmp(ks->chars, want->chars, (size_t)ks->length) == 0) { m = k; break; }
        }
        Value val;
        if (m >= 0) {
            val = kw_vals[m];
        } else if (p < fn->default_count && !IS_NIL(fn->defaults[p])) {
            Value out;
            if (vm_call_value(vm, fn->defaults[p], NULL, 0, &out) != VM_OK)
                return false;
            val = out;
        } else {
            val = make_null();
        }
        results[e] = val;
    }

    for (int e = 0; e < eff_count && e < 256; e++)
        vm->stack[base + first + e] = results[e];

    return true;
}

static Value py_string_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t py_string_hash(Value self) { return ((ObjString*)AS_OBJ(self))->hash; }
static int py_string_len(struct VM *vm, Value self) { (void)vm; return utf8_code_point_count(((ObjString*)AS_OBJ(self))->chars, ((ObjString*)AS_OBJ(self))->length); }

/* ============================================================
 * List operations (vtable)
 * ============================================================ */
static Value py_list_add(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_LIST(a) || !IS_LIST(b)) return make_null();
    ObjList *ls = (ObjList*)AS_OBJ(a);
    ObjList *rs = (ObjList*)AS_OBJ(b);
    ObjList *res = new_list(ls->count + rs->count);
    for (int i = 0; i < ls->count; i++) list_add(res, ls->items ? ls->items[i] : ls->inline_items[i]);
    for (int i = 0; i < rs->count; i++) list_add(res, rs->items ? rs->items[i] : rs->inline_items[i]);
    return make_obj((Object*)res);
}

static Value py_list_mul(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (IS_LIST(a) && IS_INT(b)) {
        ObjList *lst = (ObjList*)AS_OBJ(a);
        int64_t times = AS_INT(b);
        if (times < 0) { return make_obj((Object*)new_list(0)); }
        ObjList *res = new_list(lst->count * (int)times);
        for (int t = 0; t < times; t++)
            for (int i = 0; i < lst->count; i++)
                list_add(res, lst->items ? lst->items[i] : lst->inline_items[i]);
        return make_obj((Object*)res);
    }
    if (IS_LIST(b) && IS_INT(a)) {
        return py_list_mul(vm, b, a);
    }
    return make_null();
}

static int py_list_cmp(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_LIST(a) || !IS_LIST(b)) return 2; /* unsupported */
    /* Only exact equality is supported for list comparison (pointer-based in current code) */
    return (a == b) ? 0 : 1;  /* approximate; current behavior uses values_equal for EQ/NE */
    /* Actually current values_equal compares pointers for objects; let's replicate: */
    /* This is handled by the caller (do_cmp) for EQ/NE; cmp vtable only called when needed. */
}

static Value py_list_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_list_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_list_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_list_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }

static Value py_list_getitem(struct VM *vm, Value self, Value key) {
    (void)vm;
    if (!IS_LIST(self) || !IS_INT(key)) return make_null();
    ObjList *lst = (ObjList*)AS_OBJ(self);
    int idx = (int)AS_INT(key);
    int len = lst->count;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return make_null();
    return lst->items ? lst->items[idx] : lst->inline_items[idx];
}

static void py_list_setitem(struct VM *vm, Value self, Value key, Value val) {
    (void)vm;
    if (!IS_LIST(self) || !IS_INT(key)) return;
    ObjList *lst = (ObjList*)AS_OBJ(self);
    int idx = (int)AS_INT(key);
    int len = lst->count;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return; /* out of bounds -> no-op for this vtable */
    if (lst->items) lst->items[idx] = val; else lst->inline_items[idx] = val;
}

static Value py_list_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int py_list_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value py_list_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value py_list_tostring(struct VM *vm, Value self) { (void)vm; return self; }  /* approximate */
static uint32_t py_list_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int py_list_len(struct VM *vm, Value self) { (void)vm; return ((ObjList*)AS_OBJ(self))->count; }

/* ============================================================
 * Tuple operations (vtable) — immutable, fixed-size
 * ============================================================ */
static Value py_tuple_add(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_tuple_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_tuple_mul(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_tuple_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_tuple_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_tuple_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }
static int py_tuple_cmp(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return 2; }

static Value py_tuple_getitem(struct VM *vm, Value self, Value key) {
    (void)vm;
    if (!IS_TUPLE(self) || !IS_INT(key)) return make_null();
    ObjTuple *t = (ObjTuple*)AS_OBJ(self);
    int idx = (int)AS_INT(key);
    int len = t->count;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return make_null();
    return t->items[idx];
}

static void py_tuple_setitem(struct VM *vm, Value self, Value key, Value val) {
    (void)vm; (void)self; (void)key; (void)val;
    /* tuples are immutable */
}

static Value py_tuple_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int py_tuple_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value py_tuple_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value py_tuple_tostring(struct VM *vm, Value self) { (void)vm; return self; }

static uint32_t py_tuple_hash(Value self) {
    ObjTuple *t = (ObjTuple*)AS_OBJ(self);
    if (t->hash != 0) return t->hash;
    uint32_t mult = 1000003u;
    uint32_t x = 0x345678u;
    for (int i = 0; i < t->count; i++) {
        uint32_t y = hash_value(t->items[i]);
        x = (x ^ y) * mult;
        mult += 82520u + (uint32_t)(i * 2);
    }
    x += 97531u;
    if (x == 0) x = 1;
    t->hash = x;
    return x;
}

static int py_tuple_len(struct VM *vm, Value self) { (void)vm; return ((ObjTuple*)AS_OBJ(self))->count; }

/* lifecycle */
static void py_tuple_free(Object *obj) { free(obj); }
static void py_tuple_mark(struct VM *vm, Object *obj) {
    ObjTuple *t = (ObjTuple*)obj;
    for (int i = 0; i < t->count; i++) vm_mark_value(vm, t->items[i]);
}
static char* py_tuple_to_cstr(Value self) {
    ObjTuple *t = (ObjTuple*)AS_OBJ(self);
    int cap = 32; char *out = malloc(cap); int pos = 0;
    out[pos++] = '(';
    for (int i = 0; i < t->count; i++) {
        char *e = value_to_string(t->items[i]);
        bool is_str = IS_STRING(t->items[i]);
        int need = pos + (int)strlen(e) + (is_str ? 2 : 0) + 4;
        if (need >= cap) { cap = need * 2; out = realloc(out, cap); }
        if (i > 0) { out[pos++] = ','; out[pos++] = ' '; }
        if (is_str) out[pos++] = '"';
        int el = (int)strlen(e); memcpy(out + pos, e, el); pos += el;
        if (is_str) out[pos++] = '"';
        free(e);
    }
    if (pos + 2 >= cap) { cap = pos + 4; out = realloc(out, cap); }
    out[pos++] = ')'; out[pos] = '\0';
    char *r = strdup(out); free(out); return r;
}

/* ============================================================
 * Dict operations (vtable)
 * ============================================================ */
static Value py_dict_add(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_dict_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_dict_mul(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_dict_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_dict_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_dict_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }
static int py_dict_cmp(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return 2; }

static Value py_dict_getitem(struct VM *vm, Value self, Value key) {
    (void)vm;
    if (!IS_DICT(self)) return make_null();
    return dict_get((ObjDict*)AS_OBJ(self), key);
}

static void py_dict_setitem(struct VM *vm, Value self, Value key, Value val) {
    (void)vm;
    if (!IS_DICT(self)) return;
    dict_set((ObjDict*)AS_OBJ(self), key, val);
}

static Value py_dict_getattr(struct VM *vm, Value self, const char *name) {
    (void)vm;
    if (!IS_DICT(self)) return make_null();
    return dict_get((ObjDict*)AS_OBJ(self), make_obj((Object*)new_string(name, (int)strlen(name))));
}
static int py_dict_setattr(struct VM *vm, Value self, const char *name, Value val) {
    (void)vm;
    if (!IS_DICT(self)) return 0;
    dict_set((ObjDict*)AS_OBJ(self), make_obj((Object*)new_string(name, (int)strlen(name))), val);
    return 1;
}
static Value py_dict_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value py_dict_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t py_dict_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int py_dict_len(struct VM *vm, Value self) { (void)vm; return ((ObjDict*)AS_OBJ(self))->entry_count; }

/* ============================================================
 * Instance operations (vtable)
 * ============================================================ */
static Value py_instance_add(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_instance_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_instance_mul(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_instance_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_instance_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_instance_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }
static int py_instance_cmp(struct VM *vm, Value a, Value b) { (void)vm; if (!IS_INSTANCE(a) || !IS_INSTANCE(b)) return 2; return (a == b) ? 0 : 1; }

static Value py_instance_getitem(struct VM *vm, Value self, Value key) {
    (void)vm;
    if (!IS_INSTANCE(self) || !IS_STRING(key)) return make_null();
    return instance_get_field((ObjInstance*)AS_OBJ(self), ((ObjString*)AS_OBJ(key))->chars);
}

static void py_instance_setitem(struct VM *vm, Value self, Value key, Value val) {
    (void)vm;
    if (!IS_INSTANCE(self) || !IS_STRING(key)) return;
    instance_set_field((ObjInstance*)AS_OBJ(self), ((ObjString*)AS_OBJ(key))->chars, val);
}

static Value py_instance_getattr(struct VM *vm, Value self, const char *name) {
    (void)vm;
    if (!IS_INSTANCE(self)) return make_null();
    return instance_get_field((ObjInstance*)AS_OBJ(self), name);
}

static int py_instance_setattr(struct VM *vm, Value self, const char *name, Value val) {
    (void)vm;
    if (!IS_INSTANCE(self)) return 0;
    instance_set_field((ObjInstance*)AS_OBJ(self), name, val);
    return 1;
}

static Value py_instance_call(struct VM *vm, Value self, Value *args, int argc) {
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    if (!inst->klass) return make_null();
    ObjFunction *fn = NULL;
    for (int i = 0; i < inst->klass->method_count; i++)
        if (strcmp(inst->klass->method_names[i], "_call") == 0) { fn = inst->klass->methods[i]; break; }
    if (!fn) return make_null();
    /* Bind self as the first positional argument, like op_call's get_self path. */
    Value scratch[256];
    scratch[0] = self;
    for (int i = 0; i < argc; i++) scratch[i + 1] = args[i];
    Value out;
    if (fn->is_native) {
        if (fn->cfunc) return api_cfunc_dispatch(vm, fn->cfunc, scratch, argc + 1);
        if (!vm_call_native(vm, fn->native_fn, scratch, argc + 1, &out)) {
            if (vm->native_jump) longjmp(vm->native_jump->env, 1);
            return make_null();
        }
        return out;
    }
    if (vm_call_value(vm, make_obj((Object*)fn), scratch, argc + 1, &out) != VM_OK) {
        if (vm->native_jump) longjmp(vm->native_jump->env, 1);
        return make_null();
    }
    return out;
}
static Value py_instance_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t py_instance_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int py_instance_len(struct VM *vm, Value self) { (void)vm; (void)self; return 0; }

/* Python-style class instantiation: `Foo(args...)` allocates an instance and
 * runs its `__init__` method (inherited methods included) with self bound as
 * the first argument.  Missing __init__ is a no-op (instance is returned). */
static Value py_class_call(struct VM *vm, Value self, Value *args, int argc) {
    if (!IS_CLASS(self)) return make_null();
    ObjClass *cls = (ObjClass*)AS_OBJ(self);

    /* Builtin type classes are callable as conversions (Python style). */
    if (cls == py_fe(vm)->int_class) {
        if (!argc) return make_int(0);
        Value v = args[0];
        if (IS_INT(v) || IS_BIGINT(v)) return v;
        if (IS_BOOL(v)) return make_int(AS_BOOL(v) ? 1 : 0);
        if (IS_DOUBLE(v)) {
            Value out;
            if (bigint_from_f64(AS_DOUBLE(v), &out)) return out;
            luna_throw(vm, py_fe(vm)->value_error_class,
                isnan(AS_DOUBLE(v)) ? "cannot convert float NaN to integer"
                                    : "cannot convert float infinity to integer");
            return make_int(0);
        }
        if (IS_STRING(v)) {
            ObjString *s = (ObjString *)AS_OBJ(v);
            Value out;
            if (bigint_from_decimal(s->chars, (size_t)s->length, &out)) return out;
            char msg[128];
            snprintf(msg, sizeof(msg), "invalid literal for int() with base 10: '%.64s'", s->chars);
            luna_throw(vm, py_fe(vm)->value_error_class, msg);
            return make_int(0);
        }
        luna_throw(vm, py_fe(vm)->type_error_class,
            "int() argument must be a string, a bytes-like object or a real number");
        return make_int(0);
    }
    if (cls == py_fe(vm)->float_class) {
        if (!argc) return make_double(0.0);
        Value v = args[0];
        if (IS_DOUBLE(v)) return v;
        if (IS_INT(v)) return make_double((double)AS_INT(v));
        if (IS_BIGINT(v)) return make_double(bigint_to_f64((ObjBigInt *)AS_OBJ(v)));
        if (IS_BOOL(v)) return make_double(AS_BOOL(v) ? 1.0 : 0.0);
        if (IS_STRING(v)) {
            char *end = NULL;
            double d = strtod(((ObjString *)AS_OBJ(v))->chars, &end);
            if (end && *end == '\0') return make_double(d);
            luna_throw(vm, py_fe(vm)->value_error_class, "could not convert string to float");
            return make_double(0.0);
        }
        luna_throw(vm, py_fe(vm)->type_error_class, "float() argument must be a string or a number");
        return make_double(0.0);
    }
    if (cls == py_fe(vm)->string_class) {
        if (!argc) return make_obj((Object *)new_string("", 0));
        char *s = value_to_string(args[0]);
        Value v = make_obj((Object *)new_string(s, (int)strlen(s)));
        free(s);
        return v;
    }
    if (cls == py_fe(vm)->tuple_class) {
        if (argc < 1) return make_obj((Object *)new_tuple(0));
        Value v = args[0];
        if (IS_TUPLE(v)) return v;
        if (IS_LIST(v)) {
            ObjList *l = (ObjList *)AS_OBJ(v);
            ObjTuple *t = new_tuple(l->count);
            for (int i = 0; i < l->count; i++)
                t->items[i] = l->items ? l->items[i] : l->inline_items[i];
            return make_obj((Object *)t);
        }
        if (IS_STRING(v)) {
            ObjString *s = (ObjString *)AS_OBJ(v);
            ObjTuple *t = new_tuple(s->length);
            for (int i = 0; i < s->length; i++) {
                char buf[2] = {s->chars[i], '\0'};
                t->items[i] = make_obj((Object *)new_string(buf, 1));
            }
            return make_obj((Object *)t);
        }
        luna_throw(vm, py_fe(vm)->type_error_class, "tuple() argument must be an iterable");
        return make_obj((Object *)new_tuple(0));
    }
    if (cls == py_fe(vm)->list_class) {
        if (argc < 1) return make_obj((Object *)new_list(0));
        Value v = args[0];
        if (IS_LIST(v)) return v;
        if (IS_TUPLE(v)) {
            ObjTuple *t = (ObjTuple *)AS_OBJ(v);
            ObjList *l = new_list(t->count);
            for (int i = 0; i < t->count; i++) list_add(l, t->items[i]);
            return make_obj((Object *)l);
        }
        if (IS_STRING(v)) {
            ObjString *s = (ObjString *)AS_OBJ(v);
            ObjList *l = new_list(s->length);
            for (int i = 0; i < s->length; i++) {
                char buf[2] = {s->chars[i], '\0'};
                list_add(l, make_obj((Object *)new_string(buf, 1)));
            }
            return make_obj((Object *)l);
        }
        luna_throw(vm, py_fe(vm)->type_error_class, "list() argument must be an iterable");
        return make_obj((Object *)new_list(0));
    }

    ObjInstance *inst = new_instance(cls, 4);
    ObjFunction *fn = NULL;
    for (int i = cls->method_count - 1; i >= 0; i--)
        if (cls->methods[i] && strcmp(cls->method_names[i], "__init__") == 0) {
            fn = cls->methods[i];
            break;
        }
    if (fn) {
        Value scratch[256];
        scratch[0] = make_obj((Object*)inst);
        for (int i = 0; i < argc && i < 255; i++) scratch[i + 1] = args[i];
        Value out;
        if (fn->is_native) {
            if (fn->cfunc) return api_cfunc_dispatch(vm, fn->cfunc, scratch, argc + 1);
            if (!vm_call_native(vm, fn->native_fn, scratch, argc + 1, &out)) {
                if (vm->native_jump) longjmp(vm->native_jump->env, 1);
                return make_null();
            }
        } else if (vm_call_value(vm, make_obj((Object*)fn), scratch, argc + 1, &out) != VM_OK) {
            if (vm->native_jump) longjmp(vm->native_jump->env, 1);
            return make_null();
        }
    }
    return make_obj((Object*)inst);
}

/* ============================================================
 * BigInt numeric arithmetic (arbitrary precision, PyLong-style).
 * The core int (int32) path stays immediate; every result is
 * normalized back to int32 when it fits, so the heap bigint only
 * holds values beyond the int32 range.
 * ============================================================ */
static Value py_bigint_add(struct VM *vm, Value a, Value b) { return bigint_binary_value(vm, VM_OP_ADD, a, b); }
static Value py_bigint_sub(struct VM *vm, Value a, Value b) { return bigint_binary_value(vm, VM_OP_SUB, a, b); }
static Value py_bigint_mul(struct VM *vm, Value a, Value b) { return bigint_binary_value(vm, VM_OP_MUL, a, b); }
static Value py_bigint_div(struct VM *vm, Value a, Value b) { return bigint_binary_value(vm, VM_OP_DIV, a, b); }
static Value py_bigint_mod(struct VM *vm, Value a, Value b) { return bigint_binary_value(vm, VM_OP_MOD, a, b); }
static Value py_bigint_neg(struct VM *vm, Value a) {
    if (IS_DOUBLE(a)) return make_double(-AS_DOUBLE(a));
    return bigint_unary_value(vm, VM_OP_NEG, a);
}
static int py_bigint_cmp(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (IS_DOUBLE(a) || IS_DOUBLE(b)) {
        double db = IS_DOUBLE(a) ? AS_DOUBLE(a) : AS_DOUBLE(b);
        if (isnan(db)) return 2;                 /* incomparable */
        return IS_BIGINT(a) ? bigint_cmp_f64((ObjBigInt *)AS_OBJ(a), db)
                            : -bigint_cmp_f64((ObjBigInt *)AS_OBJ(b), db);
    }
    return IS_BIGINT(a) ? bigint_cmp_value((ObjBigInt *)AS_OBJ(a), b)
                        : -bigint_cmp_value((ObjBigInt *)AS_OBJ(b), a);
}
static Value py_bigint_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t py_bigint_hash(Value self) { return bigint_hash((ObjBigInt *)AS_OBJ(self)); }
static int py_bigint_len(struct VM *vm, Value self) { (void)vm; (void)self; return 1; }

/* ============================================================
 * Default stubs for kinds without special arithmetic/index rules
 * ============================================================ */
#define DEFAULT_BIN  (struct VM *vm, Value a, Value b) -> return make_null()
static Value py_default_add(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_default_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_default_mul(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_default_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_default_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value py_default_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }
static int py_default_cmp(struct VM *vm, Value a, Value b) { (void)vm; return (a == b) ? 0 : 1; }
static Value py_default_getitem(struct VM *vm, Value self, Value key) { (void)vm; (void)self; (void)key; return make_null(); }
static void py_default_setitem(struct VM *vm, Value self, Value key, Value val) { (void)vm; (void)self; (void)key; (void)val; }
static Value py_default_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int py_default_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value py_default_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value py_default_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t py_default_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int py_default_len(struct VM *vm, Value self) { (void)vm; return 0; }

/* ============================================================
 * Type instance definitions (vtable filled)
 * ============================================================ */
Type py_string_type  = {
    .name = "str", .kind = OBJ_STRING,
    .add = py_string_add, .sub = py_string_sub, .mul = py_string_mul, .div = py_string_div, .mod = py_string_mod,
    .neg = py_string_neg, .cmp = py_string_cmp,
    .getitem = py_string_getitem, .setitem = py_string_setitem,
    .getattr = py_string_getattr, .setattr = py_string_setattr,
    .call = py_string_call, .tostring = py_string_tostring, .hash = py_string_hash, .len = py_string_len
};

Type py_list_type = {
    .name = "list", .kind = OBJ_LIST,
    .add = py_list_add, .sub = py_list_sub, .mul = py_list_mul, .div = py_list_div, .mod = py_list_mod,
    .neg = py_list_neg, .cmp = py_list_cmp,
    .getitem = py_list_getitem, .setitem = py_list_setitem,
    .getattr = py_list_getattr, .setattr = py_list_setattr,
    .call = py_list_call, .tostring = py_list_tostring, .hash = py_list_hash, .len = py_list_len
};

Type py_tuple_type = {
    .name = "tuple", .kind = OBJ_TUPLE,
    .add = py_tuple_add, .sub = py_tuple_sub, .mul = py_tuple_mul, .div = py_tuple_div, .mod = py_tuple_mod,
    .neg = py_tuple_neg, .cmp = py_tuple_cmp,
    .getitem = py_tuple_getitem, .setitem = py_tuple_setitem,
    .getattr = py_tuple_getattr, .setattr = py_tuple_setattr,
    .call = py_tuple_call, .tostring = py_tuple_tostring, .hash = py_tuple_hash, .len = py_tuple_len
};

Type py_dict_type = {
    .name = "dict", .kind = OBJ_DICT,
    .add = py_dict_add, .sub = py_dict_sub, .mul = py_dict_mul, .div = py_dict_div, .mod = py_dict_mod,
    .neg = py_dict_neg, .cmp = py_dict_cmp,
    .getitem = py_dict_getitem, .setitem = py_dict_setitem,
    .getattr = py_dict_getattr, .setattr = py_dict_setattr,
    .call = py_dict_call, .tostring = py_dict_tostring, .hash = py_dict_hash, .len = py_dict_len
};

Type py_instance_type = {
    .name = "instance", .kind = OBJ_INSTANCE,
    .add = py_instance_add, .sub = py_instance_sub, .mul = py_instance_mul, .div = py_instance_div, .mod = py_instance_mod,
    .neg = py_instance_neg, .cmp = py_instance_cmp,
    .getitem = py_instance_getitem, .setitem = py_instance_setitem,
    .getattr = py_instance_getattr, .setattr = py_instance_setattr,
    .call = py_instance_call, .tostring = py_instance_tostring, .hash = py_instance_hash, .len = py_instance_len,
    .get_chunk = py_instance_get_chunk, .get_self = py_instance_get_self, .name_of = py_instance_name_of
};

Type py_function_type = {
    .name = "function", .kind = OBJ_FUNCTION,
    .add = py_default_add, .sub = py_default_sub, .mul = py_default_mul, .div = py_default_div, .mod = py_default_mod,
    .neg = py_default_neg, .cmp = py_default_cmp,
    .getitem = py_default_getitem, .setitem = py_default_setitem,
    .getattr = py_default_getattr, .setattr = py_default_setattr,
    .call = py_function_call, .tostring = py_default_tostring, .hash = py_default_hash, .len = py_default_len,
    .get_chunk = py_function_get_chunk, .get_self = py_function_get_self, .name_of = py_function_name_of,
    .param_count = py_function_param_count, .get_param_name = py_function_get_param_name,
    .bind_keyword_arguments = py_bind_keyword_arguments
};

Type py_closure_type = {
    .name = "closure", .kind = OBJ_CLOSURE,
    .add = py_default_add, .sub = py_default_sub, .mul = py_default_mul, .div = py_default_div, .mod = py_default_mod,
    .neg = py_default_neg, .cmp = py_default_cmp,
    .getitem = py_default_getitem, .setitem = py_default_setitem,
    .getattr = py_default_getattr, .setattr = py_default_setattr,
    .call = py_closure_call, .tostring = py_default_tostring, .hash = py_default_hash, .len = py_default_len,
    .get_chunk = py_closure_get_chunk, .get_self = py_closure_get_self, .name_of = py_closure_name_of,
    .get_upvalue = py_closure_get_upvalue, .set_upvalue = py_closure_set_upvalue, .get_upvalue_ref = py_closure_get_upvalue_ref,
    .param_count = py_closure_param_count, .get_param_name = py_closure_get_param_name,
    .bind_keyword_arguments = py_bind_keyword_arguments
};

Type py_upvalue_type = {
    .name = "upvalue", .kind = OBJ_UPVALUE,
    .add = py_default_add, .sub = py_default_sub, .mul = py_default_mul, .div = py_default_div, .mod = py_default_mod,
    .neg = py_default_neg, .cmp = py_default_cmp,
    .getitem = py_default_getitem, .setitem = py_default_setitem,
    .getattr = py_default_getattr, .setattr = py_default_setattr,
    .call = py_default_call, .tostring = py_default_tostring, .hash = py_default_hash, .len = py_default_len
};

Type py_enum_type = {
    .name = "enum", .kind = OBJ_ENUM,
    .add = py_default_add, .sub = py_default_sub, .mul = py_default_mul, .div = py_default_div, .mod = py_default_mod,
    .neg = py_default_neg, .cmp = py_default_cmp,
    .getitem = py_default_getitem, .setitem = py_default_setitem,
    .getattr = py_default_getattr, .setattr = py_default_setattr,
    .call = py_default_call, .tostring = py_default_tostring, .hash = py_default_hash, .len = py_default_len
};

Type py_class_type = {
    .name = "class", .kind = OBJ_CLASS,
    .add = py_default_add, .sub = py_default_sub, .mul = py_default_mul, .div = py_default_div, .mod = py_default_mod,
    .neg = py_default_neg, .cmp = py_default_cmp,
    .getitem = py_default_getitem, .setitem = py_default_setitem,
    .getattr = py_default_getattr, .setattr = py_default_setattr,
    .call = py_class_call, .tostring = py_default_tostring, .hash = py_default_hash, .len = py_default_len
};

Type py_bound_method_type = {
    .name = "bound_method", .kind = OBJ_BOUND_METHOD,
    .add = py_default_add, .sub = py_default_sub, .mul = py_default_mul, .div = py_default_div, .mod = py_default_mod,
    .neg = py_default_neg, .cmp = py_default_cmp,
    .getitem = py_default_getitem, .setitem = py_default_setitem,
    .getattr = py_default_getattr, .setattr = py_default_setattr,
    .call = py_bound_method_call, .tostring = py_default_tostring, .hash = py_default_hash, .len = py_default_len,
    .get_chunk = py_bound_method_get_chunk, .get_self = py_bound_method_get_self, .name_of = py_bound_method_name_of,
    .bind_keyword_arguments = py_bind_keyword_arguments
};

Type py_module_type = {
    .name = "module", .kind = OBJ_MODULE,
    .add = py_default_add, .sub = py_default_sub, .mul = py_default_mul, .div = py_default_div, .mod = py_default_mod,
    .neg = py_default_neg, .cmp = py_default_cmp,
    .getitem = py_default_getitem, .setitem = py_default_setitem,
    .getattr = py_default_getattr, .setattr = py_default_setattr,
    .call = py_default_call, .tostring = py_default_tostring, .hash = py_default_hash, .len = py_default_len
};

Type py_buffer_type = {
    .name = "buffer", .kind = OBJ_BUFFER,
    .add = py_default_add, .sub = py_default_sub, .mul = py_default_mul, .div = py_default_div, .mod = py_default_mod,
    .neg = py_default_neg, .cmp = py_default_cmp,
    .getitem = py_default_getitem, .setitem = py_default_setitem,
    .getattr = py_default_getattr, .setattr = py_default_setattr,
    .call = py_default_call, .tostring = py_default_tostring, .hash = py_default_hash, .len = py_default_len
};

Type py_bigint_type = {
    .name = "int", .kind = OBJ_BIGINT,
    .add = py_bigint_add, .sub = py_bigint_sub, .mul = py_bigint_mul, .div = py_bigint_div, .mod = py_bigint_mod,
    .neg = py_bigint_neg, .cmp = py_bigint_cmp,
    .getitem = py_default_getitem, .setitem = py_default_setitem,
    .getattr = py_default_getattr, .setattr = py_default_setattr,
    .call = py_default_call, .tostring = py_bigint_tostring, .hash = py_bigint_hash, .len = py_bigint_len
};

Type py_userdata_type = {
    .name = "userdata", .kind = OBJ_USERDATA,
    .add = py_default_add, .sub = py_default_sub, .mul = py_default_mul, .div = py_default_div, .mod = py_default_mod,
    .neg = py_default_neg, .cmp = py_default_cmp,
    .getitem = py_default_getitem, .setitem = py_default_setitem,
    .getattr = py_default_getattr, .setattr = py_default_setattr,
    .call = py_default_call, .tostring = py_default_tostring, .hash = py_default_hash, .len = py_default_len
};

/* Indexed by ObjType. */
Type *py_types[] = {
    [OBJ_STRING]       = &py_string_type,
    [OBJ_LIST]         = &py_list_type,
    [OBJ_TUPLE]        = &py_tuple_type,
    [OBJ_DICT]         = &py_dict_type,
    [OBJ_INSTANCE]     = &py_instance_type,
    [OBJ_FUNCTION]     = &py_function_type,
    [OBJ_UPVALUE]      = &py_upvalue_type,
    [OBJ_CLOSURE]      = &py_closure_type,
    [OBJ_ENUM]         = &py_enum_type,
    [OBJ_CLASS]        = &py_class_type,
    [OBJ_BOUND_METHOD] = &py_bound_method_type,
    [OBJ_MODULE]       = &py_module_type,
    [OBJ_BUFFER]       = &py_buffer_type,
    [OBJ_BIGINT]       = &py_bigint_type,
    [OBJ_USERDATA]     = &py_userdata_type,
};

/* ============================================================
 * Lifecycle / formatting vtable
 *
 * The core reaches GC mark, free, equality, and string formatting through
 * these vtable methods instead of switching on ObjType. The frontend owns
 * the kind switch here (py_wire_lifecycle), keeping the core generic.
 * ============================================================ */

/* defaults — shared by types with no extra resources and no child references */
static void py_default_free(Object *obj) { free(obj); }
static void py_default_mark(struct VM *vm, Object *obj) { (void)vm; (void)obj; }
static bool py_default_eq(Value a, Value b) { return a == b; }
static char* py_generic_to_cstr(Value self) {
    Object *o = AS_OBJ(self);
    char buf[32];
    snprintf(buf, sizeof(buf), "<%s>", o->type->name);
    return strdup(buf);
}

/* string */
static void py_string_free(Object *obj) {
    ObjString *s = (ObjString*)obj;
    intern_remove(s);
    free(s->chars);
    free(s);
}
static char* py_string_to_cstr(Value self) {
    return strdup(((ObjString*)AS_OBJ(self))->chars);
}
static const char* py_string_chars(Value self) {
    return ((ObjString*)AS_OBJ(self))->chars;
}

/* list */
static void py_list_free(Object *obj) {
    ObjList *l = (ObjList*)obj;
    if (l->items) free(l->items);
    free(l);
}
static void py_list_mark(struct VM *vm, Object *obj) {
    ObjList *l = (ObjList*)obj;
    if (l->items) {
        for (int i = 0; i < l->count; i++) vm_mark_value(vm, l->items[i]);
    } else {
        for (int i = 0; i < l->count; i++) vm_mark_value(vm, l->inline_items[i]);
    }
}
static char* py_list_to_cstr(Value self) {
    ObjList *l = (ObjList*)AS_OBJ(self);
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

/* dict */
static void py_dict_free(Object *obj) {
    ObjDict *d = (ObjDict*)obj;
    if (d->entries) { free(d->entries); free(d->order); }
    free(d);
}
static void py_dict_mark(struct VM *vm, Object *obj) {
    ObjDict *d = (ObjDict*)obj;
    if (d->entries == NULL) {
        for (int i = 0; i < d->entry_count; i++) {
            vm_mark_value(vm, d->inline_entries[i].key);
            vm_mark_value(vm, d->inline_entries[i].value);
        }
    } else {
        for (int i = 0; i < d->capacity; i++) {
            if (d->entries[i].key != EMPTY_VAL && d->entries[i].key != TOMBSTONE_VAL) {
                vm_mark_value(vm, d->entries[i].key);
                vm_mark_value(vm, d->entries[i].value);
            }
        }
    }
}
static char* py_dict_to_cstr(Value self) {
    ObjDict *d = (ObjDict*)AS_OBJ(self);
    int cap = 32; char *out = malloc(cap); int pos = 0; bool first = true;
    out[pos++] = '{';
    if (d->entries == NULL) {
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
        for (int i = 0; i < d->order_count; i++) {
            int idx = d->order[i];
            if (d->entries[idx].key == EMPTY_VAL || d->entries[idx].key == TOMBSTONE_VAL) continue;
            ObjDictEntry *e = &d->entries[idx];
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

/* instance */
static void py_instance_free(Object *obj) {
    ObjInstance *inst = (ObjInstance*)obj;
    if (inst->class_name) free(inst->class_name);
    for (int i = 0; i < inst->field_count; i++) {
        if (inst->field_names[i]) free(inst->field_names[i]);
    }
    free(inst->field_names); free(inst->fields); free(inst);
}
static void py_instance_mark(struct VM *vm, Object *obj) {
    ObjInstance *inst = (ObjInstance*)obj;
    for (int i = 0; i < inst->field_count; i++) vm_mark_value(vm, inst->fields[i]);
    if (inst->klass) vm_mark_value(vm, make_obj((Object*)inst->klass));
}
static char* py_instance_to_cstr(Value self) {
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    Value msgv = instance_get_field(inst, "message");
    char buf[64];
    if (IS_STRING(msgv)) {
        return strdup(((ObjString*)AS_OBJ(msgv))->chars);
    } else {
        snprintf(buf, sizeof(buf), "<instance of %s>", inst->class_name);
        return strdup(buf);
    }
}
static const char* py_instance_message(struct VM *vm, Value self) {
    (void)vm;
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    Value msgv = instance_get_field(inst, "message");
    if (IS_STRING(msgv)) return ((ObjString*)AS_OBJ(msgv))->chars;
    return NULL;
}
static const char* py_instance_class_name(Value self) {
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    return inst->class_name ? inst->class_name : "Error";
}

/* function */
static void py_function_free(Object *obj) {
    ObjFunction *f = (ObjFunction*)obj;
    free(f->name);
    if (f->param_names) {
        for (int i = 0; i < f->param_count; i++) free(f->param_names[i]);
        free(f->param_names);
    }
    free(f->param_name_objs);
    if (f->chunk) { chunk_free(f->chunk); free(f->chunk); }
    free(f->upvalue_descriptors);
    /* Default thunks are GC-managed objects referenced only by fn->defaults;
       the GC frees them.  We only release the index/value array here. */
    free(f->defaults);
    free(f);
}
static void py_function_mark(struct VM *vm, Object *obj) {
    ObjFunction *f = (ObjFunction*)obj;
    if (f->chunk) {
        for (int i = 0; i < f->chunk->const_count; i++) vm_mark_value(vm, f->chunk->constants[i]);
    }
    if (f->defaults) {
        for (int i = 0; i < f->default_count; i++) vm_mark_value(vm, f->defaults[i]);
    }
}
static char* py_function_to_cstr(Value self) {
    ObjFunction *f = (ObjFunction*)AS_OBJ(self);
    char buf[64];
    snprintf(buf, sizeof(buf), "<%s %s>", f->is_native ? "native fn" : "fn", f->name ? f->name : "?");
    return strdup(buf);
}

/* upvalue */
static void py_upvalue_mark(struct VM *vm, Object *obj) {
    ObjUpvalue *uv = (ObjUpvalue*)obj;
    vm_mark_value(vm, uv->closed);
}

/* closure */
static void py_closure_free(Object *obj) {
    ObjClosure *cl = (ObjClosure*)obj;
    free(cl->upvalues);
    free(cl);
}
static void py_closure_mark(struct VM *vm, Object *obj) {
    ObjClosure *cl = (ObjClosure*)obj;
    if (cl->function) vm_mark_value(vm, make_obj((Object*)cl->function));
    for (int i = 0; i < cl->upvalue_count; i++) {
        if (cl->upvalues[i]) vm_mark_value(vm, make_obj((Object*)cl->upvalues[i]));
    }
}

/* enum */
static void py_enum_free(Object *obj) {
    ObjEnum *e = (ObjEnum*)obj;
    free(e->name);
    for (int i = 0; i < e->count; i++) free(e->names[i]);
    free(e->names);
    free(e->values);
    free(e);
}
static char* py_enum_to_cstr(Value self) {
    ObjEnum *e = (ObjEnum*)AS_OBJ(self);
    char *out = malloc(64);
    int n = snprintf(out, 64, "<enum %s (%d variants)>", e->name, e->count);
    if (n >= 64) { out = realloc(out, (size_t)n + 1); snprintf(out, (size_t)n + 1, "<enum %s (%d variants)>", e->name, e->count); }
    char *r = strdup(out); free(out); return r;
}

/* class */
static void py_class_free(Object *obj) {
    ObjClass *cls = (ObjClass*)obj;
    free(cls->name);
    if (cls->method_names) {
        for (int i = 0; i < cls->method_count; i++)
            if (cls->method_names[i]) free(cls->method_names[i]);
        free(cls->method_names);
    }
    free(cls->methods);
    free(cls);
}
static void py_class_mark(struct VM *vm, Object *obj) {
    ObjClass *cls = (ObjClass*)obj;
    if (cls->base) vm_mark_value(vm, make_obj((Object*)cls->base));
    if (cls->prototype) vm_mark_value(vm, make_obj((Object*)cls->prototype));
    for (int i = 0; i < cls->method_count; i++) {
        if (cls->methods[i]) vm_mark_value(vm, make_obj((Object*)cls->methods[i]));
    }
    if (cls->fields) vm_mark_value(vm, make_obj((Object*)cls->fields));
}

/* bound_method */
static void py_bound_method_mark(struct VM *vm, Object *obj) {
    ObjBoundMethod *bm = (ObjBoundMethod*)obj;
    vm_mark_value(vm, bm->self);
    if (bm->fn) vm_mark_value(vm, make_obj((Object*)bm->fn));
}
static char* py_bound_method_to_cstr(Value self) {
    ObjBoundMethod *bm = (ObjBoundMethod*)AS_OBJ(self);
    ObjFunction *f = bm->fn;
    char buf[64];
    snprintf(buf, sizeof(buf), "<bound method %s>", f && f->name ? f->name : "?");
    return strdup(buf);
}

/* module */
static void py_module_mark(struct VM *vm, Object *obj) {
    ObjModule *mod = (ObjModule*)obj;
    if (mod->name) vm_mark_value(vm, make_obj((Object*)mod->name));
    if (mod->exports) vm_mark_value(vm, make_obj((Object*)mod->exports));
}
static char* py_module_to_cstr(Value self) {
    ObjModule *mod = (ObjModule*)AS_OBJ(self);
    char buf[64];
    snprintf(buf, sizeof(buf), "<module %s>", mod->name ? mod->name->chars : "?");
    return strdup(buf);
}

/* buffer */
static void py_buffer_free(Object *obj) {
    ObjBuffer *bufv = (ObjBuffer*)obj;
    free(bufv->data);
    free(bufv);
}
static char* py_buffer_to_cstr(Value self) {
    ObjBuffer *bufv = (ObjBuffer*)AS_OBJ(self);
    char out[64];
    snprintf(out, sizeof(out), "<buffer %zu bytes>", bufv->size);
    return strdup(out);
}

/* userdata */
static void py_userdata_free(Object *obj) {
    ObjUserdata *ud = (ObjUserdata*)obj;
    userdata_run_finalizer(ud);
    free(ud->tag);
    free(ud);
}

/* One-time wiring: the frontend owns this kind switch; the core stays generic. */
void py_wire_lifecycle(void) {
    for (int k = 0; k <= OBJ_USERDATA; k++) {
        Type *t = py_types[k];
        /* ObjType intentionally has reserved values; there is no Type for
         * those slots in py_types[]. */
        if (!t) continue;
        if (!t->free) t->free = py_default_free;
        if (!t->mark) t->mark = py_default_mark;
        if (!t->eq) t->eq = py_default_eq;
        if (!t->to_cstr) t->to_cstr = py_generic_to_cstr;
        switch (k) {
            case OBJ_STRING:
                t->free = py_string_free; t->to_cstr = py_string_to_cstr;
                t->string_chars = py_string_chars; break;
            case OBJ_LIST:
                t->free = py_list_free; t->mark = py_list_mark; t->to_cstr = py_list_to_cstr; break;
            case OBJ_TUPLE:
                t->free = py_tuple_free; t->mark = py_tuple_mark; t->to_cstr = py_tuple_to_cstr; break;
            case OBJ_DICT:
                t->free = py_dict_free; t->mark = py_dict_mark; t->to_cstr = py_dict_to_cstr; break;
            case OBJ_INSTANCE:
                t->free = py_instance_free; t->mark = py_instance_mark; t->to_cstr = py_instance_to_cstr;
                t->message = py_instance_message; t->class_name = py_instance_class_name; break;
            case OBJ_FUNCTION:
                t->free = py_function_free; t->mark = py_function_mark; t->to_cstr = py_function_to_cstr; break;
            case OBJ_UPVALUE:
                t->mark = py_upvalue_mark; break;
            case OBJ_CLOSURE:
                t->free = py_closure_free; t->mark = py_closure_mark; break;
            case OBJ_ENUM:
                t->free = py_enum_free; t->to_cstr = py_enum_to_cstr; break;
            case OBJ_CLASS:
                t->free = py_class_free; t->mark = py_class_mark; break;
            case OBJ_BOUND_METHOD:
                t->mark = py_bound_method_mark; t->to_cstr = py_bound_method_to_cstr; break;
            case OBJ_MODULE:
                t->mark = py_module_mark; t->to_cstr = py_module_to_cstr; break;
            case OBJ_BUFFER:
                t->free = py_buffer_free; t->to_cstr = py_buffer_to_cstr; break;
            case OBJ_USERDATA:
                t->free = py_userdata_free; break;
            default: break;
        }
    }
}
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
    if (IS_BIGINT(v)) return bigint_hash((ObjBigInt *)AS_OBJ(v));
    if (IS_DOUBLE(v)) {
        uint64_t u;
        memcpy(&u, &v, sizeof(u));
        return (uint32_t)(u ^ (u >> 32));
    }
    if (IS_OBJ(v)) {
        Object *obj = AS_OBJ(v);
        if (!obj) return 0;
        if (obj->type->kind == OBJ_STRING)
            return ((ObjString *)obj)->hash;
        if (obj->type->kind == OBJ_USERDATA)
            return (uint32_t)(uintptr_t)((ObjUserdata*)obj)->data;
        return (uint32_t)(uintptr_t)obj;
    }
    return 0;
}

/* ============================================================ */
/* String interning (dynamically resizing hash table)             */
/* ============================================================ */

#define INTERN_INITIAL_CAPACITY 64
#define INTERN_GROWTH_FACTOR 2
#define INTERN_LOAD_LIMIT 0.75

typedef struct StringInternEntry {
    struct StringInternEntry *next;
    uint32_t hash;
    int length;
    ObjString *string;
} StringInternEntry;

static StringInternEntry **intern_table = NULL;
static int intern_capacity = 0;
static int intern_count = 0;
static int intern_threshold = 0;

static void intern_resize(void) {
    int new_cap = intern_capacity * INTERN_GROWTH_FACTOR;
    StringInternEntry **new_table = calloc(new_cap, sizeof(StringInternEntry*));

    for (int i = 0; i < intern_capacity; i++) {
        StringInternEntry *e = intern_table[i];
        while (e) {
            StringInternEntry *next = e->next;
            uint32_t bucket = e->hash & (new_cap - 1);
            e->next = new_table[bucket];
            new_table[bucket] = e;
            e = next;
        }
    }
    free(intern_table);
    intern_table = new_table;
    intern_capacity = new_cap;
    intern_threshold = (int)(new_cap * INTERN_LOAD_LIMIT);
}

static void intern_add(ObjString *s) {
    if (!intern_table) {
        intern_capacity = INTERN_INITIAL_CAPACITY;
        intern_table = calloc(intern_capacity, sizeof(StringInternEntry*));
        intern_threshold = (int)(intern_capacity * INTERN_LOAD_LIMIT);
    } else if (intern_count >= intern_threshold) {
        intern_resize();
    }

    uint32_t bucket = s->hash & (intern_capacity - 1);
    StringInternEntry *e = malloc(sizeof(StringInternEntry));
    if (!e) { fprintf(stderr, "OOM\n"); exit(1); }
    e->hash = s->hash;
    e->length = s->length;
    e->string = s;
    e->next = intern_table[bucket];
    intern_table[bucket] = e;
    intern_count++;
}

static ObjString *intern_find(const char *chars, int length, uint32_t hash) {
    if (!intern_table) return NULL;
    uint32_t bucket = hash & (intern_capacity - 1);
    for (StringInternEntry *e = intern_table[bucket]; e; e = e->next) {
        if (e->hash == hash && e->length == length &&
            memcmp(e->string->chars, chars, length) == 0) {
            return e->string;
        }
    }
    return NULL;
}

static void intern_remove(ObjString *s) {
    if (!intern_table) return;
    uint32_t bucket = s->hash & (intern_capacity - 1);
    StringInternEntry **current = &intern_table[bucket];
    while (*current) {
        if ((*current)->string == s) {
            StringInternEntry *to_free = *current;
            *current = (*current)->next;
            free(to_free);
            intern_count--;
            return;
        }
        current = &(*current)->next;
    }
}

void value_free_intern_table(void) {
    if (!intern_table) return;
    for (int i = 0; i < intern_capacity; i++) {
        StringInternEntry *e = intern_table[i];
        while (e) {
            StringInternEntry *next = e->next;
            free(e);
            e = next;
        }
        intern_table[i] = NULL;
    }
    free(intern_table);
    intern_table = NULL;
    intern_capacity = 0;
    intern_count = 0;
    intern_threshold = 0;
}

/* ============================================================ */
/* Object constructors                                           */
/* ============================================================ */



void py_init_object(Object *obj, ObjType type, size_t size) {
    obj->type = py_types[type];
    obj->gc_color = GC_COLOR_WHITE;
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
    py_init_object((Object*)s, OBJ_STRING, sizeof(ObjString) + length + 1);
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
    py_init_object((Object*)l, OBJ_LIST, sizeof(ObjList) + (capacity > 4 ? capacity * sizeof(Value) : 0));
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

ObjTuple *new_tuple(int count) {
    size_t size = sizeof(ObjTuple) + (size_t)count * sizeof(Value);
    ObjTuple *t = malloc(size);
    if (!t) { fprintf(stderr, "OOM\n"); exit(1); }
    py_init_object((Object*)t, OBJ_TUPLE, size);
    t->count = count;
    t->hash = 0;
    for (int i = 0; i < count; i++) t->items[i] = make_null();
    return t;
}

ObjDict *new_dict(void) {
    ObjDict *d = malloc(sizeof(ObjDict));
    if (!d) { fprintf(stderr, "OOM\n"); exit(1); }
    py_init_object((Object*)d, OBJ_DICT, sizeof(ObjDict));
    d->entries = NULL;
    d->order = NULL;
    d->capacity = 0;
    d->entry_count = 0;
    d->order_count = 0;
    d->order_capacity = 0;
    d->tombstone_count = 0;
    return d;
}

/* Transition an SOO dict (inline_entries) to open-addressing heap layout */
static void dict_transition_to_heap(ObjDict *d) {
    ObjDictEntry old[4];
    int old_count = d->entry_count;
    memcpy(old, d->inline_entries, sizeof(ObjDictEntry) * old_count);

    d->capacity = 8;
    d->entries = calloc(d->capacity, sizeof(ObjDictEntry));
    if (!d->entries) { fprintf(stderr, "OOM\n"); exit(1); }
    for (int i = 0; i < d->capacity; i++) d->entries[i].key = EMPTY_VAL;

    d->order = malloc(8 * sizeof(int));
    if (!d->order) { fprintf(stderr, "OOM\n"); exit(1); }
    d->order_capacity = 8;

    d->entry_count = 0;
    d->order_count = 0;
    d->tombstone_count = 0;

    for (int i = 0; i < old_count; i++) {
        dict_set(d, old[i].key, old[i].value);
    }
}

ObjClass *new_class(const char *name, const char *base_name) {
    (void)base_name;
    ObjClass *cls = malloc(sizeof(ObjClass));
    if (!cls) { fprintf(stderr, "OOM\n"); exit(1); }
    py_init_object((Object*)cls, OBJ_CLASS, sizeof(ObjClass));
    cls->name = strdup(name);
    cls->base = NULL;
    cls->prototype = NULL;
    cls->methods = NULL;
    cls->method_names = NULL;
    cls->method_count = 0;
    cls->method_capacity = 0;
    cls->fields = new_dict();
    cls->field_slot_map = NULL;
    cls->field_count = 0;
    return cls;
}

ObjInstance *new_instance(ObjClass *klass, int cap) {
    ObjInstance *inst = malloc(sizeof(ObjInstance));
    if (!inst) { fprintf(stderr, "OOM\n"); exit(1); }
    py_init_object((Object*)inst, OBJ_INSTANCE, sizeof(ObjInstance) + sizeof(Value) * cap);
    inst->class_name    = strdup(klass->name);
    inst->klass         = klass;
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
            inst->field_count++;
        }
    }
    return inst;
}

ObjFunction *new_function(const char *name) {
    ObjFunction *f = malloc(sizeof(ObjFunction));
    if (!f) { fprintf(stderr, "OOM\n"); exit(1); }
    py_init_object((Object*)f, OBJ_FUNCTION, sizeof(ObjFunction));
    f->name        = strdup(name ? name : "<fn>");
    f->chunk       = NULL;
    f->param_names = NULL;
    f->param_name_objs = NULL;
    f->param_count = 0;
    f->upvalue_count = 0;
    f->upvalue_descriptors = NULL;
    f->is_native = false;
    f->is_leaf = false;
    f->native_fn = NULL;
    f->cfunc = NULL; /* MUST be NULL for native functions: malloc() garbage can match a non-null dispatch check in vm_opcodes.inc, causing silent failures */
    f->params = NULL;
    f->body = NULL;
    f->body_count = 0;
    f->closure = NULL;
    f->defaults = NULL;
    f->default_count = 0;
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
    py_init_object((Object*)ud, OBJ_USERDATA, sizeof(ObjUserdata));
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
    py_init_object((Object*)uv, OBJ_UPVALUE, sizeof(ObjUpvalue));
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
    py_init_object((Object*)cl, OBJ_CLOSURE, sizeof(ObjClosure) + sizeof(ObjUpvalue*) * function->upvalue_count);
    cl->function = function;
    cl->upvalue_count = function->upvalue_count;
    cl->upvalues = cl->upvalue_count > 0
        ? calloc(cl->upvalue_count, sizeof(ObjUpvalue*))
        : NULL;
    return cl;
}

ObjBoundMethod *new_bound_method(Value self, ObjFunction *fn) {
    ObjBoundMethod *bm = malloc(sizeof(ObjBoundMethod));
    if (!bm) { fprintf(stderr, "OOM\n"); exit(1); }
    py_init_object((Object*)bm, OBJ_BOUND_METHOD, sizeof(ObjBoundMethod));
    bm->self = self;
    bm->fn = fn;
    return bm;
}

ObjEnum *new_enum(const char *name, int count) {
    ObjEnum *e = malloc(sizeof(ObjEnum));
    if (!e) { fprintf(stderr, "OOM\n"); exit(1); }
    py_init_object((Object*)e, OBJ_ENUM, sizeof(ObjEnum));
    e->name   = strdup(name);
    e->count  = count;
    e->names  = count > 0 ? calloc(count, sizeof(char*)) : NULL;
    e->values = count > 0 ? calloc(count, sizeof(int64_t)) : NULL;
    return e;
}

ObjModule *new_module(const char *name) {
    ObjModule *mod = malloc(sizeof(ObjModule));
    if (!mod) { fprintf(stderr, "OOM\n"); exit(1); }
    py_init_object((Object*)mod, OBJ_MODULE, sizeof(ObjModule));
    mod->name = new_string(name, (int)strlen(name));
    mod->exports = new_dict();
    return mod;
}

ObjBuffer *new_buffer(size_t capacity) {
    ObjBuffer *buf = malloc(sizeof(ObjBuffer));
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    py_init_object((Object*)buf, OBJ_BUFFER, sizeof(ObjBuffer) + capacity);
    buf->size = 0;
    buf->capacity = capacity;
    buf->cursor = 0;
    buf->data = capacity ? malloc(capacity) : NULL;
    if (capacity && !buf->data) { fprintf(stderr, "OOM\n"); exit(1); }
    return buf;
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

Value make_exception_instance(struct VM *vm, void *cls_obj, const char *message) {
    ObjClass *cls = (ObjClass*)cls_obj;
    if (!cls) cls = py_fe(vm)->exception_class;
    if (!cls) return make_null();
    ObjInstance *inst = new_instance(cls, 4);
    instance_set_field(inst, "message", make_obj((Object*)new_string(message, strlen(message))));
    return make_obj((Object*)inst);
}

/* ============================================================ */
/* Memory management                                             */
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
 * Used exclusively by the GC sweep. This function only frees raw memory. */
void free_object_container(Object *obj) {
    /* WARNING: This function MUST NOT release child objects.
     * Only frees raw container memory. */
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

    if (obj->type->free) obj->type->free(obj);
}



/* ============================================================ */
/* Value predicates                                              */

/* ============================================================ */
/* Value predicates                                              */
/* ============================================================ */



bool is_truthy(Value v) {
    if (IS_NIL(v)) return false;
    if (IS_BOOL(v)) return AS_BOOL(v);
    if (IS_INT(v)) return AS_INT(v) != 0;
    if (IS_BIGINT(v)) return ((ObjBigInt *)AS_OBJ(v))->sign != 0;
    if (IS_DOUBLE(v)) return AS_DOUBLE(v) != 0.0;
    if (IS_OBJ(v)) return AS_OBJ(v) != NULL;
    return false;
}

bool values_equal(Value a, Value b) {
    if (a == b) return true;
    /* Exact numeric equality across int32 / bigint / double. Heap bigints
       never duplicate an int32 (normalized at construction), but bigints can
       meet doubles, and doubles lose precision above 2^53, so those paths
       compare exactly instead of through rounded doubles. */
    bool a_num = IS_INT(a) || IS_BIGINT(a) || IS_DOUBLE(a);
    bool b_num = IS_INT(b) || IS_BIGINT(b) || IS_DOUBLE(b);
    if (a_num && b_num) {
        if (IS_BIGINT(a) && IS_BIGINT(b))
            return bigint_cmp((ObjBigInt *)AS_OBJ(a), (ObjBigInt *)AS_OBJ(b)) == 0;
        if (IS_BIGINT(a) && IS_DOUBLE(b))
            return bigint_cmp_f64((ObjBigInt *)AS_OBJ(a), AS_DOUBLE(b)) == 0;
        if (IS_BIGINT(b) && IS_DOUBLE(a))
            return bigint_cmp_f64((ObjBigInt *)AS_OBJ(b), AS_DOUBLE(a)) == 0;
        if (IS_BIGINT(a))
            return bigint_cmp_value((ObjBigInt *)AS_OBJ(a), b) == 0;
        if (IS_BIGINT(b))
            return bigint_cmp_value((ObjBigInt *)AS_OBJ(b), a) == 0;
        /* int32 vs int32/double: floats are exact over the int32 range. */
        return as_double(a) == as_double(b);
    }
    /* Heap objects: route through the type's eq vtable. The default impl is
     * pointer equality (matching the previous behaviour); concrete types may
     * override for value semantics. */
    if (IS_OBJ(a) && IS_OBJ(b)) {
        Type *ta = AS_OBJ(a)->type, *tb = AS_OBJ(b)->type;
        if (ta == tb && ta->eq) return ta->eq(a, b);
    }
    return false;
}

char *value_to_string(Value v) {
    char buf[64];
    if (IS_NIL(v)) return strdup("null");
    if (IS_BOOL(v)) return strdup(AS_BOOL(v) ? "true" : "false");
    if (IS_INT(v)) { snprintf(buf, sizeof(buf), "%d", AS_INT(v)); return strdup(buf); }
    if (IS_BIGINT(v)) return bigint_to_decimal((ObjBigInt*)AS_OBJ(v));
    if (IS_DOUBLE(v)) { snprintf(buf, sizeof(buf), "%g", AS_DOUBLE(v)); return strdup(buf); }
    if (IS_OBJ(v)) {
        Object *obj = AS_OBJ(v);
        if (!obj) return strdup("null");
        if (obj->type->to_cstr) return obj->type->to_cstr(v);
        return strdup("<object>");
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
            inst->fields[i] = value;
            return;
        }
    }
    if (inst->field_count >= inst->field_capacity) {
        inst->field_capacity = inst->field_capacity < 4 ? 4 : inst->field_capacity * 2;
        inst->field_names = realloc(inst->field_names, inst->field_capacity * sizeof(char *));
        inst->fields      = realloc(inst->fields,      inst->field_capacity * sizeof(Value));
    }
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
    return bigint_from_i64_value((int64_t)raw);
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
    list->count++;
}

Value list_remove(ObjList *list, int index) {
    if (index < 0 || index >= list->count) { fprintf(stderr, "list.remove: out of bounds\n"); return make_null(); }
    Value v = list->items ? list->items[index] : list->inline_items[index];
    if (list->items) {
        memmove(&list->items[index], &list->items[index + 1], (list->count - index - 1) * sizeof(Value));
    } else {
        memmove(&list->inline_items[index], &list->inline_items[index + 1], (list->count - index - 1) * sizeof(Value));
    }
    list->count--;
    return v;
}

Value list_pop(ObjList *list) {
    if (!list->count) { fprintf(stderr, "list.pop: empty\n"); return make_null(); }
    return list_remove(list, list->count - 1);
}

void list_clear(ObjList *list) {
    list->count = 0;
}

Value list_get(ObjList *list, int index) {
    if (index < 0 || index >= list->count) return make_null();
    return list->items ? list->items[index] : list->inline_items[index];
}

void list_set(ObjList *list, int index, Value value) {
    if (index < 0 || index >= list->count) { fprintf(stderr, "list: index out of bounds\n"); return; }
    if (list->items) {
        list->items[index] = value;
    } else {
        list->inline_items[index] = value;
    }
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
    int old_cap = d->capacity;
    int new_cap = old_cap == 0 ? 8 : old_cap * 2;
    if (old_cap > 8 && d->entry_count <= old_cap / 2) {
        new_cap = old_cap;
    }

    ObjDictEntry *new_entries = calloc(new_cap, sizeof(ObjDictEntry));
    if (!new_entries) { fprintf(stderr, "OOM\n"); exit(1); }
    for (int i = 0; i < new_cap; i++) new_entries[i].key = EMPTY_VAL;

    int *new_order = malloc(d->order_count * sizeof(int));
    if (!new_order) { fprintf(stderr, "OOM\n"); exit(1); }
    int new_order_count = 0;

    for (int oi = 0; oi < d->order_count; oi++) {
        int old_idx = d->order[oi];
        ObjDictEntry *e = &d->entries[old_idx];
        if (e->key == EMPTY_VAL || e->key == TOMBSTONE_VAL) continue;

        uint32_t perturb = e->hash;
        uint32_t mask = new_cap - 1;
        uint32_t ni = e->hash & mask;
        while (new_entries[ni].key != EMPTY_VAL) {
            ni = ((ni << 2) + ni + perturb + 1) & mask;
            perturb >>= 5;
        }
        new_entries[ni] = *e;
        new_order[new_order_count++] = (int)ni;
    }

    free(d->entries);
    free(d->order);
    d->entries = new_entries;
    d->order = new_order;
    d->capacity = new_cap;
    d->order_count = new_order_count;
    d->order_capacity = new_order_count;
    d->tombstone_count = 0;
}

void dict_set(void *obj, Value key, Value value) {
    ObjDict *d = (ObjDict*)obj;
    if (d->entries == NULL) {
        for (int i = 0; i < d->entry_count; i++) {
            if (values_equal(d->inline_entries[i].key, key)) {
                d->inline_entries[i].value = value;
                return;
            }
        }
        if (d->entry_count < 4) {
            d->inline_entries[d->entry_count].hash = hash_value(key);
            d->inline_entries[d->entry_count].key = key;
            d->inline_entries[d->entry_count].value = value;
            d->entry_count++;
            return;
        }
        dict_transition_to_heap(d);
    }

    if (d->capacity == 0 || d->entry_count + d->tombstone_count >= d->capacity * 2 / 3) {
        dict_resize(d);
    }

    uint32_t hash = hash_value(key);
    uint32_t perturb = hash;
    uint32_t mask = d->capacity - 1;
    uint32_t i = hash & mask;
    int tombstone_slot = -1;

    while (d->entries[i].key != EMPTY_VAL) {
        if (d->entries[i].key == TOMBSTONE_VAL) {
            if (tombstone_slot < 0) tombstone_slot = (int)i;
        } else if (d->entries[i].hash == hash && values_equal(d->entries[i].key, key)) {
            d->entries[i].value = value;
            return;
        }
        i = ((i << 2) + i + perturb + 1) & mask;
        perturb >>= 5;
    }

    if (tombstone_slot >= 0) i = (uint32_t)tombstone_slot;

    bool reuse_tombstone = (d->entries[i].key == TOMBSTONE_VAL);
    d->entries[i].hash = hash;
    d->entries[i].key = key;
    d->entries[i].value = value;
    if (reuse_tombstone) {
        d->tombstone_count--;
    } else {
        if (d->order_count >= d->order_capacity) {
            d->order_capacity = d->order_capacity < 8 ? 8 : d->order_capacity * 2;
            d->order = realloc(d->order, d->order_capacity * sizeof(int));
            if (!d->order) { fprintf(stderr, "OOM\n"); exit(1); }
        }
        d->order[d->order_count++] = (int)i;
    }
    d->entry_count++;
}

Value dict_get(void *obj, Value key) {
    ObjDict *d = (ObjDict*)obj;
    if (d->entries == NULL) {
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

    while (d->entries[i].key != EMPTY_VAL) {
        if (d->entries[i].key != TOMBSTONE_VAL &&
            d->entries[i].hash == hash &&
            values_equal(d->entries[i].key, key)) {
            return d->entries[i].value;
        }
        i = ((i << 2) + i + perturb + 1) & mask;
        perturb >>= 5;
    }
    return make_null();
}

bool dict_has(void *obj, Value key) {
    ObjDict *d = (ObjDict*)obj;
    if (d->entries == NULL) {
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

    while (d->entries[i].key != EMPTY_VAL) {
        if (d->entries[i].key != TOMBSTONE_VAL &&
            d->entries[i].hash == hash &&
            values_equal(d->entries[i].key, key)) {
            return true;
        }
        i = ((i << 2) + i + perturb + 1) & mask;
        perturb >>= 5;
    }
    return false;
}

Value dict_remove(void *obj, Value key) {
    ObjDict *d = (ObjDict*)obj;
    if (d->entries == NULL) {
        for (int i = 0; i < d->entry_count; i++) {
            if (values_equal(d->inline_entries[i].key, key)) {
                Value v = d->inline_entries[i].value;
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

    while (d->entries[i].key != EMPTY_VAL) {
        if (d->entries[i].key != TOMBSTONE_VAL &&
            d->entries[i].hash == hash &&
            values_equal(d->entries[i].key, key)) {
            Value v = d->entries[i].value;
            d->entries[i].key = TOMBSTONE_VAL;
            d->entries[i].value = make_null();
            d->entry_count--;
            d->tombstone_count++;
            return v;
        }
        i = ((i << 2) + i + perturb + 1) & mask;
        perturb >>= 5;
    }
    return make_null();
}

void dict_clear(void *obj) {
    ObjDict *d = (ObjDict*)obj;
    if (d->entries == NULL) {
        d->entry_count = 0;
        return;
    }
    for (int i = 0; i < d->capacity; i++) d->entries[i].key = EMPTY_VAL;
    d->entry_count = 0;
    d->order_count = 0;
    d->tombstone_count = 0;
}

int dict_length(void *obj) { return ((ObjDict*)obj)->entry_count; }

Value dict_keys(void *obj) {
    ObjDict *d = (ObjDict*)obj;
    ObjList *list = new_list(d->entry_count);
    if (d->entries == NULL) {
        for (int i = 0; i < d->entry_count; i++)
            list_add(list, d->inline_entries[i].key);
    } else {
        for (int i = 0; i < d->order_count; i++) {
            int idx = d->order[i];
            if (d->entries[idx].key != EMPTY_VAL && d->entries[idx].key != TOMBSTONE_VAL)
                list_add(list, d->entries[idx].key);
        }
    }
    return make_obj((Object *)list);
}

Value dict_values(void *obj) {
    ObjDict *d = (ObjDict*)obj;
    ObjList *list = new_list(d->entry_count);
    if (d->entries == NULL) {
        for (int i = 0; i < d->entry_count; i++)
            list_add(list, d->inline_entries[i].value);
    } else {
        for (int i = 0; i < d->order_count; i++) {
            int idx = d->order[i];
            if (d->entries[idx].key != EMPTY_VAL && d->entries[idx].key != TOMBSTONE_VAL)
                list_add(list, d->entries[idx].value);
        }
    }
    return make_obj((Object *)list);
}

/* ============================================================ */
/* Class method helpers                                           */
/* ============================================================ */

void class_add_native_method(void *cls_obj, const char *name, NativeFn fn) {
    ObjClass *cls = (ObjClass*)cls_obj;
    if (cls->method_count >= cls->method_capacity) {
        int new_cap = cls->method_capacity < 8 ? 8 : cls->method_capacity * 2;
        cls->methods      = realloc(cls->methods,      new_cap * sizeof(ObjFunction*));
        cls->method_names = realloc(cls->method_names, new_cap * sizeof(char*));
        cls->method_capacity = new_cap;
    }
    ObjFunction *f = new_native_function(name, fn);
    cls->methods[cls->method_count] = f;
    cls->method_names[cls->method_count] = strdup(name);
    cls->method_count++;
}

struct ObjClass *get_class(VM *vm, Value val) {
    if (!IS_OBJ(val) || !AS_OBJ(val)) {
        /* Immediates are virtual objects: bool/int map to int, double to float. */
        if (IS_INT(val) || IS_BOOL(val)) return py_fe(vm)->int_class;
        if (IS_DOUBLE(val)) return py_fe(vm)->float_class;
        return NULL;
    }
    switch (AS_OBJ(val)->type->kind) {
        case OBJ_STRING:       return py_fe(vm)->string_class;
        case OBJ_LIST:         return py_fe(vm)->list_class;
        case OBJ_TUPLE:        return py_fe(vm)->tuple_class;
        case OBJ_DICT:         return py_fe(vm)->dict_class;
        case OBJ_ENUM:         return py_fe(vm)->enum_class;
        case OBJ_BUFFER:       return py_fe(vm)->buffer_class;
        case OBJ_FUNCTION:     return py_fe(vm)->function_class;
        case OBJ_CLOSURE:      return py_fe(vm)->closure_class;
        case OBJ_BOUND_METHOD: return py_fe(vm)->bound_method_class;
        case OBJ_CLASS:        return py_fe(vm)->class_class;
        case OBJ_MODULE:       return py_fe(vm)->module_class;
        case OBJ_USERDATA:     return py_fe(vm)->userdata_class;
        case OBJ_BIGINT:       return py_fe(vm)->int_class;
        case OBJ_INSTANCE:     return ((ObjInstance*)AS_OBJ(val))->klass;
        default:               return NULL;
    }
}

struct ObjFunction *class_find_method(ObjClass *cls, const char *name) {
    if (!cls) return NULL;
    for (int i = 0; i < cls->method_count; i++) {
        if (strcmp(cls->method_names[i], name) == 0) {
            return cls->methods[i];
        }
    }
    if (cls->base) return class_find_method(cls->base, name);
    return NULL;
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

/* ============================================================ */
/* UTF-8 helpers                                                 */
/* ============================================================ */

int utf8_code_point_count(const char *s, int byte_len) {
    int count = 0;
    for (int i = 0; i < byte_len; i++) {
        if ((s[i] & 0xC0) != 0x80) count++;
    }
    return count;
}
