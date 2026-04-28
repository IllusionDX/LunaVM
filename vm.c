/* vm.c — Luna Register VM: fetch-decode-execute loop.
 *
 * All instructions are 32-bit uint32_t (ABC / ABx / AsBx).
 * See opcode.h for DECODE_OP / DECODE_A / DECODE_B / DECODE_C /
 * DECODE_Bx / DECODE_sBx macros.
 *
 * Register layout per frame:
 *   regs[0..param_count-1]  — parameters
 *   regs[param_count..]     — locals / temporaries
 */

#pragma GCC diagnostic ignored "-Wclobbered"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vm.h"
#include "value.h"
#include "chunk.h"
#include "compiler.h"
#include "module.h"
#include "opcode.h"
#include "stdlib_math.h"
#include "stdlib_random.h"
#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include "module.h"

/* Declared in vm_builtins.c */
void vm_register_builtins(VM *vm);
bool vm_invoke_list(VM *vm, ObjList  *list, const char *method, Value *args, int nargs, Value *result);
bool vm_invoke_dict(VM *vm, ObjDict  *dict, const char *method, Value *args, int nargs, Value *result);
bool vm_invoke_enum(VM *vm, ObjEnum  *enm, const char *method, Value *args, int nargs, Value *result);
ObjFunction *vm_dict_method_lookup(const char *name, int len);

/* ============================================================ */
/* Arithmetic helpers (inlined for dispatch loop)               */
/* ============================================================ */

static inline double to_f64(Value v) {
    if (IS_INT(v)) return (double)AS_INT(v);
    if (IS_DOUBLE(v)) return AS_DOUBLE(v);
    return 0.0;
}
static inline int64_t to_i64(Value v) {
    if (IS_INT(v)) return (int64_t)AS_INT(v);
    if (IS_DOUBLE(v)) return (int64_t)AS_DOUBLE(v);
    return 0;
}
static inline bool is_num(Value v) {
    return IS_NUMBER(v);
}
static inline bool is_int_type(Value v) { return IS_INT(v); }

static inline Value do_arith(Value L, Value R, OpCode op) {
    /* String concat for ADD */
    if (op == OP_ADD && IS_STRING(L)) {
        ObjString *ls = (ObjString*)AS_OBJ(L);
        const char *rs;
        int rs_len;
        char *rs_tmp = NULL;
        if (IS_STRING(R)) {
            ObjString *rs_str = (ObjString*)AS_OBJ(R);
            rs = rs_str->chars;
            rs_len = rs_str->length;
        } else {
            rs_tmp = value_to_string(R);
            rs = rs_tmp;
            rs_len = (int)strlen(rs_tmp);
        }
        int len = ls->length + rs_len;
        char *buf = malloc(len + 1);
        memcpy(buf, ls->chars, ls->length);
        memcpy(buf + ls->length, rs, rs_len + 1);
        ObjString *s = new_string(buf, len);
        free(rs_tmp);
        free(buf);
        return make_obj((Object*)s);
    }
    /* List concat for ADD */
    if (op == OP_ADD && IS_LIST(L) && IS_LIST(R)) {
        ObjList *ls = (ObjList*)AS_OBJ(L);
        ObjList *rs = (ObjList*)AS_OBJ(R);
        ObjList *result = new_list(ls->count + rs->count);
        for (int i = 0; i < ls->count; i++) {
            list_add(result, ls->items ? ls->items[i] : ls->inline_items[i]);
        }
        for (int i = 0; i < rs->count; i++) {
            list_add(result, rs->items ? rs->items[i] : rs->inline_items[i]);
        }
        return make_obj((Object*)result);
    }
    /* List/string repetition for MUL */
    if (op == OP_MUL) {
        ObjList *lst = NULL;
        ObjString *str = NULL;
        int64_t times = 0;
        if (IS_LIST(L) && IS_INT(R)) {
            lst = (ObjList*)AS_OBJ(L);
            times = AS_INT(R);
        } else if (IS_LIST(R) && IS_INT(L)) {
            lst = (ObjList*)AS_OBJ(R);
            times = AS_INT(L);
        } else if (IS_STRING(L) && IS_INT(R)) {
            str = (ObjString*)AS_OBJ(L);
            times = AS_INT(R);
        } else if (IS_STRING(R) && IS_INT(L)) {
            str = (ObjString*)AS_OBJ(R);
            times = AS_INT(L);
        }
        if (lst && times >= 0) {
            ObjList *result = new_list(lst->count * (int)times);
            for (int t = 0; t < times; t++) {
                for (int i = 0; i < lst->count; i++) {
                    list_add(result, lst->items ? lst->items[i] : lst->inline_items[i]);
                }
            }
            return make_obj((Object*)result);
        }
        if (lst && times < 0) return make_obj((Object*)new_list(0));
        if (str && times >= 0) {
            int len = str->length * (int)times;
            char *buf = malloc(len + 1);
            if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
            buf[0] = '\0';
            for (int t = 0; t < times; t++) {
                memcpy(buf + t * str->length, str->chars, str->length);
            }
            buf[len] = '\0';
            ObjString *result = new_string(buf, len);
            free(buf);
            return make_obj((Object*)result);
        }
        if (str && times < 0) return make_obj((Object*)new_string("", 0));
    }
    if (!is_num(L) || !is_num(R)) return make_null();
    /* Integer path */
    if (IS_INT(L) && IS_INT(R)) {
        int64_t li = AS_INT(L);
        int64_t ri = AS_INT(R);
        switch (op) {
            case OP_ADD: return make_int(li + ri);
            case OP_SUB: return make_int(li - ri);
            case OP_MUL: return make_int(li * ri);
            case OP_DIV: {
                if (!ri) return make_null();
                return make_int(li / ri);
            }
            case OP_MOD: {
                if (!ri) return make_null();
                return make_int(li % ri);
            }
            default: return make_null();
        }
    }
    /* Floating-point path */
    double l = to_f64(L);
    double r = to_f64(R);
    switch (op) {
        case OP_ADD: return make_double(l + r);
        case OP_SUB: return make_double(l - r);
        case OP_MUL: return make_double(l * r);
        case OP_DIV: {
            if (r == 0.0) {
                if (l == 0.0) return make_double(0.0/0.0);
                else if (l > 0.0) return make_pos_inf();
                else return make_neg_inf();
            }
            return make_double(l / r);
        }
        case OP_MOD: {
            if (r == 0.0) return make_double(0.0/0.0);
            return make_double(fmod(l, r));
        }
        default: return make_null();
    }
}
static inline Value do_cmp(Value L, Value R, OpCode op) {
    /* Integer fast path: avoid float conversion for int vs int */
    if (IS_INT(L) && IS_INT(R)) {
        int32_t a = AS_INT(L), b = AS_INT(R);
        switch (op) {
            case OP_LT: return make_bool(a <  b);
            case OP_LE: return make_bool(a <= b);
            case OP_GT: return make_bool(a >  b);
            case OP_GE: return make_bool(a >= b);
            case OP_EQ: return make_bool(a == b);
            case OP_NE: return make_bool(a != b);
            default: break;
        }
    }
    if (is_num(L) && is_num(R)) {
        double a = to_f64(L), b = to_f64(R);
        switch (op) { case OP_LT: return make_bool(a < b); case OP_LE: return make_bool(a <= b); case OP_GT: return make_bool(a > b); case OP_GE: return make_bool(a >= b); default: break; }
    }
    /* Fast path: interned string equality is pointer comparison */
    if (IS_STRING(L) && IS_STRING(R)) {
        bool same = AS_OBJ(L) == AS_OBJ(R);
        switch (op) {
            case OP_EQ: return make_bool(same);
            case OP_NE: return make_bool(!same);
            default: break;
        }
    }
    switch (op) {
        case OP_EQ: return make_bool(values_equal(L, R));
        case OP_NE: return make_bool(!values_equal(L, R));
        default: return make_null();
    }
}

/* ============================================================ */
/* GC (Mark & Sweep)                                             */
/* ============================================================ */

extern Object *all_objects;
extern int allocated_objects;

static void mark_object(Object *obj);

static void mark_value(Value v) {
    if (IS_OBJ(v) && AS_OBJ(v)) {
        mark_object(AS_OBJ(v));
    }
}

static void mark_object(Object *obj) {
    if (!obj || obj->is_marked) return;
    obj->is_marked = true;

    switch (obj->type) {
        case OBJ_LIST: {
            ObjList *l = (ObjList *)obj;
            if (l->items) {
                for (int i = 0; i < l->count; i++) mark_value(l->items[i]);
            } else {
                for (int i = 0; i < l->count; i++) mark_value(l->inline_items[i]);
            }
            break;
        }
        case OBJ_DICT: {
            ObjDict *d = (ObjDict *)obj;
            if (d->indices == NULL) {
                for (int i = 0; i < d->entry_count; i++) {
                    mark_value(d->inline_entries[i].key);
                    mark_value(d->inline_entries[i].value);
                }
            } else {
                for (int i = 0; i < d->next_entry; i++) {
                    if (d->entries[i].key != EMPTY_VAL) {
                        mark_value(d->entries[i].key);
                        mark_value(d->entries[i].value);
                    }
                }
            }
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance *inst = (ObjInstance *)obj;
            for (int i = 0; i < inst->field_count; i++) {
                mark_value(inst->fields[i]);
            }
            if (inst->klass) mark_object((Object*)inst->klass);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure *cl = (ObjClosure *)obj;
            mark_object((Object*)cl->function);
            for (int i = 0; i < cl->upvalue_count; i++) {
                mark_object((Object*)cl->upvalues[i]);
            }
            break;
        }
        case OBJ_ENUM: break; /* no child Values to mark */
        case OBJ_FUNCTION: {
            ObjFunction *f = (ObjFunction *)obj;
            if (f->chunk) {
                for (int i = 0; i < f->chunk->const_count; i++) {
                    mark_value(f->chunk->constants[i]);
                }
            }
            break;
        }
        case OBJ_UPVALUE: {
            ObjUpvalue *uv = (ObjUpvalue *)obj;
            mark_value(uv->closed);
            break;
        }
        case OBJ_CLASS: {
            ObjClass *cls = (ObjClass *)obj;
            if (cls->base) mark_object((Object*)cls->base);
            if (cls->prototype) mark_object((Object*)cls->prototype);
            for (int i = 0; i < cls->method_count; i++) {
                mark_object((Object*)cls->methods[i]);
            }
            break;
        }
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod *bm = (ObjBoundMethod *)obj;
            mark_value(bm->self);
            if (bm->fn) mark_object((Object*)bm->fn);
            break;
        }
        case OBJ_MODULE: {
            ObjModule *mod = (ObjModule *)obj;
            if (mod->name) mark_object((Object*)mod->name);
            if (mod->exports) mark_object((Object*)mod->exports);
            break;
        }
        default: break;
    }
}

void mark_and_sweep(VM *vm) {
    if (gc_collecting) return;

    // 1. Mark roots
    for (int i = 0; i < vm->stack_count; i++) {
        mark_value(vm->stack[i]);
    }
    for (int i = 0; i < VM_GLOBAL_BUCKETS; i++) {
        GlobalEntry *e = vm->globals[i];
        while (e) {
            mark_value(e->value);
            e = e->next;
        }
    }
    for (int i = 0; i < vm->frame_count; i++) {
        if (vm->frames[i].closure) mark_object((Object*)vm->frames[i].closure);
        if (vm->frames[i].fn) mark_object((Object*)vm->frames[i].fn);
        if (vm->frames[i].chunk) {
            for (int j = 0; j < vm->frames[i].chunk->const_count; j++) {
                mark_value(vm->frames[i].chunk->constants[j]);
            }
        }
        mark_value(vm->frames[i].kw_args);
    }
    ObjUpvalue *uv = vm->open_upvalues;
    while (uv) {
        mark_object((Object*)uv);
        uv = uv->next;
    }
    mark_value(vm->last_exception);
    if (vm->module_cache) mark_object((Object*)vm->module_cache);
    if (vm->exception_class) mark_object((Object*)vm->exception_class);
    if (vm->type_error_class) mark_object((Object*)vm->type_error_class);
    if (vm->key_error_class) mark_object((Object*)vm->key_error_class);
    if (vm->index_error_class) mark_object((Object*)vm->index_error_class);
    if (vm->attribute_error_class) mark_object((Object*)vm->attribute_error_class);
    if (vm->value_error_class) mark_object((Object*)vm->value_error_class);
    if (vm->runtime_error_class) mark_object((Object*)vm->runtime_error_class);

    // Invalidate inline caches — any cached object may have been collected.
    memset(vm->global_ic, 0, sizeof(vm->global_ic));
    memset(vm->member_ic, 0, sizeof(vm->member_ic));
    memset(vm->method_ic, 0, sizeof(vm->method_ic));

    // 2. Collect unmarked garbage
    Object *garbage = NULL;
    Object *obj = all_objects;
    while (obj) {
        if (!obj->is_marked) {
            // Isolate it from the doubly linked list
            if (obj->prev) obj->prev->next = obj->next;
            else all_objects = obj->next;
            if (obj->next) obj->next->prev = obj->prev;

            Object *next = obj->next;
            // Add to garbage list (using next field temporarily)
            obj->next = garbage;
            obj->prev = NULL;
            garbage = obj;
            obj = next;
        } else {
            obj->is_marked = false; // Reset for next GC
            obj = obj->next;
        }
    }

    /* 3. Free garbage
     *
     * During sweep we release children first (breaking cycles) and then
     * free container memory.  We set gc_collecting so that release_obj()
     * never re-enters free_object() while we are mid-sweep.  After children
     * are released we call free_object_container() which only frees memory
     * and does NOT release children again, avoiding the double-decrement
     * bug that the old 1M-refcount hack had.
     */
    gc_collecting = true;

    Object *g = garbage;
    while (g) {
        switch (g->type) {
            case OBJ_LIST: {
                ObjList *l = (ObjList *)g;
                if (l->items) {
                    for (int i = 0; i < l->count; i++) release_value(l->items[i]);
                } else {
                    for (int i = 0; i < l->count; i++) release_value(l->inline_items[i]);
                }
                l->count = 0;
                break;
            }
            case OBJ_DICT: {
                ObjDict *d = (ObjDict *)g;
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
                d->entry_count = 0;
                d->next_entry = 0;
                break;
            }
            case OBJ_INSTANCE: {
                ObjInstance *inst = (ObjInstance *)g;
                for (int i = 0; i < inst->field_count; i++) release_value(inst->fields[i]);
                inst->field_count = 0;
                if (inst->klass) release_obj((Object*)inst->klass);
                inst->klass = NULL;
                break;
            }
            case OBJ_CLOSURE: {
                ObjClosure *cl = (ObjClosure *)g;
                if (cl->function) release_obj((Object*)cl->function);
                cl->function = NULL;
                for (int i = 0; i < cl->upvalue_count; i++) {
                    if (cl->upvalues[i]) release_obj((Object*)cl->upvalues[i]);
                }
                cl->upvalue_count = 0;
                break;
            }
            case OBJ_UPVALUE: {
                ObjUpvalue *uv = (ObjUpvalue *)g;
                release_value(uv->closed);
                uv->closed = make_null();
                break;
            }
            case OBJ_CLASS: {
                ObjClass *cls = (ObjClass *)g;
                if (cls->prototype) release_obj((Object*)cls->prototype);
                cls->prototype = NULL;
                if (cls->base) release_obj((Object*)cls->base);
                cls->base = NULL;
                for (int i = 0; i < cls->method_count; i++) {
                    if (cls->methods[i]) release_obj((Object*)cls->methods[i]);
                }
                cls->method_count = 0;
                break;
            }
            default: break;
        }
        g = g->next;
    }

    g = garbage;
    while (g) {
        Object *next = g->next;
        g->next = NULL;
        free_object_container(g);
        g = next;
    }

    gc_collecting = false;

    if (bytes_allocated > next_gc_threshold) {
        next_gc_threshold = bytes_allocated * 2;
    } else {
        next_gc_threshold = 64 * 1024 * 1024; // Reset to 64MB
    }
}

/* ============================================================ */
/* VM init / free                                                */
/* ============================================================ */

static ObjClass *vm_register_builtin_exception(VM *vm, const char *name, ObjClass *base) {
    ObjClass *cls = new_class(name, NULL);
    if (base) {
        cls->base = base;
        retain_obj((Object*)base);
        /* Copy prototype fields from base */
        if (base->prototype) {
            ObjInstance *bp = base->prototype;
            cls->prototype = new_instance(cls, bp->field_capacity > 4 ? bp->field_capacity : 4);
            for (int i = 0; i < bp->field_count; i++) {
                instance_set_field(cls->prototype, bp->field_names[i], bp->fields[i]);
            }
        }
    } else {
        cls->prototype = new_instance(cls, 4);
        instance_set_field(cls->prototype, "message", make_null());
        instance_set_field(cls->prototype, "stack_trace", make_null());
        instance_set_field(cls->prototype, "file", make_null());
        instance_set_field(cls->prototype, "line", make_int(0));
    }
    retain_obj((Object*)cls->prototype);
    Value cls_val = make_obj((Object*)cls);
    retain_obj((Object*)cls);
    vm_set_global(vm, name, cls_val, false);
    return cls;
}

void vm_init(VM *vm) {
    memset(vm, 0, sizeof(VM));
    vm_register_builtins(vm);

    /* Register built-in Exception hierarchy */
    vm->exception_class      = vm_register_builtin_exception(vm, "Exception", NULL);
    vm->type_error_class     = vm_register_builtin_exception(vm, "TypeError", vm->exception_class);
    vm->key_error_class      = vm_register_builtin_exception(vm, "KeyError", vm->exception_class);
    vm->index_error_class    = vm_register_builtin_exception(vm, "IndexError", vm->exception_class);
    vm->attribute_error_class = vm_register_builtin_exception(vm, "AttributeError", vm->exception_class);
    vm->value_error_class    = vm_register_builtin_exception(vm, "ValueError", vm->exception_class);
    vm->runtime_error_class  = vm_register_builtin_exception(vm, "RuntimeError", vm->exception_class);
    vm->argument_error_class = vm_register_builtin_exception(vm, "ArgumentError", vm->exception_class);

    vm->module_cache = new_dict();
    retain_obj((Object*)vm->module_cache);
    vm_register_math_module(vm);
    vm_register_random_module(vm);
}

static void close_upvalues(VM *vm, int frame_depth);

static void vm_pop_try_frames(VM *vm, int min_depth) {
    while (vm->try_stack && vm->try_stack->frame_depth > min_depth) {
        TryFrame *tf = vm->try_stack;
        vm->try_stack = tf->next;
        free(tf);
    }
}

void vm_free(VM *vm) {
    for (int i = 0; i < VM_GLOBAL_BUCKETS; i++) {
        GlobalEntry *e = vm->globals[i];
        while (e) {
            GlobalEntry *nx = e->next;
            if (IS_OBJ(e->value) && AS_OBJ(e->value)) release_obj(AS_OBJ(e->value));
            free(e->name); free(e); e = nx;
        }
        vm->globals[i] = NULL;
    }

    if (vm->module_cache) {
        release_obj((Object*)vm->module_cache);
        vm->module_cache = NULL;
    }

    close_upvalues(vm, 0);

    // Release any values still on the stack so their refcounts drop
    for (int i = 0; i < vm->stack_count; i++) {
        if (IS_OBJ(vm->stack[i]) && AS_OBJ(vm->stack[i]))
            release_obj(AS_OBJ(vm->stack[i]));
    }
    free(vm->stack);
    vm->stack = NULL;

    while (vm->try_stack) {
        TryFrame *tf = vm->try_stack;
        vm->try_stack = tf->next;
        free(tf);
    }

    if (vm->exception_class) {
        release_obj((Object*)vm->exception_class);
        vm->exception_class = NULL;
    }

    value_free_intern_table();

    // Free any remaining objects (shutdown path — no cascading releases).
    gc_collecting = true;
    Object *obj = all_objects;
    while (obj) {
        Object *next = obj->next;
        obj->next = NULL;
        obj->prev = NULL;
        free_object_container(obj);
        obj = next;
    }
    all_objects = NULL;
    allocated_objects = 0;
    bytes_allocated = 0;
    gc_collecting = false;
}

/* ============================================================ */
/* Module import helpers                                          */
/* ============================================================ */

static GlobalEntry **vm_globals_save(VM *vm) {
    GlobalEntry **saved = malloc(sizeof(GlobalEntry *) * VM_GLOBAL_BUCKETS);
    if (!saved) { fprintf(stderr, "OOM\n"); exit(1); }
    memcpy(saved, vm->globals, sizeof(GlobalEntry *) * VM_GLOBAL_BUCKETS);
    return saved;
}

static void vm_globals_restore(VM *vm, GlobalEntry **saved) {
    for (int i = 0; i < VM_GLOBAL_BUCKETS; i++) {
        GlobalEntry *e = vm->globals[i];
        while (e) {
            GlobalEntry *next = e->next;
            release_value(e->value);
            free(e->name);
            free(e);
            e = next;
        }
    }
    memcpy(vm->globals, saved, sizeof(GlobalEntry *) * VM_GLOBAL_BUCKETS);
    free(saved);
}

static void vm_globals_fresh(VM *vm) {
    memset(vm->globals, 0, sizeof(GlobalEntry *) * VM_GLOBAL_BUCKETS);
}

static ObjDict *vm_globals_to_dict(VM *vm) {
    ObjDict *d = new_dict();
    for (int i = 0; i < VM_GLOBAL_BUCKETS; i++) {
        for (GlobalEntry *e = vm->globals[i]; e; e = e->next) {
            ObjString *key = new_string(e->name, (int)strlen(e->name));
            dict_set(d, make_obj((Object*)key), e->value);
        }
    }
    return d;
}

static void frame_set_refs(CallFrame *frame, ObjClosure *closure, ObjFunction *fn) {
    frame->closure = closure;
    frame->fn = fn;
    if (closure) retain_obj((Object*)closure);
    if (fn) retain_obj((Object*)fn);
}

static void frame_release_refs(CallFrame *frame) {
    if (frame->closure) {
        release_obj((Object*)frame->closure);
        frame->closure = NULL;
    }
    if (frame->fn) {
        release_obj((Object*)frame->fn);
        frame->fn = NULL;
    }
}

/* Extract directory component from a file path (modifies buf in-place).
 * Handles both '/' and '\\' separators.
 */
static void path_dirname(const char *path, char *buf, size_t buf_size) {
    if (!path || !path[0]) {
        buf[0] = '\0';
        return;
    }
    strncpy(buf, path, buf_size - 1);
    buf[buf_size - 1] = '\0';
    char *last_slash = strrchr(buf, '/');
    char *last_backslash = strrchr(buf, '\\');
    char *sep = last_slash > last_backslash ? last_slash : last_backslash;
    if (sep) {
        *sep = '\0';
    } else {
        buf[0] = '\0';
    }
}

/* ============================================================ */
/* vm_run_chunk — main dispatch loop                             */
/* ============================================================ */

#define FRAME       vm->frames[vm->frame_count-1]
#define CHUNK       (FRAME.chunk)
#define IP          (FRAME.ip)
#define RA          (A)
#define RB          (B)
#define RC          (C)
#define RKB         (ISK(B) ? CONST(Bx & 0xFF) : REG(B))
#define RKC         (ISK(C) ? CONST(Bx & 0xFF) : REG(C))
#define REG(i)      (vm->stack[FRAME.base + (i)])
#define SET_REG(i, v) do { \
    Value _new = (v); \
    retain_value(_new); \
    Value _old = REG(i); \
    REG(i) = _new; \
    release_value(_old); \
} while (0)
/* For values that are guaranteed NOT to be heap objects (int, bool, null, double).
   Skips the no-op retain_value branch. */
#define SET_REG_PRIM(i, v) do { \
    Value _old = REG(i); \
    REG(i) = (v); \
    release_value(_old); \
} while (0)
#define SET_STACK(idx, v) do { \
    Value _new = (v); \
    retain_value(_new); \
    Value _old = vm->stack[idx]; \
    vm->stack[idx] = _new; \
    release_value(_old); \
} while (0)
#define CONST(i)    (CHUNK->constants[(i)])
#define SET_FIELD(inst, idx, v) do { \
    Value _new = (v); \
    retain_value(_new); \
    Value _old = (inst)->fields[idx]; \
    (inst)->fields[idx] = _new; \
    release_value(_old); \
} while (0)
#define KSTR(n)     (((ObjString*)AS_OBJ(CONST(n)))->chars)
#define KSTROBJ(n)  ((ObjString*)AS_OBJ(CONST(n)))

static VMResult vm_execute_loop(VM *vm, Chunk *chunk) {
    (void)chunk;

    uint32_t instr;
    uint8_t  A, B, C;
    uint16_t Bx;
    int      sBx;
    volatile Value _exc = make_null();


#define DECODE \
    do { \
        if ((++vm->instr_count & 4095) == 0 && bytes_allocated > next_gc_threshold) mark_and_sweep(vm); \
        instr = CHUNK->code[IP++]; \
        A   = DECODE_A(instr); \
        B   = DECODE_B(instr); \
        C   = DECODE_C(instr); \
        Bx  = DECODE_Bx(instr); \
        sBx = DECODE_sBx(instr); \
    } while (0)

#define OP(inst) DECODE_OP(inst)
#define BX       Bx
#define SBX      sBx
#define ISK(x)   ((x) & 0x100)

    static const void *op_labels[] = {
        &&op_loadk,         // 0  OP_LOADK
        &&op_loadnull,      // 1  OP_LOADNULL
        &&op_loadtrue,      // 2  OP_LOADTRUE
        &&op_loadfalse,     // 3  OP_LOADFALSE
        &&op_loadi,         // 4  OP_LOADI
        &&op_move,          // 5  OP_MOVE
        &&op_swap,          // 6  OP_SWAP
        &&op_add,           // 7  OP_ADD
        &&op_sub,           // 8  OP_SUB
        &&op_mul,           // 9  OP_MUL
        &&op_div,           // 10 OP_DIV
        &&op_mod,           // 11 OP_MOD
        &&op_neg,           // 12 OP_NEG
        &&op_band,          // 13 OP_BAND
        &&op_bor,           // 14 OP_BOR
        &&op_bxor,          // 15 OP_BXOR
        &&op_bnot,          // 16 OP_BNOT
        &&op_shl,           // 17 OP_SHL
        &&op_shr,           // 18 OP_SHR
        &&op_addi,          // 19 OP_ADDI
        &&op_subi,          // 20 OP_SUBI
        &&op_eq,            // 21 OP_EQ
        &&op_ne,            // 22 OP_NE
        &&op_lt,            // 23 OP_LT
        &&op_le,            // 24 OP_LE
        &&op_gt,            // 25 OP_GT
        &&op_ge,            // 26 OP_GE
        &&op_in,            // 27 OP_IN
        &&op_not,           // 28 OP_NOT
        &&op_jmp,           // 29 OP_JMP
        &&op_jz,            // 30 OP_JZ
        &&op_jnz,           // 31 OP_JNZ
        &&op_jnil,          // 32 OP_JNIL
        &&op_call,          // 33 OP_CALL
        &&op_ret,           // 34 OP_RET
        &&op_enter,         // 35 OP_ENTER
        &&op_leave,         // 36 OP_LEAVE
        &&op_closure,       // 37 OP_CLOSURE
        &&op_getglobal,     // 38 OP_GETGLOBAL
        &&op_setglobal,     // 39 OP_SETGLOBAL
        &&op_getupval,      // 40 OP_GETUPVAL
        &&op_setupval,      // 41 OP_SETUPVAL
        &&op_new,           // 42 OP_NEW
        &&op_newdict,       // 43 OP_NEWDICT
        &&op_newlist,       // 44 OP_NEWLIST
        &&op_listappend,    // 45 OP_LISTAPPEND
        &&op_getiter,       // 46 OP_GETITER
        &&op_forloop,       // 47 OP_FORLOOP
        &&op_indexget,      // 48 OP_INDEXGET
        &&op_indexset,      // 49 OP_INDEXSET
        &&op_slice,         // 50 OP_SLICE
        &&op_memberget,     // 51 OP_MEMBERGET
        &&op_memberset,     // 52 OP_MEMBERSET
        &&op_invoke,        // 53 OP_INVOKE
        &&op_super,         // 54 OP_SUPER
        &&op_throw,         // 55 OP_THROW
        &&op_try,           // 56 OP_TRY
        &&op_endtry,        // 57 OP_ENDTRY
        &&op_isinstance,    // 58 OP_ISINSTANCE
        &&op_default,       // 59 OP_DEFAULT
        &&op_kwargs,        // 60 OP_KWARGS
        &&op_kcall,         // 61 OP_KCALL
        &&op_coalesce,      // 62 OP_COALESCE
        &&op_memberget_safe,// 63 OP_MEMBERGET_SAFE
        &&op_indexget_safe, // 64 OP_INDEXGET_SAFE
        &&op_slice_safe,    // 65 OP_SLICE_SAFE
        &&op_import,        // 66 OP_IMPORT
        &&op_halt           // 67 OP_HALT
    };

    DECODE;
    goto *op_labels[OP(instr)];


#include "vm_opcodes.inc"

    
#undef DECODE
#undef FRAME
#undef CHUNK
#undef IP
#undef RA
#undef RB
#undef RC
#undef RKB
#undef RKC
#undef REG
#undef CONST
#undef KSTR
}
VMResult vm_run_chunk(VM *vm, Chunk *chunk) {
    if (allocated_objects > 1000) {
        mark_and_sweep(vm);
    }
    if (vm->frame_count >= MAX_FRAMES) {
        vm->last_exception = make_exception_instance(vm, vm->exception_class, "vm: frame overflow");
        return VM_EXCEPTION;
    }
    CallFrame *frame = &vm->frames[vm->frame_count++];
    frame->chunk = chunk;
    frame->ip = 0;
    frame->base = vm->stack_count;
    frame->ret_reg = 0;
    frame->nargs = 0;
    frame->kw_args = make_null();
    frame->closure = NULL;
    frame->fn = NULL;
    frame->saved_globals = NULL;
    int needed = frame->base + chunk->max_registers;
    if (needed > vm->stack_cap) {
        vm->stack_cap = needed < 64 ? 64 : needed * 2;
        vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
    }
    for (int i = 0; i < chunk->max_registers; i++) vm->stack[frame->base + i] = make_null();
    vm->stack_count = needed;

    LunaJump jump;
    jump.prev = vm->native_jump;
    vm->native_jump = &jump;

    VMResult result;
    if (setjmp(jump.env) == 0) {
        result = vm_execute_loop(vm, chunk);
    } else {
        result = VM_EXCEPTION;
    }
    vm->native_jump = jump.prev;
    return result;
}


/* ============================================================ */
/* Upvalue helpers                                              */
/* ============================================================ */

ObjUpvalue *capture_upvalue(VM *vm, int stack_idx) {
    ObjUpvalue *prev = NULL;
    ObjUpvalue *uv = vm->open_upvalues;
    while (uv && uv->stack_index > stack_idx) {
        prev = uv;
        uv = uv->next;
    }
    if (uv && uv->is_open && uv->stack_index == stack_idx) return uv;
    
    ObjUpvalue *created = new_upvalue(stack_idx);
    created->next = uv;
    created->frame_depth = vm->frame_count;
    retain_obj((Object*)created); // Owned by the open_upvalues list
    if (prev) prev->next = created;
    else vm->open_upvalues = created;
    return created;
}

static void close_upvalues(VM *vm, int frame_depth) {
    while (vm->open_upvalues && vm->open_upvalues->frame_depth >= frame_depth) {
        ObjUpvalue *uv = vm->open_upvalues;
        uv->closed = vm->stack[uv->stack_index];
        if (IS_OBJ(uv->closed) && AS_OBJ(uv->closed)) retain_obj(AS_OBJ(uv->closed));
        uv->is_open = false;
        vm->open_upvalues = uv->next;
        release_obj((Object*)uv); // No longer owned by the list
    }
}

/* ============================================================ */
/* Exception helpers                                             */
/* ============================================================ */

void vm_push_try(VM *vm, int catch_ip) {
    TryFrame *tf = malloc(sizeof(TryFrame));
    tf->catch_ip = catch_ip;
    tf->frame_depth = vm->frame_count;
    tf->stack_count = vm->stack_count;
    tf->next = vm->try_stack;
    vm->try_stack = tf;
}

void vm_pop_try(VM *vm) {
    if (vm->try_stack) {
        TryFrame *tf = vm->try_stack;
        vm->try_stack = tf->next;
        free(tf);
    }
}

int vm_throw(VM *vm, Value exception) {
    if (vm->try_stack && vm->try_stack->frame_depth <= vm->frame_count) {
        TryFrame *tf = vm->try_stack;
        vm->try_stack = tf->next;
        vm_pop_try_frames(vm, tf->frame_depth);
        while (vm->frame_count > tf->frame_depth) {
            vm->frame_count--;
        }
        vm->stack_count = tf->stack_count;
        CallFrame *frame = &vm->frames[vm->frame_count - 1];
        frame->ip = tf->catch_ip;
        free(tf);
        return 1;
    }
    /* Extract message from Exception instance if applicable */
    if (IS_INSTANCE(exception) && vm->exception_class) {
        ObjInstance *inst = (ObjInstance*)AS_OBJ(exception);
        if (inst->klass == vm->exception_class) {
            Value msg = instance_get_field(inst, "message");
            if (IS_STRING(msg)) {
                fprintf(stderr, "Uncaught exception: %s\n", ((ObjString*)AS_OBJ(msg))->chars);
            } else {
                fprintf(stderr, "Uncaught exception\n");
            }
            return 1;
        }
    }
    char *s = value_to_string(exception);
    fprintf(stderr, "Uncaught exception: %s\n", s);
    free(s);
    return 1;
}

/* Native exception throw — callable from C builtin functions.
 * Uses longjmp to safely return to the VM bytecode loop,
 * which then reuses the existing op_throw unwinding logic. */
void luna_throw(VM *vm, ObjClass *error_class, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    char buf[256];
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);

    vm->last_exception = make_exception_instance(vm, error_class, buf);

    if (vm->native_jump) {
        longjmp(vm->native_jump->env, 1);
    } else {
        fprintf(stderr, "Fatal uncaught exception (no active VM frame): %s\n", buf);
        abort();
    }
}
