/* luna/object.c — Luna MOP Type instances + vtable implementations.
 *
 * Part 1: Type definitions + luna_types[] table.
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
#include "luna/object.h"
#include "chunk.h"
#include "vm.h"

/* Forward declarations for helpers used by the lifecycle/formatting vtable
 * (defined later in this file, but referenced by the per-type free funcs). */
static void intern_remove(ObjString *s);
static void userdata_run_finalizer(ObjUserdata *ud);

/* ---- helpers available to vtable functions ---- */
static inline bool is_int64_type(Value v) { return IS_OBJ(v) && AS_OBJ(v) && AS_OBJ(v)->type == luna_types[OBJ_INT64]; }

/* ============================================================
 * String operations (vtable)
 * ============================================================ */
static Value luna_string_add(struct VM *vm, Value a, Value b) {
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

static Value luna_string_mul(struct VM *vm, Value a, Value b) {
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
        return luna_string_mul(vm, b, a);
    }
    return make_null();
}

static int luna_string_cmp(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_STRING(a) || !IS_STRING(b)) return 2; /* unsupported -> do_cmp returns null */
    ObjString *as = (ObjString*)AS_OBJ(a);
    ObjString *bs = (ObjString*)AS_OBJ(b);
    if (as == bs) return 0;
    if (as->length != bs->length) return (as->length < bs->length) ? -1 : 1;
    int cmp = memcmp(as->chars, bs->chars, (size_t)as->length);
    return (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
}

static Value luna_string_neg(struct VM *vm, Value a) { (void)vm; return make_null(); }
static Value luna_string_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_string_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_string_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }

static Value luna_string_getitem(struct VM *vm, Value self, Value key) {
    (void)vm;
    if (!IS_STRING(self) || !IS_INT(key)) return make_null();
    ObjString *s = (ObjString*)AS_OBJ(self);
    int idx = (int)AS_INT(key);
    if (idx < 0) idx += s->length;
    if (idx < 0 || idx >= s->length) return make_null();
    return make_obj((Object*)new_string(&s->chars[idx], 1));
}

static void luna_string_setitem(struct VM *vm, Value self, Value key, Value val) {
    (void)vm; (void)self; (void)key; (void)val; /* unsupported */
}

static Value luna_string_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int luna_string_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value luna_string_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }

/* ---- MOP call vtables (Part 5) ----
 * Bytecode callables (function/closure/bound_method) run synchronously through
 * vm_call_value, which manages real Luna call frames via a nested execute loop.
 * vm_call_value returns a VMResult (VM_OK == 0 on success), so the check is
 * `!= VM_OK`, not `!`. On failure the exception is already in vm->last_exception;
 * we re-propagate via longjmp exactly like op_throw (goto op_throw), preserving
 * Luna's frame-based exception unwinding instead of swallowing it as null. */
static Value luna_function_call(struct VM *vm, Value self, Value *args, int argc) {
    if (!IS_FUNCTION(self)) return make_null();
    ObjFunction *fn = (ObjFunction*)AS_OBJ(self);
    if (fn->is_native) {
        Value scratch[256];
        for (int i = 0; i < argc; i++) scratch[i] = args[i];
        Value result;
        if (fn->cfunc) {
            result = luna_cfunc_dispatch(vm, fn, scratch, argc);
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

static Value luna_closure_call(struct VM *vm, Value self, Value *args, int argc) {
    if (!IS_CLOSURE(self)) return make_null();
    ObjClosure *cl = (ObjClosure *)AS_OBJ(self);
    if (cl->function->is_native) {
        /* Native closure: dispatch like luna_function_call.  This mirrors the
         * old inline OP_INVOKE native path (which inspected fn->is_native) and
         * avoids an infinite loop when the core routes a native closure through
         * t->call (get_chunk returns NULL, so vm_call_value would re-enter). */
        Value scratch[256];
        for (int i = 0; i < argc; i++) scratch[i] = args[i];
        Value result;
        if (cl->function->cfunc) return luna_cfunc_dispatch(vm, cl->function, scratch, argc);
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

static Value luna_bound_method_call(struct VM *vm, Value self, Value *args, int argc) {
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
 * Callable / closure protocol (Part 6b)
 *
 * The core (op_call / op_ret / OP_GETUPVAL / OP_SETUPVAL) reaches a callable's
 * bytecode chunk, bound self, display name and upvalue storage purely through
 * these vtable methods — it never names ObjClosure / ObjFunction /
 * ObjBoundMethod / ObjInstance. Native callables return NULL from get_chunk and
 * are dispatched synchronously via `call`.
 * ============================================================ */
static Chunk* luna_function_get_chunk(Value self) {
    ObjFunction *fn = (ObjFunction*)AS_OBJ(self);
    return fn->is_native ? NULL : fn->chunk;
}
static Value luna_function_get_self(Value self) { (void)self; return make_null(); }
static const char* luna_function_name_of(Value self) { return ((ObjFunction*)AS_OBJ(self))->name; }
static int luna_function_param_count(Value self) { return ((ObjFunction*)AS_OBJ(self))->param_count; }
static Value luna_function_get_param_name(Value self, int i) {
    ObjFunction *fn = (ObjFunction*)AS_OBJ(self);
    if (i < 0 || i >= fn->param_count) return make_null();
    return make_obj((Object*)fn->param_name_objs[i]);
}
static int luna_closure_param_count(Value self) { return ((ObjClosure*)AS_OBJ(self))->function->param_count; }
static Value luna_closure_get_param_name(Value self, int i) {
    ObjFunction *fn = ((ObjClosure*)AS_OBJ(self))->function;
    if (i < 0 || i >= fn->param_count) return make_null();
    return make_obj((Object*)fn->param_name_objs[i]);
}

static Chunk* luna_closure_get_chunk(Value self) {
    ObjClosure *cl = (ObjClosure*)AS_OBJ(self);
    ObjFunction *fn = cl->function;
    return fn->is_native ? NULL : fn->chunk;
}
static Value luna_closure_get_self(Value self) { (void)self; return make_null(); }
static const char* luna_closure_name_of(Value self) { return ((ObjClosure*)AS_OBJ(self))->function->name; }
static Value luna_closure_get_upvalue(struct VM *vm, Value self, int i) {
    ObjClosure *cl = (ObjClosure*)AS_OBJ(self);
    if (i < 0 || i >= cl->upvalue_count) return make_null();
    ObjUpvalue *uv = cl->upvalues[i];
    if (!uv) return make_null();
    return uv->is_open ? vm->stack[uv->stack_index] : uv->closed;
}
static void luna_closure_set_upvalue(struct VM *vm, Value self, int i, Value v) {
    ObjClosure *cl = (ObjClosure*)AS_OBJ(self);
    if (i < 0 || i >= cl->upvalue_count) return;
    ObjUpvalue *uv = cl->upvalues[i];
    if (!uv) return;
    if (uv->is_open) vm->stack[uv->stack_index] = v; else uv->closed = v;
}
static Value luna_closure_get_upvalue_ref(Value self, int i) {
    ObjClosure *cl = (ObjClosure*)AS_OBJ(self);
    if (i < 0 || i >= cl->upvalue_count) return make_null();
    ObjUpvalue *uv = cl->upvalues[i];
    return uv ? make_obj((Object*)uv) : make_null();
}

static Chunk* luna_bound_method_get_chunk(Value self) {
    ObjBoundMethod *bm = (ObjBoundMethod*)AS_OBJ(self);
    return bm->fn->is_native ? NULL : bm->fn->chunk;
}
static Value luna_bound_method_get_self(Value self) { return ((ObjBoundMethod*)AS_OBJ(self))->self; }
static const char* luna_bound_method_name_of(Value self) { return ((ObjBoundMethod*)AS_OBJ(self))->fn->name; }

static Chunk* luna_instance_get_chunk(Value self) {
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    if (!inst->klass) return NULL;
    for (int i = 0; i < inst->klass->method_count; i++)
        if (strcmp(inst->klass->method_names[i], "_call") == 0) {
            ObjFunction *fn = inst->klass->methods[i];
            return fn->is_native ? NULL : fn->chunk;
        }
    return NULL;
}
static Value luna_instance_get_self(Value self) { return self; }
static const char* luna_instance_name_of(Value self) {
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    return inst->klass ? inst->klass->name : "instance";
}

static bool luna_bind_keyword_arguments(struct VM *vm, Value fn_val, uint8_t nargs,
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

static Value luna_string_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_string_hash(Value self) { return ((ObjString*)AS_OBJ(self))->hash; }
static int luna_string_len(struct VM *vm, Value self) { (void)vm; return utf8_code_point_count(((ObjString*)AS_OBJ(self))->chars, ((ObjString*)AS_OBJ(self))->length); }

/* ============================================================
 * List operations (vtable)
 * ============================================================ */
static Value luna_list_add(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_LIST(a) || !IS_LIST(b)) return make_null();
    ObjList *ls = (ObjList*)AS_OBJ(a);
    ObjList *rs = (ObjList*)AS_OBJ(b);
    ObjList *res = new_list(ls->count + rs->count);
    for (int i = 0; i < ls->count; i++) list_add(res, ls->items ? ls->items[i] : ls->inline_items[i]);
    for (int i = 0; i < rs->count; i++) list_add(res, rs->items ? rs->items[i] : rs->inline_items[i]);
    return make_obj((Object*)res);
}

static Value luna_list_mul(struct VM *vm, Value a, Value b) {
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
        return luna_list_mul(vm, b, a);
    }
    return make_null();
}

static int luna_list_cmp(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_LIST(a) || !IS_LIST(b)) return 2; /* unsupported */
    /* Only exact equality is supported for list comparison (pointer-based in current code) */
    return (a == b) ? 0 : 1;  /* approximate; current behavior uses values_equal for EQ/NE */
    /* Actually current values_equal compares pointers for objects; let's replicate: */
    /* This is handled by the caller (do_cmp) for EQ/NE; cmp vtable only called when needed. */
}

static Value luna_list_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_list_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_list_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_list_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }

static Value luna_list_getitem(struct VM *vm, Value self, Value key) {
    (void)vm;
    if (!IS_LIST(self) || !IS_INT(key)) return make_null();
    ObjList *lst = (ObjList*)AS_OBJ(self);
    int idx = (int)AS_INT(key);
    int len = lst->count;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return make_null();
    return lst->items ? lst->items[idx] : lst->inline_items[idx];
}

static void luna_list_setitem(struct VM *vm, Value self, Value key, Value val) {
    (void)vm;
    if (!IS_LIST(self) || !IS_INT(key)) return;
    ObjList *lst = (ObjList*)AS_OBJ(self);
    int idx = (int)AS_INT(key);
    int len = lst->count;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return; /* out of bounds -> no-op for this vtable */
    if (lst->items) lst->items[idx] = val; else lst->inline_items[idx] = val;
}

static Value luna_list_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int luna_list_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value luna_list_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_list_tostring(struct VM *vm, Value self) { (void)vm; return self; }  /* approximate */
static uint32_t luna_list_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_list_len(struct VM *vm, Value self) { (void)vm; return ((ObjList*)AS_OBJ(self))->count; }

/* ============================================================
 * Dict operations (vtable)
 * ============================================================ */
static Value luna_dict_add(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_dict_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_dict_mul(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_dict_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_dict_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_dict_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }
static int luna_dict_cmp(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return 2; }

static Value luna_dict_getitem(struct VM *vm, Value self, Value key) {
    (void)vm;
    if (!IS_DICT(self)) return make_null();
    return dict_get((ObjDict*)AS_OBJ(self), key);
}

static void luna_dict_setitem(struct VM *vm, Value self, Value key, Value val) {
    (void)vm;
    if (!IS_DICT(self)) return;
    dict_set((ObjDict*)AS_OBJ(self), key, val);
}

static Value luna_dict_getattr(struct VM *vm, Value self, const char *name) {
    (void)vm;
    if (!IS_DICT(self)) return make_null();
    return dict_get((ObjDict*)AS_OBJ(self), make_obj((Object*)new_string(name, (int)strlen(name))));
}
static int luna_dict_setattr(struct VM *vm, Value self, const char *name, Value val) {
    (void)vm;
    if (!IS_DICT(self)) return 0;
    dict_set((ObjDict*)AS_OBJ(self), make_obj((Object*)new_string(name, (int)strlen(name))), val);
    return 1;
}
static Value luna_dict_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_dict_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_dict_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_dict_len(struct VM *vm, Value self) { (void)vm; return ((ObjDict*)AS_OBJ(self))->entry_count; }

/* ============================================================
 * Instance operations (vtable)
 * ============================================================ */
static Value luna_instance_add(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_instance_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_instance_mul(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_instance_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_instance_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_instance_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }
static int luna_instance_cmp(struct VM *vm, Value a, Value b) { (void)vm; if (!IS_INSTANCE(a) || !IS_INSTANCE(b)) return 2; return (a == b) ? 0 : 1; }

static Value luna_instance_getitem(struct VM *vm, Value self, Value key) {
    (void)vm;
    if (!IS_INSTANCE(self) || !IS_STRING(key)) return make_null();
    return instance_get_field((ObjInstance*)AS_OBJ(self), ((ObjString*)AS_OBJ(key))->chars);
}

static void luna_instance_setitem(struct VM *vm, Value self, Value key, Value val) {
    (void)vm;
    if (!IS_INSTANCE(self) || !IS_STRING(key)) return;
    instance_set_field((ObjInstance*)AS_OBJ(self), ((ObjString*)AS_OBJ(key))->chars, val);
}

static Value luna_instance_getattr(struct VM *vm, Value self, const char *name) {
    (void)vm;
    if (!IS_INSTANCE(self)) return make_null();
    return instance_get_field((ObjInstance*)AS_OBJ(self), name);
}

static int luna_instance_setattr(struct VM *vm, Value self, const char *name, Value val) {
    (void)vm;
    if (!IS_INSTANCE(self)) return 0;
    instance_set_field((ObjInstance*)AS_OBJ(self), name, val);
    return 1;
}

static Value luna_instance_call(struct VM *vm, Value self, Value *args, int argc) {
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
        if (fn->cfunc) return luna_cfunc_dispatch(vm, fn, scratch, argc + 1);
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
static Value luna_instance_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_instance_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_instance_len(struct VM *vm, Value self) { (void)vm; (void)self; return 0; }

/* Python-style class instantiation: `Foo(args...)` allocates an instance and
 * runs its `__init__` method (inherited methods included) with self bound as
 * the first argument.  Missing __init__ is a no-op (instance is returned). */
static Value py_class_call(struct VM *vm, Value self, Value *args, int argc) {
    if (!IS_CLASS(self)) return make_null();
    ObjClass *cls = (ObjClass*)AS_OBJ(self);
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
            if (fn->cfunc) return luna_cfunc_dispatch(vm, fn, scratch, argc + 1);
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
 * Vector operations (vtable)
 * ============================================================ */
static Value luna_vector_add(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_VECTOR(a) || !IS_VECTOR(b)) return make_null();
    ObjVector *av = (ObjVector*)AS_OBJ(a), *bv = (ObjVector*)AS_OBJ(b);
    return make_obj((Object*)new_vector(av->data[0]+bv->data[0], av->data[1]+bv->data[1], av->data[2]+bv->data[2], av->data[3]+bv->data[3]));
}
static Value luna_vector_sub(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_VECTOR(a) || !IS_VECTOR(b)) return make_null();
    ObjVector *av = (ObjVector*)AS_OBJ(a), *bv = (ObjVector*)AS_OBJ(b);
    return make_obj((Object*)new_vector(av->data[0]-bv->data[0], av->data[1]-bv->data[1], av->data[2]-bv->data[2], av->data[3]-bv->data[3]));
}
static Value luna_vector_mul(struct VM *vm, Value a, Value b) {
    (void)vm;
    /* Component-wise or scalar mul (current behavior in do_arith) */
    if (IS_VECTOR(a) && IS_VECTOR(b)) {
        ObjVector *av = (ObjVector*)AS_OBJ(a), *bv = (ObjVector*)AS_OBJ(b);
        return make_obj((Object*)new_vector(av->data[0]*bv->data[0], av->data[1]*bv->data[1], av->data[2]*bv->data[2], av->data[3]*bv->data[3]));
    }
    if (IS_VECTOR(a) && IS_NUMBER(b)) {
        ObjVector *av = (ObjVector*)AS_OBJ(a);
        float s = (float)value_to_double(b);
        return make_obj((Object*)new_vector(av->data[0]*s, av->data[1]*s, av->data[2]*s, av->data[3]*s));
    }
    if (IS_NUMBER(a) && IS_VECTOR(b)) {
        return luna_vector_mul(vm, b, a);
    }
    return make_null();
}
static Value luna_vector_div(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (IS_VECTOR(a) && IS_NUMBER(b)) {
        ObjVector *av = (ObjVector*)AS_OBJ(a);
        float s = (float)value_to_double(b);
        return make_obj((Object*)new_vector(av->data[0]/s, av->data[1]/s, av->data[2]/s, av->data[3]/s));
    }
    return make_null();
}
static Value luna_vector_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_vector_neg(struct VM *vm, Value a) {
    (void)vm;
    if (!IS_VECTOR(a)) return make_null();
    ObjVector *v = (ObjVector*)AS_OBJ(a);
    return make_obj((Object*)new_vector(-v->data[0], -v->data[1], -v->data[2], -v->data[3]));
}
static int luna_vector_cmp(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_VECTOR(a) || !IS_VECTOR(b)) return 2;
    return a == b ? 0 : 1;
}

static Value luna_vector_getitem(struct VM *vm, Value self, Value key) { (void)vm; (void)self; (void)key; return make_null(); }
static void luna_vector_setitem(struct VM *vm, Value self, Value key, Value val) { (void)vm; (void)self; (void)key; (void)val; }
static Value luna_vector_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int luna_vector_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value luna_vector_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_vector_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_vector_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_vector_len(struct VM *vm, Value self) { (void)vm; return 4; }

/* ============================================================
 * Matrix operations (vtable)
 * ============================================================ */
static Value luna_matrix_add(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_matrix_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_matrix_mul(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (IS_MATRIX(a) && IS_MATRIX(b)) {
        ObjMatrix *ma = (ObjMatrix*)AS_OBJ(a), *mb = (ObjMatrix*)AS_OBJ(b);
        ObjMatrix *res = new_matrix();
        for (int col = 0; col < 4; col++) for (int row = 0; row < 4; row++) {
            float sum = 0.0f; for (int k = 0; k < 4; k++) sum += ma->m[row + k*4] * mb->m[k + col*4];
            res->m[row + col*4] = sum; }
        return make_obj((Object*)res);
    }
    if (IS_MATRIX(a) && IS_VECTOR(b)) {
        ObjMatrix *m = (ObjMatrix*)AS_OBJ(a); ObjVector *v = (ObjVector*)AS_OBJ(b);
        float rx = m->m[0]*v->data[0] + m->m[4]*v->data[1] + m->m[8]*v->data[2] + m->m[12]*v->data[3];
        float ry = m->m[1]*v->data[0] + m->m[5]*v->data[1] + m->m[9]*v->data[2] + m->m[13]*v->data[3];
        float rz = m->m[2]*v->data[0] + m->m[6]*v->data[1] + m->m[10]*v->data[2] + m->m[14]*v->data[3];
        float rw = m->m[3]*v->data[0] + m->m[7]*v->data[1] + m->m[11]*v->data[2] + m->m[15]*v->data[3];
        return make_obj((Object*)new_vector(rx, ry, rz, rw));
    }
    if (IS_MATRIX(a) && IS_NUMBER(b)) {
        ObjMatrix *m = (ObjMatrix*)AS_OBJ(a); float s = (float)value_to_double(b); ObjMatrix *res = new_matrix();
        for (int i = 0; i < 16; i++) res->m[i] = m->m[i] * s; return make_obj((Object*)res);
    }
    return make_null();
}
static Value luna_matrix_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_matrix_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_matrix_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }
static int luna_matrix_cmp(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return 2; }

static Value luna_matrix_getitem(struct VM *vm, Value self, Value key) { (void)vm; (void)self; (void)key; return make_null(); }
static void luna_matrix_setitem(struct VM *vm, Value self, Value key, Value val) { (void)vm; (void)self; (void)key; (void)val; }
static Value luna_matrix_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int luna_matrix_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value luna_matrix_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_matrix_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_matrix_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_matrix_len(struct VM *vm, Value self) { (void)vm; return 16; }

/* ============================================================
 * Int64 numeric arithmetic.
 * The core int (int32) path already promotes to int64 on overflow in
 * luna_binary_operation(); these MOPs restore that behaviour for the
 * int64 object itself (e.g. int64 + int32) and promote the result back
 * to core int whenever it fits, keeping the value as int64 otherwise.
 * ============================================================ */
static Value luna_int64_promote(int64_t value) {
    if (value >= -2147483647LL - 1LL && value <= 2147483647LL)
        return make_int((int32_t)value);
    return make_int64(value);
}

static Value luna_int64_add(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (IS_DOUBLE(a) || IS_DOUBLE(b)) return make_double(as_double(a) + as_double(b));
    return luna_int64_promote(as_int64(a) + as_int64(b));
}
static Value luna_int64_sub(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (IS_DOUBLE(a) || IS_DOUBLE(b)) return make_double(as_double(a) - as_double(b));
    return luna_int64_promote(as_int64(a) - as_int64(b));
}
static Value luna_int64_mul(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (IS_DOUBLE(a) || IS_DOUBLE(b)) return make_double(as_double(a) * as_double(b));
    return luna_int64_promote(as_int64(a) * as_int64(b));
}
static Value luna_int64_div(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (IS_DOUBLE(a) || IS_DOUBLE(b)) {
        double da = as_double(a), db = as_double(b);
        return make_double(db == 0.0 ? da / 0.0 : da / db);
    }
    int64_t ia = as_int64(a), ib = as_int64(b);
    if (ib == 0) return make_null();
    return luna_int64_promote(ia / ib);
}
static Value luna_int64_mod(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (IS_DOUBLE(a) || IS_DOUBLE(b)) {
        double da = as_double(a), db = as_double(b);
        return make_double(db == 0.0 ? da / 0.0 : fmod(da, db));
    }
    int64_t ia = as_int64(a), ib = as_int64(b);
    if (ib == 0) return make_null();
    return luna_int64_promote(ia % ib);
}
static Value luna_int64_neg(struct VM *vm, Value a) {
    (void)vm;
    if (IS_DOUBLE(a)) return make_double(-AS_DOUBLE(a));
    return luna_int64_promote(-as_int64(a));
}
static int luna_int64_cmp(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (IS_DOUBLE(a) || IS_DOUBLE(b)) {
        double da = as_double(a), db = as_double(b);
        return (da < db) ? -1 : (da > db) ? 1 : 0;
    }
    int64_t ia = as_int64(a), ib = as_int64(b);
    return (ia < ib) ? -1 : (ia > ib) ? 1 : 0;
}
static Value luna_int64_getitem(struct VM *vm, Value self, Value key) { (void)vm; (void)self; (void)key; return make_null(); }
static void luna_int64_setitem(struct VM *vm, Value self, Value key, Value val) { (void)vm; (void)self; (void)key; (void)val; }
static Value luna_int64_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int luna_int64_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value luna_int64_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_int64_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_int64_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_int64_len(struct VM *vm, Value self) { (void)vm; return 1; }

/* ============================================================
 * Default stubs for kinds without special arithmetic/index rules
 * ============================================================ */
#define DEFAULT_BIN  (struct VM *vm, Value a, Value b) -> return make_null()
static Value luna_default_add(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_default_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_default_mul(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_default_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_default_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_default_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }
static int luna_default_cmp(struct VM *vm, Value a, Value b) { (void)vm; return (a == b) ? 0 : 1; }
static Value luna_default_getitem(struct VM *vm, Value self, Value key) { (void)vm; (void)self; (void)key; return make_null(); }
static void luna_default_setitem(struct VM *vm, Value self, Value key, Value val) { (void)vm; (void)self; (void)key; (void)val; }
static Value luna_default_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int luna_default_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value luna_default_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_default_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_default_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_default_len(struct VM *vm, Value self) { (void)vm; return 0; }

/* ============================================================
 * Type instance definitions (vtable filled)
 * ============================================================ */
Type luna_string_type  = {
    .name = "str", .kind = OBJ_STRING,
    .add = luna_string_add, .sub = luna_string_sub, .mul = luna_string_mul, .div = luna_string_div, .mod = luna_string_mod,
    .neg = luna_string_neg, .cmp = luna_string_cmp,
    .getitem = luna_string_getitem, .setitem = luna_string_setitem,
    .getattr = luna_string_getattr, .setattr = luna_string_setattr,
    .call = luna_string_call, .tostring = luna_string_tostring, .hash = luna_string_hash, .len = luna_string_len
};

Type luna_list_type = {
    .name = "list", .kind = OBJ_LIST,
    .add = luna_list_add, .sub = luna_list_sub, .mul = luna_list_mul, .div = luna_list_div, .mod = luna_list_mod,
    .neg = luna_list_neg, .cmp = luna_list_cmp,
    .getitem = luna_list_getitem, .setitem = luna_list_setitem,
    .getattr = luna_list_getattr, .setattr = luna_list_setattr,
    .call = luna_list_call, .tostring = luna_list_tostring, .hash = luna_list_hash, .len = luna_list_len
};

Type luna_dict_type = {
    .name = "dict", .kind = OBJ_DICT,
    .add = luna_dict_add, .sub = luna_dict_sub, .mul = luna_dict_mul, .div = luna_dict_div, .mod = luna_dict_mod,
    .neg = luna_dict_neg, .cmp = luna_dict_cmp,
    .getitem = luna_dict_getitem, .setitem = luna_dict_setitem,
    .getattr = luna_dict_getattr, .setattr = luna_dict_setattr,
    .call = luna_dict_call, .tostring = luna_dict_tostring, .hash = luna_dict_hash, .len = luna_dict_len
};

Type luna_instance_type = {
    .name = "instance", .kind = OBJ_INSTANCE,
    .add = luna_instance_add, .sub = luna_instance_sub, .mul = luna_instance_mul, .div = luna_instance_div, .mod = luna_instance_mod,
    .neg = luna_instance_neg, .cmp = luna_instance_cmp,
    .getitem = luna_instance_getitem, .setitem = luna_instance_setitem,
    .getattr = luna_instance_getattr, .setattr = luna_instance_setattr,
    .call = luna_instance_call, .tostring = luna_instance_tostring, .hash = luna_instance_hash, .len = luna_instance_len,
    .get_chunk = luna_instance_get_chunk, .get_self = luna_instance_get_self, .name_of = luna_instance_name_of
};

Type luna_function_type = {
    .name = "function", .kind = OBJ_FUNCTION,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_function_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len,
    .get_chunk = luna_function_get_chunk, .get_self = luna_function_get_self, .name_of = luna_function_name_of,
    .param_count = luna_function_param_count, .get_param_name = luna_function_get_param_name,
    .bind_keyword_arguments = luna_bind_keyword_arguments
};

Type luna_closure_type = {
    .name = "closure", .kind = OBJ_CLOSURE,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_closure_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len,
    .get_chunk = luna_closure_get_chunk, .get_self = luna_closure_get_self, .name_of = luna_closure_name_of,
    .get_upvalue = luna_closure_get_upvalue, .set_upvalue = luna_closure_set_upvalue, .get_upvalue_ref = luna_closure_get_upvalue_ref,
    .param_count = luna_closure_param_count, .get_param_name = luna_closure_get_param_name,
    .bind_keyword_arguments = luna_bind_keyword_arguments
};

Type luna_upvalue_type = {
    .name = "upvalue", .kind = OBJ_UPVALUE,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_enum_type = {
    .name = "enum", .kind = OBJ_ENUM,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_class_type = {
    .name = "class", .kind = OBJ_CLASS,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = py_class_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_bound_method_type = {
    .name = "bound_method", .kind = OBJ_BOUND_METHOD,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_bound_method_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len,
    .get_chunk = luna_bound_method_get_chunk, .get_self = luna_bound_method_get_self, .name_of = luna_bound_method_name_of,
    .bind_keyword_arguments = luna_bind_keyword_arguments
};

Type luna_module_type = {
    .name = "module", .kind = OBJ_MODULE,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_buffer_type = {
    .name = "buffer", .kind = OBJ_BUFFER,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_int64_type = {
    .name = "int64", .kind = OBJ_INT64,
    .add = luna_int64_add, .sub = luna_int64_sub, .mul = luna_int64_mul, .div = luna_int64_div, .mod = luna_int64_mod,
    .neg = luna_int64_neg, .cmp = luna_int64_cmp,
    .getitem = luna_int64_getitem, .setitem = luna_int64_setitem,
    .getattr = luna_int64_getattr, .setattr = luna_int64_setattr,
    .call = luna_int64_call, .tostring = luna_int64_tostring, .hash = luna_int64_hash, .len = luna_int64_len
};

Type luna_userdata_type = {
    .name = "userdata", .kind = OBJ_USERDATA,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_vector_type = {
    .name = "vector", .kind = OBJ_VECTOR,
    .add = luna_vector_add, .sub = luna_vector_sub, .mul = luna_vector_mul, .div = luna_vector_div, .mod = luna_vector_mod,
    .neg = luna_vector_neg, .cmp = luna_vector_cmp,
    .getitem = luna_vector_getitem, .setitem = luna_vector_setitem,
    .getattr = luna_vector_getattr, .setattr = luna_vector_setattr,
    .call = luna_vector_call, .tostring = luna_vector_tostring, .hash = luna_vector_hash, .len = luna_vector_len
};

Type luna_matrix_type = {
    .name = "matrix", .kind = OBJ_MATRIX,
    .add = luna_matrix_add, .sub = luna_matrix_sub, .mul = luna_matrix_mul, .div = luna_matrix_div, .mod = luna_matrix_mod,
    .neg = luna_matrix_neg, .cmp = luna_matrix_cmp,
    .getitem = luna_matrix_getitem, .setitem = luna_matrix_setitem,
    .getattr = luna_matrix_getattr, .setattr = luna_matrix_setattr,
    .call = luna_matrix_call, .tostring = luna_matrix_tostring, .hash = luna_matrix_hash, .len = luna_matrix_len
};

/* Indexed by ObjType. */
Type *luna_types[] = {
    [OBJ_STRING]       = &luna_string_type,
    [OBJ_LIST]         = &luna_list_type,
    [OBJ_DICT]         = &luna_dict_type,
    [OBJ_INSTANCE]     = &luna_instance_type,
    [OBJ_FUNCTION]     = &luna_function_type,
    [OBJ_UPVALUE]      = &luna_upvalue_type,
    [OBJ_CLOSURE]      = &luna_closure_type,
    [OBJ_ENUM]         = &luna_enum_type,
    [OBJ_CLASS]        = &luna_class_type,
    [OBJ_BOUND_METHOD] = &luna_bound_method_type,
    [OBJ_MODULE]       = &luna_module_type,
    [OBJ_BUFFER]       = &luna_buffer_type,
    [OBJ_INT64]        = &luna_int64_type,
    [OBJ_USERDATA]     = &luna_userdata_type,
    [OBJ_VECTOR]       = &luna_vector_type,
    [OBJ_MATRIX]       = &luna_matrix_type,
};

/* ============================================================
 * Lifecycle / formatting vtable (Part 6c)
 *
 * The core reaches GC mark, free, equality, and string formatting through
 * these vtable methods instead of switching on ObjType. The frontend owns
 * the kind switch here (luna_wire_lifecycle), keeping the core generic.
 * ============================================================ */

/* defaults — shared by types with no extra resources and no child references */
static void luna_default_free(Object *obj) { free(obj); }
static void luna_default_mark(struct VM *vm, Object *obj) { (void)vm; (void)obj; }
static bool luna_default_eq(Value a, Value b) { return a == b; }
static char* luna_generic_to_cstr(Value self) {
    Object *o = AS_OBJ(self);
    char buf[32];
    snprintf(buf, sizeof(buf), "<%s>", o->type->name);
    return strdup(buf);
}

/* string */
static void luna_string_free(Object *obj) {
    ObjString *s = (ObjString*)obj;
    intern_remove(s);
    free(s->chars);
    free(s);
}
static char* luna_string_to_cstr(Value self) {
    return strdup(((ObjString*)AS_OBJ(self))->chars);
}
static const char* luna_string_chars(Value self) {
    return ((ObjString*)AS_OBJ(self))->chars;
}

/* list */
static void luna_list_free(Object *obj) {
    ObjList *l = (ObjList*)obj;
    if (l->items) free(l->items);
    free(l);
}
static void luna_list_mark(struct VM *vm, Object *obj) {
    ObjList *l = (ObjList*)obj;
    if (l->items) {
        for (int i = 0; i < l->count; i++) vm_mark_value(vm, l->items[i]);
    } else {
        for (int i = 0; i < l->count; i++) vm_mark_value(vm, l->inline_items[i]);
    }
}
static char* luna_list_to_cstr(Value self) {
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
static void luna_dict_free(Object *obj) {
    ObjDict *d = (ObjDict*)obj;
    if (d->entries) { free(d->entries); free(d->order); }
    free(d);
}
static void luna_dict_mark(struct VM *vm, Object *obj) {
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
static char* luna_dict_to_cstr(Value self) {
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
static void luna_instance_free(Object *obj) {
    ObjInstance *inst = (ObjInstance*)obj;
    if (inst->class_name) free(inst->class_name);
    for (int i = 0; i < inst->field_count; i++) {
        if (inst->field_names[i]) free(inst->field_names[i]);
    }
    free(inst->field_names); free(inst->fields); free(inst);
}
static void luna_instance_mark(struct VM *vm, Object *obj) {
    ObjInstance *inst = (ObjInstance*)obj;
    for (int i = 0; i < inst->field_count; i++) vm_mark_value(vm, inst->fields[i]);
    if (inst->klass) vm_mark_value(vm, make_obj((Object*)inst->klass));
}
static char* luna_instance_to_cstr(Value self) {
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
static const char* luna_instance_message(struct VM *vm, Value self) {
    (void)vm;
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    Value msgv = instance_get_field(inst, "message");
    if (IS_STRING(msgv)) return ((ObjString*)AS_OBJ(msgv))->chars;
    return NULL;
}
static const char* luna_instance_class_name(Value self) {
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    return inst->class_name ? inst->class_name : "Error";
}

/* function */
static void luna_function_free(Object *obj) {
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
static void luna_function_mark(struct VM *vm, Object *obj) {
    ObjFunction *f = (ObjFunction*)obj;
    if (f->chunk) {
        for (int i = 0; i < f->chunk->const_count; i++) vm_mark_value(vm, f->chunk->constants[i]);
    }
    if (f->defaults) {
        for (int i = 0; i < f->default_count; i++) vm_mark_value(vm, f->defaults[i]);
    }
}
static char* luna_function_to_cstr(Value self) {
    ObjFunction *f = (ObjFunction*)AS_OBJ(self);
    char buf[64];
    snprintf(buf, sizeof(buf), "<%s %s>", f->is_native ? "native fn" : "fn", f->name ? f->name : "?");
    return strdup(buf);
}

/* upvalue */
static void luna_upvalue_mark(struct VM *vm, Object *obj) {
    ObjUpvalue *uv = (ObjUpvalue*)obj;
    vm_mark_value(vm, uv->closed);
}

/* closure */
static void luna_closure_free(Object *obj) {
    ObjClosure *cl = (ObjClosure*)obj;
    free(cl->upvalues);
    free(cl);
}
static void luna_closure_mark(struct VM *vm, Object *obj) {
    ObjClosure *cl = (ObjClosure*)obj;
    if (cl->function) vm_mark_value(vm, make_obj((Object*)cl->function));
    for (int i = 0; i < cl->upvalue_count; i++) {
        if (cl->upvalues[i]) vm_mark_value(vm, make_obj((Object*)cl->upvalues[i]));
    }
}

/* enum */
static void luna_enum_free(Object *obj) {
    ObjEnum *e = (ObjEnum*)obj;
    free(e->name);
    for (int i = 0; i < e->count; i++) free(e->names[i]);
    free(e->names);
    free(e->values);
    free(e);
}
static char* luna_enum_to_cstr(Value self) {
    ObjEnum *e = (ObjEnum*)AS_OBJ(self);
    char *out = malloc(64);
    int n = snprintf(out, 64, "<enum %s (%d variants)>", e->name, e->count);
    if (n >= 64) { out = realloc(out, (size_t)n + 1); snprintf(out, (size_t)n + 1, "<enum %s (%d variants)>", e->name, e->count); }
    char *r = strdup(out); free(out); return r;
}

/* class */
static void luna_class_free(Object *obj) {
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
static void luna_class_mark(struct VM *vm, Object *obj) {
    ObjClass *cls = (ObjClass*)obj;
    if (cls->base) vm_mark_value(vm, make_obj((Object*)cls->base));
    if (cls->prototype) vm_mark_value(vm, make_obj((Object*)cls->prototype));
    for (int i = 0; i < cls->method_count; i++) {
        if (cls->methods[i]) vm_mark_value(vm, make_obj((Object*)cls->methods[i]));
    }
    if (cls->fields) vm_mark_value(vm, make_obj((Object*)cls->fields));
}

/* bound_method */
static void luna_bound_method_mark(struct VM *vm, Object *obj) {
    ObjBoundMethod *bm = (ObjBoundMethod*)obj;
    vm_mark_value(vm, bm->self);
    if (bm->fn) vm_mark_value(vm, make_obj((Object*)bm->fn));
}
static char* luna_bound_method_to_cstr(Value self) {
    ObjBoundMethod *bm = (ObjBoundMethod*)AS_OBJ(self);
    ObjFunction *f = bm->fn;
    char buf[64];
    snprintf(buf, sizeof(buf), "<bound method %s>", f && f->name ? f->name : "?");
    return strdup(buf);
}

/* module */
static void luna_module_mark(struct VM *vm, Object *obj) {
    ObjModule *mod = (ObjModule*)obj;
    if (mod->name) vm_mark_value(vm, make_obj((Object*)mod->name));
    if (mod->exports) vm_mark_value(vm, make_obj((Object*)mod->exports));
}
static char* luna_module_to_cstr(Value self) {
    ObjModule *mod = (ObjModule*)AS_OBJ(self);
    char buf[64];
    snprintf(buf, sizeof(buf), "<module %s>", mod->name ? mod->name->chars : "?");
    return strdup(buf);
}

/* buffer */
static void luna_buffer_free(Object *obj) {
    ObjBuffer *bufv = (ObjBuffer*)obj;
    free(bufv->data);
    free(bufv);
}
static char* luna_buffer_to_cstr(Value self) {
    ObjBuffer *bufv = (ObjBuffer*)AS_OBJ(self);
    char out[64];
    snprintf(out, sizeof(out), "<buffer %zu bytes>", bufv->size);
    return strdup(out);
}

/* userdata */
static void luna_userdata_free(Object *obj) {
    ObjUserdata *ud = (ObjUserdata*)obj;
    userdata_run_finalizer(ud);
    free(ud->tag);
    free(ud);
}

/* One-time wiring: the frontend owns this kind switch; the core stays generic. */
void luna_wire_lifecycle(void) {
    for (int k = 0; k <= OBJ_MATRIX; k++) {
        Type *t = luna_types[k];
        /* ObjType intentionally has reserved values; there is no Type for
         * those slots in luna_types[]. */
        if (!t) continue;
        if (!t->free) t->free = luna_default_free;
        if (!t->mark) t->mark = luna_default_mark;
        if (!t->eq) t->eq = luna_default_eq;
        if (!t->to_cstr) t->to_cstr = luna_generic_to_cstr;
        switch (k) {
            case OBJ_STRING:
                t->free = luna_string_free; t->to_cstr = luna_string_to_cstr;
                t->string_chars = luna_string_chars; break;
            case OBJ_LIST:
                t->free = luna_list_free; t->mark = luna_list_mark; t->to_cstr = luna_list_to_cstr; break;
            case OBJ_DICT:
                t->free = luna_dict_free; t->mark = luna_dict_mark; t->to_cstr = luna_dict_to_cstr; break;
            case OBJ_INSTANCE:
                t->free = luna_instance_free; t->mark = luna_instance_mark; t->to_cstr = luna_instance_to_cstr;
                t->message = luna_instance_message; t->class_name = luna_instance_class_name; break;
            case OBJ_FUNCTION:
                t->free = luna_function_free; t->mark = luna_function_mark; t->to_cstr = luna_function_to_cstr; break;
            case OBJ_UPVALUE:
                t->mark = luna_upvalue_mark; break;
            case OBJ_CLOSURE:
                t->free = luna_closure_free; t->mark = luna_closure_mark; break;
            case OBJ_ENUM:
                t->free = luna_enum_free; t->to_cstr = luna_enum_to_cstr; break;
            case OBJ_CLASS:
                t->free = luna_class_free; t->mark = luna_class_mark; break;
            case OBJ_BOUND_METHOD:
                t->mark = luna_bound_method_mark; t->to_cstr = luna_bound_method_to_cstr; break;
            case OBJ_MODULE:
                t->mark = luna_module_mark; t->to_cstr = luna_module_to_cstr; break;
            case OBJ_BUFFER:
                t->free = luna_buffer_free; t->to_cstr = luna_buffer_to_cstr; break;
            case OBJ_USERDATA:
                t->free = luna_userdata_free; break;
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



static void init_object(Object *obj, ObjType type, size_t size) {
    obj->type = luna_types[type];
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
    init_object((Object*)cls, OBJ_CLASS, sizeof(ObjClass));
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
    init_object((Object*)inst, OBJ_INSTANCE, sizeof(ObjInstance) + sizeof(Value) * cap);
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
    init_object((Object*)f, OBJ_FUNCTION, sizeof(ObjFunction));
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
    bm->fn = fn;
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
    mod->exports = new_dict();
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

Value make_exception_instance(struct VM *vm, void *cls_obj, const char *message) {
    ObjClass *cls = (ObjClass*)cls_obj;
    if (!cls) cls = vm->exception_class;
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
    if (IS_INT64(v)) return ((ObjInt64*)AS_OBJ(v))->value != 0;
    if (IS_DOUBLE(v)) return AS_DOUBLE(v) != 0.0;
    if (IS_OBJ(v)) return AS_OBJ(v) != NULL;
    return false;
}

bool values_equal(Value a, Value b) {
    if (a == b) return true;
    /* Exact 64-bit integer equality: doubles lose precision above 2^53,
       so two distinct ObjInt64 (e.g. 2^54 and 2^54+1) must not collapse. */
    if (IS_INT64(a) && IS_INT64(b)) {
        return ((ObjInt64*)AS_OBJ(a))->value == ((ObjInt64*)AS_OBJ(b))->value;
    }
    if ((IS_DOUBLE(a) || IS_INT(a) || IS_INT64(a)) && (IS_DOUBLE(b) || IS_INT(b) || IS_INT64(b))) {
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
    if (IS_INT64(v)) { snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)((ObjInt64*)AS_OBJ(v))->value); return strdup(buf); }
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
    if (!IS_OBJ(val) || !AS_OBJ(val)) return NULL;
    switch (AS_OBJ(val)->type->kind) {
        case OBJ_STRING:       return vm->string_class;
        case OBJ_LIST:         return vm->list_class;
        case OBJ_DICT:         return vm->dict_class;
        case OBJ_ENUM:         return vm->enum_class;
        case OBJ_BUFFER:       return vm->buffer_class;
        case OBJ_VECTOR:       return vm->vector_class;
        case OBJ_MATRIX:       return vm->matrix_class;
        case OBJ_FUNCTION:     return vm->function_class;
        case OBJ_CLOSURE:      return vm->closure_class;
        case OBJ_BOUND_METHOD: return vm->bound_method_class;
        case OBJ_CLASS:        return vm->class_class;
        case OBJ_MODULE:       return vm->module_class;
        case OBJ_USERDATA:     return vm->userdata_class;
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
