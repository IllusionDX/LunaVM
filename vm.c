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

#define _POSIX_C_SOURCE 200809L

#pragma GCC diagnostic ignored "-Wclobbered"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "vm.h"
#include "value.h"
#include "chunk.h"
#include "compiler.h"
#include "module.h"
#include "opcode.h"
#include "stdlib_math.h"
#include "stdlib_random.h"
#include "stdlib_noise.h"
#include "stdlib_io.h"
#include "stdlib_time.h"
#include "stdlib_os.h"
#include "stdlib_buffer.h"
#include "stdlib_string.h"
#include "stdlib_net.h"
#include "stdlib_json.h"
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
    if (IS_INT64(v)) return (double)((int64_t)((ObjInt64*)AS_OBJ(v))->value);
    if (IS_DOUBLE(v)) return AS_DOUBLE(v);
    return 0.0;
}
static inline int64_t to_i64(Value v) {
    if (IS_INT(v)) return (int64_t)AS_INT(v);
    if (IS_INT64(v)) return (int64_t)((ObjInt64*)AS_OBJ(v))->value;
    if (IS_DOUBLE(v)) return (int64_t)AS_DOUBLE(v);
    return 0;
}
static inline bool is_num(Value v) {
    return IS_NUMBER(v);
}
static inline bool is_int_type(Value v) { return IS_INT(v) || IS_INT64(v); }

static inline bool fits_int32(int64_t v) {
    return v >= INT32_MIN && v <= INT32_MAX;
}

static inline Value make_int_result(int64_t v) {
    return fits_int32(v) ? make_int((int32_t)v) : make_int64(v);
}

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
    /* Vector operations */
    if (IS_VECTOR(L) && IS_VECTOR(R)) {
        ObjVector *lv = (ObjVector*)AS_OBJ(L);
        ObjVector *rv = (ObjVector*)AS_OBJ(R);
        float rx, ry, rz, rw;
        switch (op) {
            case OP_ADD: rx = lv->data[0] + rv->data[0]; ry = lv->data[1] + rv->data[1]; rz = lv->data[2] + rv->data[2]; rw = lv->data[3] + rv->data[3]; break;
            case OP_SUB: rx = lv->data[0] - rv->data[0]; ry = lv->data[1] - rv->data[1]; rz = lv->data[2] - rv->data[2]; rw = lv->data[3] - rv->data[3]; break;
            case OP_MUL: rx = lv->data[0] * rv->data[0]; ry = lv->data[1] * rv->data[1]; rz = lv->data[2] * rv->data[2]; rw = lv->data[3] * rv->data[3]; break;
            default: return make_null();
        }
        return make_obj((Object*)new_vector(rx, ry, rz, rw));
    }
    if (IS_VECTOR(L) && is_num(R)) {
        ObjVector *lv = (ObjVector*)AS_OBJ(L);
        float s = (float)value_to_double(R);
        float rx, ry, rz, rw;
        switch (op) {
            case OP_MUL: rx = lv->data[0] * s; ry = lv->data[1] * s; rz = lv->data[2] * s; rw = lv->data[3] * s; break;
            case OP_DIV: rx = lv->data[0] / s; ry = lv->data[1] / s; rz = lv->data[2] / s; rw = lv->data[3] / s; break;
            default: return make_null();
        }
        return make_obj((Object*)new_vector(rx, ry, rz, rw));
    }
    /* Matrix operations */
    if (IS_MATRIX(L) && IS_MATRIX(R)) {
        if (op != OP_MUL) return make_null();
        ObjMatrix *ma = (ObjMatrix*)AS_OBJ(L);
        ObjMatrix *mb = (ObjMatrix*)AS_OBJ(R);
        ObjMatrix *res = new_matrix();
        for (int col = 0; col < 4; col++) {
            for (int row = 0; row < 4; row++) {
                float sum = 0.0f;
                for (int k = 0; k < 4; k++)
                    sum += ma->m[row + k * 4] * mb->m[k + col * 4];
                res->m[row + col * 4] = sum;
            }
        }
        return make_obj((Object*)res);
    }
    if (IS_MATRIX(L) && IS_VECTOR(R)) {
        if (op != OP_MUL) return make_null();
        ObjMatrix *m = (ObjMatrix*)AS_OBJ(L);
        ObjVector *v = (ObjVector*)AS_OBJ(R);
        float rx = m->m[0]*v->data[0] + m->m[4]*v->data[1] + m->m[8]*v->data[2] + m->m[12]*v->data[3];
        float ry = m->m[1]*v->data[0] + m->m[5]*v->data[1] + m->m[9]*v->data[2] + m->m[13]*v->data[3];
        float rz = m->m[2]*v->data[0] + m->m[6]*v->data[1] + m->m[10]*v->data[2] + m->m[14]*v->data[3];
        float rw = m->m[3]*v->data[0] + m->m[7]*v->data[1] + m->m[11]*v->data[2] + m->m[15]*v->data[3];
        return make_obj((Object*)new_vector(rx, ry, rz, rw));
    }
    if (IS_MATRIX(L) && is_num(R)) {
        if (op != OP_MUL) return make_null();
        ObjMatrix *m = (ObjMatrix*)AS_OBJ(L);
        float s = (float)value_to_double(R);
        ObjMatrix *res = new_matrix();
        for (int i = 0; i < 16; i++) res->m[i] = m->m[i] * s;
        return make_obj((Object*)res);
    }
    if (!is_num(L) || !is_num(R)) return make_null();
    /* Integer path */
    if (is_int_type(L) && is_int_type(R)) {
        int64_t li = to_i64(L);
        int64_t ri = to_i64(R);
        switch (op) {
            case OP_ADD: return make_int_result(li + ri);
            case OP_SUB: return make_int_result(li - ri);
            case OP_MUL: return make_int_result(li * ri);
            case OP_DIV: {
                if (!ri) return make_null();
                return make_int_result(li / ri);
            }
            case OP_MOD: {
                if (!ri) return make_null();
                return make_int_result(li % ri);
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
    if (is_int_type(L) && is_int_type(R)) {
        int64_t a = to_i64(L), b = to_i64(R);
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
extern Object *userdata_objects;
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
            if (cls->fields) mark_object((Object*)cls->fields);
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
        case OBJ_BUFFER: break;
        case OBJ_INT64: break;
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

    /* 3. Free garbage */
    gc_collecting = true;

    Object *g = garbage;
    while (g) {
        Object *next = g->next;
        g->next = NULL;
        g->prev = NULL;
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
    Value cls_val = make_obj((Object*)cls);
    vm_set_global(vm, name, cls_val, false);
    return cls;
}

void vm_init(VM *vm) {
    memset(vm, 0, sizeof(VM));
    vm->time_start_us = luna_time_monotonic_us();
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
    vm_register_math_module(vm);
    vm_register_random_module(vm);
    vm_register_noise_module(vm);
    vm_register_io_module(vm);
    vm_register_time_module(vm);
    vm_register_os_module(vm);
    vm_register_buffer_module(vm);
    vm_register_string_module(vm);
    vm_register_net_module(vm);
    vm_register_json_module(vm);
}

void vm_set_process_args(VM *vm, int argc, char **argv) {
    vm->process_argc = argc > 0 ? argc : 0;
    vm->process_argv = argv;
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
            free(e->name); free(e); e = nx;
        }
        vm->globals[i] = NULL;
    }

    vm->module_cache = NULL;

    close_upvalues(vm, 0);

    free(vm->stack);
    vm->stack = NULL;

    while (vm->try_stack) {
        TryFrame *tf = vm->try_stack;
        vm->try_stack = tf->next;
        free(tf);
    }

    vm->exception_class = NULL;

    value_free_intern_table();

    // Free any remaining objects (shutdown path).
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
}

static void frame_release_refs(CallFrame *frame) {
    frame->closure = NULL;
    frame->fn = NULL;
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
#define REG(i)      (vm->stack[FRAME.base + (i)])
#define SET_REG(i, v) do { \
    REG(i) = (v); \
} while (0)
#define SET_REG_PRIM(i, v) do { \
    REG(i) = (v); \
} while (0)
#define SET_STACK(idx, v) do { \
    vm->stack[idx] = (v); \
} while (0)
#define CONST(i)    (CHUNK->constants[(i)])
#define SET_FIELD(inst, idx, v) do { \
    (inst)->fields[idx] = (v); \
} while (0)
#define KSTR(n)     (((ObjString*)AS_OBJ(CONST(n)))->chars)
#define KSTROBJ(n)  ((ObjString*)AS_OBJ(CONST(n)))

/* ============================================================ */
/* Matrix math helpers                                           */
/* ============================================================ */

static float mat4_det3(float a, float b, float c,
                        float d, float e, float f,
                        float g, float h, float i) {
    return a*(e*i - f*h) - b*(d*i - f*g) + c*(d*h - e*g);
}

static int mat4_invert(float *m, float *out) {
    float cof[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            int c0 = (c+1)%4, c1 = (c+2)%4, c2 = (c+3)%4;
            int r0 = (r+1)%4, r1 = (r+2)%4, r2 = (r+3)%4;
            cof[r + c*4] = mat4_det3(
                m[r0 + c0*4], m[r0 + c1*4], m[r0 + c2*4],
                m[r1 + c0*4], m[r1 + c1*4], m[r1 + c2*4],
                m[r2 + c0*4], m[r2 + c1*4], m[r2 + c2*4])
                * (((c+r) % 2 == 0) ? 1.0f : -1.0f);
        }
    }
    float det = 0.0f;
    for (int i = 0; i < 4; i++)
        det += m[i*4] * cof[i*4];
    if (det == 0.0f) return 0;
    float inv_det = 1.0f / det;
    for (int i = 0; i < 16; i++)
        out[i] = cof[i] * inv_det;
    return 1;
}

static void mat4_transpose(float *m, float *out) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            out[r + c*4] = m[c + r*4];
}

static void mat4_mul_translate(float *m, float tx, float ty, float tz) {
    for (int i = 0; i < 4; i++)
        m[12 + i] = m[0 + i]*tx + m[4 + i]*ty + m[8 + i]*tz + m[12 + i];
}

static void mat4_mul_rotate_x(float *m, float angle) {
    float c = cosf(angle), s = sinf(angle);
    for (int i = 0; i < 4; i++) {
        float a = m[4 + i], b = m[8 + i];
        m[4 + i] = a * c + b * s;
        m[8 + i] = -a * s + b * c;
    }
}

static void mat4_mul_rotate_y(float *m, float angle) {
    float c = cosf(angle), s = sinf(angle);
    for (int i = 0; i < 4; i++) {
        float a = m[0 + i], b = m[8 + i];
        m[0 + i] = a * c - b * s;
        m[8 + i] = a * s + b * c;
    }
}

static void mat4_mul_rotate_z(float *m, float angle) {
    float c = cosf(angle), s = sinf(angle);
    for (int i = 0; i < 4; i++) {
        float a = m[0 + i], b = m[4 + i];
        m[0 + i] = a * c + b * s;
        m[4 + i] = -a * s + b * c;
    }
}

static void mat4_mul_scale(float *m, float sx, float sy, float sz) {
    for (int i = 0; i < 4; i++)
        m[0 + i] *= sx;
    for (int i = 0; i < 4; i++)
        m[4 + i] *= sy;
    for (int i = 0; i < 4; i++)
        m[8 + i] *= sz;
}

static VMResult vm_execute_loop(VM *vm, Chunk *chunk) {
    (void)chunk;

    uint32_t instr;
    volatile Value _exc = make_null();

#define DECODE do { instr = CHUNK->code[IP++]; } while (0)

#define A        DECODE_A(instr)
#define B        DECODE_B(instr)
#define C        DECODE_C(instr)
#define Bx       DECODE_Bx(instr)
#define sBx      DECODE_sBx(instr)

#define RA       (A)
#define RB       (B)
#define RC       (C)
#define RKB      REG(B)
#define RKC      REG(C)

#define CHECK_GC \
    do { \
        if (LUNA_UNLIKELY(bytes_allocated > next_gc_threshold)) mark_and_sweep(vm); \
    } while (0)

#define OP(inst) DECODE_OP(inst)
#define BX       Bx
#define SBX      sBx

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
        &&op_tryinit,       // 67 OP_TRYINIT
        &&op_halt,          // 68 OP_HALT
        &&op_lt_jz,         // 69 OP_LT_JZ
        &&op_le_jz,         // 70 OP_LE_JZ
        &&op_gt_jz,         // 71 OP_GT_JZ
        &&op_ge_jz          // 72 OP_GE_JZ
    };

    DECODE;
    goto *op_labels[OP(instr)];


#define CHECK_FRAME_OVERFLOW() do { \
    if (LUNA_UNLIKELY((vm)->frame_count >= MAX_FRAMES)) { \
        _exc = make_exception_instance(vm, vm->exception_class, "call stack overflow"); \
        goto op_throw; \
    } \
} while (0)

#define PUSH_FRAME(fn, cl, base_reg, retreg, n, extra) do { \
    CHECK_FRAME_OVERFLOW(); \
    CallFrame *_c = &(vm)->frames[(vm)->frame_count]; \
    _c->chunk = (fn)->chunk; \
    _c->ip = 0; \
    _c->base = (base_reg); \
    _c->ret_reg = (retreg); \
    _c->nargs = (n); \
    _c->kw_args = make_null(); \
    frame_set_refs(_c, (cl), (fn)); \
    _c->saved_globals = NULL; \
    int _needed = _c->base + (fn)->chunk->max_registers; \
    if (_needed > (vm)->stack_cap) { \
        (vm)->stack_cap = _needed < 64 ? 64 : _needed * 2; \
        (vm)->stack = realloc((vm)->stack, (vm)->stack_cap * sizeof(Value)); \
    } \
    for (int _i = (n) + (extra); _i < (fn)->chunk->max_registers; _i++) \
        (vm)->stack[_c->base + _i] = make_null(); \
    (vm)->stack_count = _needed > (vm)->stack_count ? _needed : (vm)->stack_count; \
    (vm)->frame_count++; \
    CHUNK = _c->chunk; \
    IP = 0; \
} while (0)

#include "vm_opcodes.inc"

    
#undef PUSH_FRAME
#undef CHECK_FRAME_OVERFLOW
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
        vm->last_exception = make_exception_instance(vm, vm->exception_class, "frame overflow");
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
    if (prev) prev->next = created;
    else vm->open_upvalues = created;
    return created;
}

static void close_upvalues(VM *vm, int frame_depth) {
    while (vm->open_upvalues && vm->open_upvalues->frame_depth >= frame_depth) {
        ObjUpvalue *uv = vm->open_upvalues;
        uv->closed = vm->stack[uv->stack_index];
        uv->is_open = false;
        vm->open_upvalues = uv->next;
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

bool vm_call_native(VM *vm, NativeFn fn, Value *args, int arg_count, Value *out) {
    LunaJump jump;
    jump.prev = vm->native_jump;
    vm->native_jump = &jump;

    bool ok = true;
    if (setjmp(jump.env) == 0) {
        *out = fn(vm, args, arg_count);
    } else {
        ok = false;
    }

    vm->native_jump = jump.prev;
    return ok;
}

void vm_format_stack_trace(VM *vm, char *buf, size_t buf_size, const char *error_msg) {
    /* Find the innermost frame that has a source_path */
    const char *fallback_source = NULL;
    for (int j = vm->frame_count - 1; j >= 0; j--) {
        if (vm->frames[j].chunk && vm->frames[j].chunk->source_path) {
            fallback_source = vm->frames[j].chunk->source_path;
            break;
        }
    }
    int pos = 0;
    /* Top frame = the one that threw */
    int top_line = 0;
    const char *top_source = NULL;
    if (vm->frame_count > 0) {
        CallFrame *f = &vm->frames[vm->frame_count - 1];
        if (f->chunk) {
            int ip = f->ip;
            if (ip > 0 && ip <= f->chunk->count) top_line = f->chunk->lines[ip - 1];
            top_source = f->chunk->source_path ? f->chunk->source_path : fallback_source;
        }
    }
    /* Get exception class name */
    const char *class_name = "Error";
    if (IS_OBJ(vm->last_exception) && AS_OBJ(vm->last_exception)) {
        Object *obj = AS_OBJ(vm->last_exception);
        if (obj->type == OBJ_INSTANCE) {
            ObjInstance *inst = (ObjInstance*)obj;
            class_name = inst->class_name ? inst->class_name : "Error";
        }
    }
    /* Get the raw message from the exception instance (not value_to_string which wraps in <>) */
    const char *raw_msg = error_msg;
    if (IS_OBJ(vm->last_exception) && AS_OBJ(vm->last_exception)) {
        Object *obj = AS_OBJ(vm->last_exception);
        if (obj->type == OBJ_INSTANCE) {
            Value msgv = instance_get_field((ObjInstance*)obj, "message");
            if (IS_STRING(msgv)) raw_msg = ((ObjString*)AS_OBJ(msgv))->chars;
        }
    }
    /* file:line: ClassName: message */
    int n = snprintf(buf + pos, buf_size - pos, "%s:%d: %s: %s",
                     top_source ? top_source : "?", top_line, class_name,
                     raw_msg ? raw_msg : "?");
    if (n > 0) pos += n;
    /* Source context (3 lines, GCC-style with line numbers) */
    if (top_source && top_line > 0) {
        FILE *f = fopen(top_source, "r");
        if (f) {
            char line_buf[512];
            int current = 1;
            while (fgets(line_buf, sizeof(line_buf), f)) {
                size_t len = strlen(line_buf);
                if (len > 0 && line_buf[len-1] == '\n') line_buf[len-1] = '\0';
                if (current >= top_line - 1 && current <= top_line + 1) {
                    n = snprintf(buf + pos, buf_size - pos, "\n%5d | %s%s",
                                 current, line_buf,
                                 current == top_line ? "  <-- HERE" : "");
                    if (n > 0) pos += n;
                }
                current++;
            }
            fclose(f);
        }
    }
    /* Backtrace (all frames, top to bottom) */
    for (int i = 0; i < vm->frame_count && pos < (int)buf_size - 1; i++) {
        CallFrame *f = &vm->frames[i];
        if (!f->chunk) continue;
        int line = 0;
        int ip = f->ip;
        if (ip > 0 && ip <= f->chunk->count) line = f->chunk->lines[ip - 1];
        const char *fn_name = f->chunk->name ? f->chunk->name : "<unknown>";
        const char *source = f->chunk->source_path ? f->chunk->source_path : (fallback_source ? fallback_source : NULL);
        n = snprintf(buf + pos, buf_size - pos, "\n  %s:%d: in %s", source ? source : "?", line, fn_name);
        if (n > 0) pos += n;
    }
    buf[pos] = '\0';
}
