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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vm.h"
#include "value.h"
#include "chunk.h"
#include "opcode.h"
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

    vm->module_cache = new_dict();
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
    frame->saved_globals = NULL;
    int needed = frame->base + chunk->max_registers;
    if (needed > vm->stack_cap) {
        vm->stack_cap = needed < 64 ? 64 : needed * 2;
        vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
    }
    for (int i = 0; i < chunk->max_registers; i++) vm->stack[frame->base + i] = make_null();
    vm->stack_count = needed;

    uint32_t instr;
    uint8_t  A, B, C;
    uint16_t Bx;
    int      sBx;
    Value    _exc = make_null();

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

    /* ---- Constants & Loading ---- */
    /* 0.  OP_LOADK */
    op_loadk:
        SET_REG(RA, CONST(BX));
        DECODE; goto *op_labels[OP(instr)];

    /* 1.  OP_LOADNULL */
    op_loadnull:
        SET_REG_PRIM(RA, make_null());
        DECODE; goto *op_labels[OP(instr)];

    /* 2.  OP_LOADTRUE */
    op_loadtrue:
        SET_REG_PRIM(RA, make_bool(true));
        DECODE; goto *op_labels[OP(instr)];

    /* 3.  OP_LOADFALSE */
    op_loadfalse:
        SET_REG_PRIM(RA, make_bool(false));
        DECODE; goto *op_labels[OP(instr)];

    /* 4.  OP_LOADI */
    op_loadi:
        SET_REG_PRIM(RA, make_int(SBX));
        DECODE; goto *op_labels[OP(instr)];

    /* ---- Register Ops ---- */
    /* 5.  OP_MOVE */
    op_move:
        SET_REG(RA, REG(RB));
        DECODE; goto *op_labels[OP(instr)];

    /* 6.  OP_SWAP */
    op_swap: {
        Value t = REG(RA); REG(RA) = REG(RB); REG(RB) = t;
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Arithmetic (ABC: A = B op C) ---- */
    /* 7.  OP_ADD */
    op_add: {
        Value _L = RKB;
        Value _R = RKC;
        if (IS_INT(_L) && IS_INT(_R)) {
            SET_REG_PRIM(RA, make_int(AS_INT(_L) + AS_INT(_R)));
        } else {
            SET_REG(RA, do_arith(_L, _R, OP_ADD));
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 8.  OP_SUB */
    op_sub: { Value _L=RKB,_R=RKC; if(IS_INT(_L)&&IS_INT(_R)){SET_REG_PRIM(RA,make_int(AS_INT(_L)-AS_INT(_R)));}else{SET_REG_PRIM(RA,do_arith(_L,_R,OP_SUB));} DECODE; goto *op_labels[OP(instr)]; }
    /* 9.  OP_MUL */
    op_mul: { Value _L=RKB,_R=RKC; if(IS_INT(_L)&&IS_INT(_R)){SET_REG_PRIM(RA,make_int(AS_INT(_L)*AS_INT(_R)));}else{SET_REG_PRIM(RA,do_arith(_L,_R,OP_MUL));} DECODE; goto *op_labels[OP(instr)]; }

    /* 10. OP_DIV */
    op_div: {
        Value _L = RKB, _R = RKC;
        if (IS_INT(_L) && IS_INT(_R)) {
            int64_t ri = AS_INT(_R);
            if (!ri) { release_value(_exc); _exc = make_exception_instance(vm, vm->exception_class, "vm: div/0"); goto op_throw; }
            SET_REG_PRIM(RA, make_int(AS_INT(_L) / ri));
        } else if (is_num(_L) && is_num(_R)) {
            double r = to_f64(_R);
            if (r == 0.0) {
                double l = to_f64(_L);
                if (l == 0.0) SET_REG_PRIM(RA, make_double(0.0/0.0));
                else if (l > 0.0) SET_REG_PRIM(RA, make_pos_inf());
                else SET_REG_PRIM(RA, make_neg_inf());
            } else {
                SET_REG_PRIM(RA, make_double(to_f64(_L) / r));
            }
        } else {
            SET_REG_PRIM(RA, make_null());
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 11. OP_MOD */
    op_mod: {
        Value _L = RKB, _R = RKC;
        if (IS_INT(_L) && IS_INT(_R)) {
            int64_t ri = AS_INT(_R);
            if (!ri) { release_value(_exc); _exc = make_exception_instance(vm, vm->exception_class, "vm: mod/0"); goto op_throw; }
            SET_REG_PRIM(RA, make_int(AS_INT(_L) % ri));
        } else if (is_num(_L) && is_num(_R)) {
            double r = to_f64(_R);
            if (r == 0.0) {
                SET_REG_PRIM(RA, make_double(0.0/0.0));
            } else {
                SET_REG_PRIM(RA, make_double(fmod(to_f64(_L), r)));
            }
        } else {
            SET_REG_PRIM(RA, make_null());
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 12. OP_NEG */
    op_neg: {
        Value v = REG(RB);
        if (IS_INT(v))    { SET_REG_PRIM(RA, make_int(-AS_INT(v)));     DECODE; goto *op_labels[OP(instr)]; }
        if (IS_DOUBLE(v)) { SET_REG_PRIM(RA, make_double(-AS_DOUBLE(v))); DECODE; goto *op_labels[OP(instr)]; }
        SET_REG_PRIM(RA, make_null());
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 13. OP_BAND */
    op_band: SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) & to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    /* 14. OP_BOR */
    op_bor:  SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) | to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    /* 15. OP_BXOR */
    op_bxor: SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) ^ to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    /* 16. OP_BNOT */
    op_bnot: SET_REG_PRIM(RA, make_int(~to_i64(REG(RB)))); DECODE; goto *op_labels[OP(instr)];
    /* 17. OP_SHL */
    op_shl:  SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) << to_i64(REG(RC)))); DECODE; goto *op_labels[OP(instr)];
    /* 18. OP_SHR */
    op_shr:  SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) >> to_i64(REG(RC)))); DECODE; goto *op_labels[OP(instr)];

    /* ---- Integer-immediate arithmetic (ABC: A = A op (int8_t)B) ---- */
    /* 19. OP_ADDI */
    op_addi: {
        Value v = REG(RA);
        if (IS_INT(v))         REG(RA) = make_int(AS_INT(v) + (int8_t)B);
        else if (IS_DOUBLE(v)) REG(RA) = make_double(AS_DOUBLE(v) + (double)(int8_t)B);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 20. OP_SUBI */
    op_subi: {
        Value v = REG(RA);
        if (IS_INT(v))         REG(RA) = make_int(AS_INT(v) - (int8_t)B);
        else if (IS_DOUBLE(v)) REG(RA) = make_double(AS_DOUBLE(v) - (double)(int8_t)B);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Comparison (ABC: A = B cmp C, result is bool) ---- */
    /* 21. OP_EQ */
    op_eq: SET_REG_PRIM(RA, do_cmp(RKB, RKC, OP_EQ)); DECODE; goto *op_labels[OP(instr)];
    /* 22. OP_NE */
    op_ne: SET_REG_PRIM(RA, do_cmp(RKB, RKC, OP_NE)); DECODE; goto *op_labels[OP(instr)];
    /* 23. OP_LT */
    op_lt: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,IS_INT(_L)&&IS_INT(_R)?make_bool(AS_INT(_L)< AS_INT(_R)):do_cmp(_L,_R,OP_LT)); DECODE; goto *op_labels[OP(instr)]; }
    /* 24. OP_LE */
    op_le: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,IS_INT(_L)&&IS_INT(_R)?make_bool(AS_INT(_L)<=AS_INT(_R)):do_cmp(_L,_R,OP_LE)); DECODE; goto *op_labels[OP(instr)]; }
    /* 25. OP_GT */
    op_gt: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,IS_INT(_L)&&IS_INT(_R)?make_bool(AS_INT(_L)> AS_INT(_R)):do_cmp(_L,_R,OP_GT)); DECODE; goto *op_labels[OP(instr)]; }
    /* 26. OP_GE */
    op_ge: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,IS_INT(_L)&&IS_INT(_R)?make_bool(AS_INT(_L)>=AS_INT(_R)):do_cmp(_L,_R,OP_GE)); DECODE; goto *op_labels[OP(instr)]; }
    /* 27. OP_IN */
    op_in: {
        Value needle = RKB;
        Value haystack = RKC;
        bool found = false;
        if (IS_LIST(haystack)) {
            found = list_contains((ObjList*)AS_OBJ(haystack), needle);
        } else if (IS_DICT(haystack)) {
            found = dict_has((ObjDict*)AS_OBJ(haystack), needle);
        } else if (IS_STRING(haystack) && IS_STRING(needle)) {
            found = strstr(((ObjString*)AS_OBJ(haystack))->chars, ((ObjString*)AS_OBJ(needle))->chars) != NULL;
        }
        SET_REG_PRIM(RA, make_bool(found));
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Logical ---- */
    /* 28. OP_NOT */
    op_not: SET_REG_PRIM(RA, make_bool(!is_truthy(REG(RB)))); DECODE; goto *op_labels[OP(instr)];

    /* ---- Control Flow ---- */
    /* 29. OP_JMP */
    op_jmp:
        IP += sBx;
        DECODE; goto *op_labels[OP(instr)];

    /* 30. OP_JZ */
    op_jz:
        if (!is_truthy(REG(RA))) IP += sBx;
        DECODE; goto *op_labels[OP(instr)];

    /* 31. OP_JNZ */
    op_jnz:
        if (is_truthy(REG(RA))) IP += sBx;
        DECODE; goto *op_labels[OP(instr)];

    /* 32. OP_JNIL */
    op_jnil:
        if (IS_NIL(REG(RA))) IP += sBx;
        DECODE; goto *op_labels[OP(instr)];

    /* ---- Functions ---- */
    /* 33. OP_CALL */
    op_call: {
        uint8_t ret_reg = RA;
        uint8_t fn_reg  = B;
        uint8_t nargs   = C;
        Value fn_val = REG(fn_reg);
        /* removed duplicate non-object check */
        if (IS_FUNCTION(fn_val)) {
            ObjFunction *fn = (ObjFunction *)AS_OBJ(fn_val);
            if (fn->is_native) {
                Value scratch[256];
                for (int i = 0; i < nargs; i++) scratch[i] = REG(fn_reg + 1 + i);
                Value result = fn->native_fn(vm, scratch, nargs);
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            } else {
                if (vm->frame_count >= MAX_FRAMES) {
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->exception_class, "vm: call stack overflow");
                    goto op_throw;
                }
                CallFrame *caller = &FRAME;
                CallFrame *callee = &vm->frames[vm->frame_count];
                callee->chunk = fn->chunk;
                callee->ip = 0;
                callee->base = caller->base + fn_reg + 1;
                callee->closure = NULL;
                callee->ret_reg = ret_reg;
                callee->nargs = nargs;
                callee->kw_args = make_null();
                callee->fn = fn;
                callee->saved_globals = NULL;
                int needed = callee->base + fn->chunk->max_registers;
                if (needed > vm->stack_cap) {
                    vm->stack_cap = needed < 64 ? 64 : needed * 2;
                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                }
                for (int i = nargs; i < fn->chunk->max_registers; i++)
                    vm->stack[callee->base + i] = make_null();
                vm->stack_count = needed > vm->stack_count ? needed : vm->stack_count;
                vm->frame_count++;
                CHUNK = callee->chunk;
                IP = 0;
                DECODE;
                goto *op_labels[OP(instr)];
            }
        } else if (IS_CLOSURE(fn_val)) {
            ObjClosure *cl = (ObjClosure *)AS_OBJ(fn_val);
            ObjFunction *fn = cl->function;
            if (fn->is_native) {
                Value scratch[256];
                for (int i = 0; i < nargs; i++) scratch[i] = REG(fn_reg + 1 + i);
                Value result = fn->native_fn(vm, scratch, nargs);
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            } else {
                if (vm->frame_count >= MAX_FRAMES) {
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->exception_class, "vm: call stack overflow");
                    goto op_throw;
                }
                CallFrame *caller = &FRAME;
                CallFrame *callee = &vm->frames[vm->frame_count];
                callee->chunk = fn->chunk;
                callee->ip = 0;
                callee->base = caller->base + fn_reg + 1;
                callee->closure = cl;
                callee->ret_reg = ret_reg;
                callee->nargs = nargs;
                callee->kw_args = make_null();
                callee->fn = fn;
                callee->saved_globals = NULL;
                int needed = callee->base + fn->chunk->max_registers;
                if (needed > vm->stack_cap) {
                    vm->stack_cap = needed < 64 ? 64 : needed * 2;
                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                }
                for (int i = nargs; i < fn->chunk->max_registers; i++)
                    vm->stack[callee->base + i] = make_null();
                vm->stack_count = needed > vm->stack_count ? needed : vm->stack_count;
                vm->frame_count++;
                CHUNK = callee->chunk;
                IP = 0;
                DECODE;
                goto *op_labels[OP(instr)];
            }
        } else if (IS_BOUND_METHOD(fn_val)) {
            ObjBoundMethod *bm = (ObjBoundMethod *)AS_OBJ(fn_val);
            ObjFunction *fn = bm->fn;
            if (fn->is_native) {
                Value scratch[256];
                scratch[0] = bm->self;
                for (int i = 0; i < nargs; i++) scratch[i + 1] = REG(fn_reg + 1 + i);
                Value result = fn->native_fn(vm, scratch, nargs + 1);
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            } else {
                if (vm->frame_count >= MAX_FRAMES) {
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->exception_class, "vm: call stack overflow");
                    goto op_throw;
                }
                CallFrame *caller = &FRAME;
                CallFrame *callee = &vm->frames[vm->frame_count];
                callee->chunk = fn->chunk;
                callee->ip = 0;
                /* Overwrite bound method slot with self; base points there so reg 0 = self */
                SET_REG(fn_reg, bm->self);
                callee->base = caller->base + fn_reg;
                callee->closure = NULL;
                callee->ret_reg = ret_reg;
                callee->nargs = nargs;
                callee->kw_args = make_null();
                callee->fn = fn;
                callee->saved_globals = NULL;
                int needed = callee->base + fn->chunk->max_registers;
                if (needed > vm->stack_cap) {
                    vm->stack_cap = needed < 64 ? 64 : needed * 2;
                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                }
                /* nargs+1 because self occupies reg 0 */
                for (int i = nargs + 1; i < fn->chunk->max_registers; i++)
                    vm->stack[callee->base + i] = make_null();
                vm->stack_count = needed > vm->stack_count ? needed : vm->stack_count;
                vm->frame_count++;
                CHUNK = callee->chunk;
                IP = 0;
                DECODE;
                goto *op_labels[OP(instr)];
            }
        } else {
            char *s = value_to_string(fn_val);
            char buf[256];
            if (IS_OBJ(fn_val)) {
                snprintf(buf, sizeof(buf), "vm: attempt to call non-function (type=%d, value=%s)", (int)AS_OBJ(fn_val)->type, s);
            } else {
                snprintf(buf, sizeof(buf), "vm: attempt to call non-function (got %s)", s);
            }
            free(s);
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 34. OP_RET */
    op_ret: {
        uint8_t ret_reg = RA;
        Value retval = REG(ret_reg);
        if (IS_OBJ(retval) && AS_OBJ(retval)) retain_obj(AS_OBJ(retval));

        if (vm->frame_count <= 1) {
            close_upvalues(vm, 0);
            for (int i = 0; i < FRAME.chunk->max_registers; i++) SET_REG(i, make_null());
            vm->stack_count = 0;
            vm->frame_count = 0;
            if (IS_OBJ(retval) && AS_OBJ(retval)) release_obj(AS_OBJ(retval));
            return VM_OK;
        }
        CallFrame *callee = &FRAME;
        int old_base = callee->base;
        release_value(callee->kw_args);
        close_upvalues(vm, vm->frame_count);

        /* Module import cleanup: capture exports before destroying globals */
        if (callee->saved_globals) {
            char *captured_name = callee->fn && callee->fn->name ? strdup(callee->fn->name) : strdup("<module>");
            release_value(retval);
            ObjDict *exports = vm_globals_to_dict(vm);
            vm_globals_restore(vm, callee->saved_globals);
            callee->saved_globals = NULL;
            ObjModule *mod = new_module(captured_name);
            release_obj((Object*)mod->exports);
            mod->exports = exports;
            retain_obj((Object*)exports);
            Value module_val = make_obj((Object*)mod);
            dict_set(vm->module_cache, make_obj((Object*)new_string(captured_name, (int)strlen(captured_name))), module_val);
            retval = module_val;
            if (IS_OBJ(retval)) retain_obj(AS_OBJ(retval));
            free(captured_name);
        }

        for (int i = 0; i < callee->chunk->max_registers; i++) SET_REG(i, make_null());
        
        vm_pop_try_frames(vm, vm->frame_count - 1);
        vm->frame_count--;
        CallFrame *caller = &FRAME;
        CHUNK = caller->chunk;
        IP = caller->ip;
        SET_REG(callee->ret_reg, retval);
        if (IS_OBJ(retval) && AS_OBJ(retval)) release_obj(AS_OBJ(retval));
        vm->stack_count = old_base;
        DECODE;
        goto *op_labels[OP(instr)];
    }

    /* 34. OP_ENTER */
    op_enter:
        /* Stack is pre-allocated by vm_run_chunk based on max_registers */
        DECODE; goto *op_labels[OP(instr)];

    /* 35. OP_LEAVE */
    op_leave:
        close_upvalues(vm, vm->frame_count);
        DECODE; goto *op_labels[OP(instr)];

    /* 36. OP_CLOSURE */
    op_closure: {
        uint8_t k = B;
        uint8_t dst = RA;
        Value fn_val = CONST(k);
        if (!IS_FUNCTION(fn_val)) {
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, "vm: CLOSURE needs function constant");
            goto op_throw;
        }
        ObjFunction *fn = (ObjFunction*)AS_OBJ(fn_val);
        ObjClosure *cl = new_closure(fn);
        for (int i = 0; i < fn->upvalue_count; i++) {
            uint8_t is_local = fn->upvalue_descriptors[i].is_local;
            uint8_t index    = fn->upvalue_descriptors[i].index;
            if (is_local) {
                cl->upvalues[i] = capture_upvalue(vm, FRAME.base + index);
            } else {
                if (FRAME.closure && index < (uint8_t)FRAME.closure->upvalue_count)
                    cl->upvalues[i] = FRAME.closure->upvalues[index];
                else
                    cl->upvalues[i] = NULL;
            }
            if (cl->upvalues[i]) retain_obj((Object*)cl->upvalues[i]);
        }
        SET_REG(dst, make_obj((Object*)cl));
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Globals ---- */
    /* 37. OP_GETGLOBAL */
    op_getglobal: {
        Value v;
        if (vm_get_global_fast(vm, KSTROBJ(BX), &v)) SET_REG(RA, v);
        else SET_REG_PRIM(RA, make_null());
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 38. OP_SETGLOBAL */
    op_setglobal: {
        vm_set_global(vm, KSTR(BX), REG(RA), false);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Upvalues ---- */
    /* 39. OP_GETUPVAL */
    op_getupval: {
        uint8_t upv_idx = B;
        uint8_t dst = RA;
        if (FRAME.closure && upv_idx < (uint8_t)FRAME.closure->upvalue_count) {
            ObjUpvalue *uv = FRAME.closure->upvalues[upv_idx];
            SET_REG(dst, uv->is_open ? vm->stack[uv->stack_index] : uv->closed);
        } else {
            SET_REG_PRIM(dst, make_null());
        }
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 40. OP_SETUPVAL */
    op_setupval: {
        uint8_t upv_idx = B;
        uint8_t src = RA;
        if (FRAME.closure && upv_idx < (uint8_t)FRAME.closure->upvalue_count) {
            ObjUpvalue *uv = FRAME.closure->upvalues[upv_idx];
            Value val = REG(src);
            if (uv->is_open) {
                SET_STACK(uv->stack_index, val);
            } else {
                if (IS_OBJ(val) && AS_OBJ(val)) retain_obj(AS_OBJ(val));
                Value old = uv->closed;
                uv->closed = val;
                if (IS_OBJ(old) && AS_OBJ(old)) release_obj(AS_OBJ(old));
            }
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Object Ops ---- */
    /* 41. OP_NEW */
    op_new: {
        ObjString *cls_str = KSTROBJ(BX);
        if (!cls_str) { SET_REG_PRIM(RA, make_null()); DECODE; goto *op_labels[OP(instr)]; }
        const char *cls_name = cls_str->chars;
        Value cls_val;
        if (!vm_get_global(vm, cls_name, &cls_val) || !IS_CLASS(cls_val)) {
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, "vm: class not found for new");
            goto op_throw;
        }
        ObjClass *cls = (ObjClass*)AS_OBJ(cls_val);
        ObjInstance *new_inst = new_instance(cls, 4);
        SET_REG(RA, make_obj((Object*)new_inst));
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 42. OP_NEWDICT */
    op_newdict: SET_REG(RA, make_obj((Object*)new_dict())); DECODE; goto *op_labels[OP(instr)];
    /* 43. OP_NEWLIST */
    op_newlist: SET_REG(RA, make_obj((Object*)new_list((int)Bx))); DECODE; goto *op_labels[OP(instr)];
    /* 44. OP_LISTAPPEND */
    op_listappend: {
        Value lst = REG(RA);
        Value val = REG(RB);
        if (IS_LIST(lst)) {
            list_add((ObjList*)AS_OBJ(lst), val);
        } else {
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, "vm: listappend on non-list");
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 45. OP_GETITER */
    op_getiter: {
        Value iter = REG(RB);
        if (IS_DICT(iter)) {
            ObjDict *dict = (ObjDict*)AS_OBJ(iter);
            ObjList *keys = new_list(dict->entry_count);
            if (dict->indices == NULL) {
                for (int i = 0; i < dict->entry_count; i++)
                    list_add(keys, dict->inline_entries[i].key);
            } else {
                for (int i = 0; i < dict->next_entry; i++) {
                    if (dict->entries[i].key != EMPTY_VAL)
                        list_add(keys, dict->entries[i].key);
                }
            }
            SET_REG(RA, make_obj((Object*)keys));
        } else {
            SET_REG(RA, iter);
        }
        SET_REG_PRIM(RA + 1, make_int(0));
        SET_REG_PRIM(RA + 2, make_null());
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 46. OP_FORLOOP */
    op_forloop: {
        Value iter = REG(RA);
        Value state = REG(RA + 1);
        bool has_next = false;

        if (IS_OBJ(iter) && AS_OBJ(iter)) {
            switch (AS_OBJ(iter)->type) {
                case OBJ_LIST: {
                    ObjList *lst = (ObjList*)AS_OBJ(iter);
                    int idx = AS_INT(state);
                    if (idx < lst->count) {
                        Value item = lst->items ? lst->items[idx] : lst->inline_items[idx];
                        SET_REG(RA + 2, item);
                        SET_REG_PRIM(RA + 1, make_int(idx + 1));
                        has_next = true;
                    }
                    break;
                }
                case OBJ_STRING: {
                    ObjString *str = (ObjString*)AS_OBJ(iter);
                    int idx = AS_INT(state);
                    if (idx < str->length) {
                        char buf[2] = {str->chars[idx], '\0'};
                        SET_REG(RA + 2, make_obj((Object*)new_string(buf, 1)));
                        SET_REG_PRIM(RA + 1, make_int(idx + 1));
                        has_next = true;
                    }
                    break;
                }
                default:
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->exception_class, "vm: object is not iterable");
                    goto op_throw;
            }
        } else {
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, "vm: non-object is not iterable");
            goto op_throw;
        }

        if (has_next) {
            IP += SBX;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 48. OP_INDEXGET (strict — throws on missing or null obj) */
    op_indexget: {
        Value obj = REG(RB);
        Value key = REG(RC);
        if (!IS_OBJ(obj) || !AS_OBJ(obj)) {
            release_value(_exc);
                _exc = make_exception_instance(vm, vm->type_error_class, "TypeError: cannot index null value");
            goto op_throw;
        }
        switch (AS_OBJ(obj)->type) {
            case OBJ_LIST: {
                if (!IS_INT(key)) {
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->type_error_class, "TypeError: list index must be integer");
                    goto op_throw;
                }
                int idx = (int)AS_INT(key);
                ObjList *lst = (ObjList*)AS_OBJ(obj);
                int len = lst->count;
                if (idx < 0) idx += len;
                if (idx < 0 || idx >= len) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "IndexError: list index out of bounds (index: %d, size: %d)", idx, len);
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->index_error_class, buf);
                    goto op_throw;
                }
                SET_REG(RA, lst->items ? lst->items[idx] : lst->inline_items[idx]);
                break;
            }
            case OBJ_DICT: {
                Value val = dict_get((ObjDict*)AS_OBJ(obj), key);
                if (IS_NIL(val)) {
                    char *ks = value_to_string(key);
                    char buf[256];
                    snprintf(buf, sizeof(buf), "KeyError: %s not found in dict", ks);
                    free(ks);
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->key_error_class, buf);
                    goto op_throw;
                }
                SET_REG(RA, val);
                break;
            }
            case OBJ_STRING: {
                ObjString *s = (ObjString*)AS_OBJ(obj);
                if (!IS_INT(key)) {
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->type_error_class, "TypeError: string index must be integer");
                    goto op_throw;
                }
                int idx = (int)AS_INT(key);
                if (idx < 0) idx += s->length;
                if (idx < 0 || idx >= s->length) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "IndexError: string index out of bounds (index: %d, length: %d)", idx, s->length);
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->index_error_class, buf);
                    goto op_throw;
                }
                SET_REG(RA, make_obj((Object*)new_string(&s->chars[idx], 1)));
                break;
            }
            default: {
                release_value(_exc);
                _exc = make_exception_instance(vm, vm->type_error_class, "TypeError: value is not indexable");
                goto op_throw;
            }
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 64. OP_INDEXGET_SAFE (safe — null on missing or null obj) */
    op_indexget_safe: {
        Value obj = REG(RB);
        Value key = REG(RC);
        if (!IS_OBJ(obj) || !AS_OBJ(obj)) { SET_REG_PRIM(RA, make_null()); DECODE; goto *op_labels[OP(instr)]; }
        switch (AS_OBJ(obj)->type) {
            case OBJ_LIST:
                SET_REG(RA, IS_INT(key) ? list_get((ObjList*)AS_OBJ(obj), AS_INT(key)) : make_null());
                break;
            case OBJ_DICT:
                SET_REG(RA, dict_get((ObjDict*)AS_OBJ(obj), key));
                break;
            case OBJ_STRING: {
                ObjString *s = (ObjString*)AS_OBJ(obj);
                if (IS_INT(key) && AS_INT(key) >= 0 && AS_INT(key) < s->length)
                    SET_REG(RA, make_obj((Object*)new_string(&s->chars[AS_INT(key)], 1)));
                else
                    SET_REG_PRIM(RA, make_null());
                break;
            }
            default: SET_REG_PRIM(RA, make_null()); break;
        }
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 48. OP_INDEXSET */
    op_indexset: {
        Value obj = REG(RA);
        Value key = REG(RB);
        Value val = REG(RC);
        if (IS_OBJ(obj) && AS_OBJ(obj)) {
            if (IS_LIST(obj) && IS_INT(key)) list_set((ObjList*)AS_OBJ(obj), AS_INT(key), val);
            else if (IS_DICT(obj)) dict_set((ObjDict*)AS_OBJ(obj), key, val);
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 49. OP_SLICE */
    op_slice: {
        Value obj = REG(RB);
        Value start_val = REG(RB + 1);
        Value stop_val  = REG(RB + 2);
        Value step_val  = REG(RB + 3);
        if (IS_LIST(obj)) {
            ObjList *lst = (ObjList*)AS_OBJ(obj);
            int len = list_length(lst);
            int step = IS_INT(step_val) ? AS_INT(step_val) : 1;
            if (step == 0) {
                SET_REG(RA, make_obj((Object*)new_list(0)));
            } else {
                int start, stop;
                if (IS_NIL(start_val)) {
                    start = (step < 0) ? len - 1 : 0;
                } else if (IS_INT(start_val)) {
                    start = AS_INT(start_val);
                    if (start < 0) start += len;
                    if (start < 0) start = (step < 0) ? -1 : 0;
                    if (start > len) start = len;
                } else {
                    start = (step < 0) ? len - 1 : 0;
                }
                if (IS_NIL(stop_val)) {
                    stop = (step < 0) ? -1 : len;
                } else if (IS_INT(stop_val)) {
                    stop = AS_INT(stop_val);
                    if (stop < 0) stop += len;
                    if (stop < 0) stop = -1;
                    if (stop > len) stop = len;
                } else {
                    stop = (step < 0) ? -1 : len;
                }
                ObjList *result = new_list(0);
                if (step > 0) {
                    for (int i = start; i < stop; i += step) {
                        Value v = lst->items ? lst->items[i] : lst->inline_items[i];
                        list_add(result, v);
                        if (IS_OBJ(v) && AS_OBJ(v)) retain_obj(AS_OBJ(v));
                    }
                } else {
                    for (int i = start; i > stop; i += step) {
                        Value v = lst->items ? lst->items[i] : lst->inline_items[i];
                        list_add(result, v);
                        if (IS_OBJ(v) && AS_OBJ(v)) retain_obj(AS_OBJ(v));
                    }
                }
                SET_REG(RA, make_obj((Object*)result));
            }
        } else if (IS_STRING(obj)) {
            ObjString *s = (ObjString*)AS_OBJ(obj);
            int len = s->length;
            int step = IS_INT(step_val) ? AS_INT(step_val) : 1;
            if (step == 0) {
                SET_REG(RA, make_obj((Object*)new_string("", 0)));
            } else {
                int start, stop;
                if (IS_NIL(start_val)) {
                    start = (step < 0) ? len - 1 : 0;
                } else if (IS_INT(start_val)) {
                    start = AS_INT(start_val);
                    if (start < 0) start += len;
                    if (start < 0) start = (step < 0) ? -1 : 0;
                    if (start > len) start = len;
                } else {
                    start = (step < 0) ? len - 1 : 0;
                }
                if (IS_NIL(stop_val)) {
                    stop = (step < 0) ? -1 : len;
                } else if (IS_INT(stop_val)) {
                    stop = AS_INT(stop_val);
                    if (stop < 0) stop += len;
                    if (stop < 0) stop = -1;
                    if (stop > len) stop = len;
                } else {
                    stop = (step < 0) ? -1 : len;
                }
                int count = 0;
                if (step > 0) {
                    for (int i = start; i < stop; i += step) count++;
                } else {
                    for (int i = start; i > stop; i += step) count++;
                }
                char *buf = (char*)malloc(count + 1);
                int j = 0;
                if (step > 0) {
                    for (int i = start; i < stop; i += step) buf[j++] = s->chars[i];
                } else {
                    for (int i = start; i > stop; i += step) buf[j++] = s->chars[i];
                }
                buf[j] = '\0';
                ObjString *result = new_string(buf, count);
                free(buf);
                SET_REG(RA, make_obj((Object*)result));
            }
        } else {
            SET_REG_PRIM(RA, make_null());
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 65. OP_SLICE_SAFE (safe — null on null obj) */
    op_slice_safe: {
        Value obj = REG(RB);
        Value start_val = REG(RB + 1);
        Value stop_val  = REG(RB + 2);
        Value step_val  = REG(RB + 3);
        if (!IS_OBJ(obj) || !AS_OBJ(obj)) { SET_REG_PRIM(RA, make_null()); DECODE; goto *op_labels[OP(instr)]; }
        if (IS_LIST(obj)) {
            ObjList *lst = (ObjList*)AS_OBJ(obj);
            int len = list_length(lst);
            int step = IS_INT(step_val) ? AS_INT(step_val) : 1;
            if (step == 0) {
                SET_REG(RA, make_obj((Object*)new_list(0)));
            } else {
                int start, stop;
                if (IS_NIL(start_val)) {
                    start = (step < 0) ? len - 1 : 0;
                } else if (IS_INT(start_val)) {
                    start = AS_INT(start_val);
                    if (start < 0) start += len;
                    if (start < 0) start = (step < 0) ? -1 : 0;
                    if (start > len) start = len;
                } else {
                    start = (step < 0) ? len - 1 : 0;
                }
                if (IS_NIL(stop_val)) {
                    stop = (step < 0) ? -1 : len;
                } else if (IS_INT(stop_val)) {
                    stop = AS_INT(stop_val);
                    if (stop < 0) stop += len;
                    if (stop < 0) stop = -1;
                    if (stop > len) stop = len;
                } else {
                    stop = (step < 0) ? -1 : len;
                }
                ObjList *result = new_list(0);
                if (step > 0) {
                    for (int i = start; i < stop; i += step) {
                        Value v = lst->items ? lst->items[i] : lst->inline_items[i];
                        list_add(result, v);
                        if (IS_OBJ(v) && AS_OBJ(v)) retain_obj(AS_OBJ(v));
                    }
                } else {
                    for (int i = start; i > stop; i += step) {
                        Value v = lst->items ? lst->items[i] : lst->inline_items[i];
                        list_add(result, v);
                        if (IS_OBJ(v) && AS_OBJ(v)) retain_obj(AS_OBJ(v));
                    }
                }
                SET_REG(RA, make_obj((Object*)result));
            }
        } else if (IS_STRING(obj)) {
            ObjString *s = (ObjString*)AS_OBJ(obj);
            int len = s->length;
            int step = IS_INT(step_val) ? AS_INT(step_val) : 1;
            if (step == 0) {
                SET_REG(RA, make_obj((Object*)new_string("", 0)));
            } else {
                int start, stop;
                if (IS_NIL(start_val)) {
                    start = (step < 0) ? len - 1 : 0;
                } else if (IS_INT(start_val)) {
                    start = AS_INT(start_val);
                    if (start < 0) start += len;
                    if (start < 0) start = (step < 0) ? -1 : 0;
                    if (start > len) start = len;
                } else {
                    start = (step < 0) ? len - 1 : 0;
                }
                if (IS_NIL(stop_val)) {
                    stop = (step < 0) ? -1 : len;
                } else if (IS_INT(stop_val)) {
                    stop = AS_INT(stop_val);
                    if (stop < 0) stop += len;
                    if (stop < 0) stop = -1;
                    if (stop > len) stop = len;
                } else {
                    stop = (step < 0) ? -1 : len;
                }
                int count = 0;
                if (step > 0) {
                    for (int i = start; i < stop; i += step) count++;
                } else {
                    for (int i = start; i > stop; i += step) count++;
                }
                char *buf = (char*)malloc(count + 1);
                int j = 0;
                if (step > 0) {
                    for (int i = start; i < stop; i += step) buf[j++] = s->chars[i];
                } else {
                    for (int i = start; i > stop; i += step) buf[j++] = s->chars[i];
                }
                buf[j] = '\0';
                ObjString *result = new_string(buf, count);
                free(buf);
                SET_REG(RA, make_obj((Object*)result));
            }
        } else {
            SET_REG_PRIM(RA, make_null());
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 51. OP_MEMBERGET (strict — throws on missing) */
    op_memberget: {
        Value obj = REG(RB);
        ObjString *field = KSTROBJ(RC);
        if (!IS_OBJ(obj) || !AS_OBJ(obj)) {
            release_value(_exc);
                _exc = make_exception_instance(vm, vm->attribute_error_class, "AttributeError: cannot access member of null value");
            goto op_throw;
        }
        switch (AS_OBJ(obj)->type) {
            case OBJ_INSTANCE: {
                ObjInstance *inst = (ObjInstance*)AS_OBJ(obj);
                uint32_t h = ((uint32_t)(uintptr_t)inst ^ field->hash);
                int idx = h & (IC_CACHE_SIZE - 1);
                IC_MemberEntry *ic = &vm->member_ic[idx];
                if (ic->inst == inst && ic->name == field) {
                    SET_REG(RA, inst->fields[ic->index]);
                } else {
                    int fi = -1;
                    for (int i = 0; i < inst->field_count; i++) {
                        if (strcmp(inst->field_names[i], field->chars) == 0) { fi = i; break; }
                    }
                    if (fi >= 0) {
                        SET_REG(RA, inst->fields[fi]);
                        ic->inst = inst; ic->name = field; ic->index = fi;
                    } else {
                        int mi = -1;
                        if (inst->klass) {
                            for (int i = 0; i < inst->klass->method_count; i++) {
                                if (strcmp(inst->klass->method_names[i], field->chars) == 0) { mi = i; break; }
                            }
                        }
                        if (mi >= 0) {
                            ObjBoundMethod *bm = new_bound_method(obj, inst->klass->methods[mi]);
                            SET_REG(RA, make_obj((Object*)bm));
                        } else {
                            release_value(_exc);
                            char buf[256];
                            snprintf(buf, sizeof(buf), "AttributeError: instance has no field or method '%s'", field->chars);
                            _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                            goto op_throw;
                        }
                    }
                }
                break;
            }
            case OBJ_LIST: {
                if (field->length == 6 && !memcmp(field->chars, "length", 6))
                    SET_REG_PRIM(RA, make_int(list_length((ObjList*)AS_OBJ(obj))));
                else {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "AttributeError: list has no field '%s'", field->chars);
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                    goto op_throw;
                }
                break;
            }
            case OBJ_MODULE: {
                ObjModule *mod = (ObjModule*)AS_OBJ(obj);
                Value export_val = dict_get(mod->exports, make_obj((Object*)field));
                if (!IS_NIL(export_val)) {
                    SET_REG(RA, export_val);
                } else {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "AttributeError: module '%s' has no attribute '%s'", mod->name->chars, field->chars);
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                    goto op_throw;
                }
                break;
            }
            case OBJ_DICT: {
                ObjFunction *mfn = vm_dict_method_lookup(field->chars, field->length);
                if (mfn) {
                    ObjBoundMethod *bm = new_bound_method(obj, mfn);
                    SET_REG(RA, make_obj((Object*)bm));
                } else {
                    Value val = dict_get((ObjDict*)AS_OBJ(obj), make_obj((Object*)field));
                    if (IS_NIL(val)) {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "KeyError: '%s' not found in dict", field->chars);
                        release_value(_exc);
                        _exc = make_exception_instance(vm, vm->key_error_class, buf);
                        goto op_throw;
                    }
                    SET_REG(RA, val);
                }
                break;
            }
            case OBJ_STRING: {
                if (field->length == 6 && !memcmp(field->chars, "length", 6))
                    SET_REG_PRIM(RA, make_int(((ObjString*)AS_OBJ(obj))->length));
                else {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "AttributeError: string has no field '%s'", field->chars);
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                    goto op_throw;
                }
                break;
            }
            case OBJ_ENUM: {
                ObjEnum *e = (ObjEnum*)AS_OBJ(obj);
                int fi = -1;
                for (int i = 0; i < e->count; i++) {
                    if (strcmp(e->names[i], field->chars) == 0) { fi = i; break; }
                }
                if (fi >= 0) SET_REG_PRIM(RA, make_int(e->values[fi]));
                else {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "AttributeError: enum has no variant '%s'", field->chars);
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                    goto op_throw;
                }
                break;
            }
            case OBJ_CLASS: {
                ObjClass *cls = (ObjClass*)AS_OBJ(obj);
                if (strcmp(field->chars, "name") == 0)
                    SET_REG(RA, make_obj((Object*)new_string(cls->name, (int)strlen(cls->name))));
                else if (strcmp(field->chars, "base") == 0)
                    SET_REG(RA, cls->base ? make_obj((Object*)cls->base) : make_null());
                else {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "AttributeError: class has no field '%s'", field->chars);
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->exception_class, buf);
                    goto op_throw;
                }
                break;
            }
            default: {
                release_value(_exc);
                _exc = make_exception_instance(vm, vm->attribute_error_class, "AttributeError: value has no fields");
                goto op_throw;
            }
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 63. OP_MEMBERGET_SAFE (safe — null on missing or null obj) */
    op_memberget_safe: {
        Value obj = REG(RB);
        ObjString *field = KSTROBJ(RC);
        if (!IS_OBJ(obj) || !AS_OBJ(obj)) { SET_REG_PRIM(RA, make_null()); DECODE; goto *op_labels[OP(instr)]; }
        switch (AS_OBJ(obj)->type) {
            case OBJ_INSTANCE: {
                ObjInstance *inst = (ObjInstance*)AS_OBJ(obj);
                int fi = -1;
                for (int i = 0; i < inst->field_count; i++) {
                    if (strcmp(inst->field_names[i], field->chars) == 0) { fi = i; break; }
                }
                if (fi >= 0) { SET_REG(RA, inst->fields[fi]); break; }
                int mi = -1;
                if (inst->klass) {
                    for (int i = 0; i < inst->klass->method_count; i++) {
                        if (strcmp(inst->klass->method_names[i], field->chars) == 0) { mi = i; break; }
                    }
                }
                if (mi >= 0) {
                    ObjBoundMethod *bm = new_bound_method(obj, inst->klass->methods[mi]);
                    SET_REG(RA, make_obj((Object*)bm));
                } else {
                    SET_REG_PRIM(RA, make_null());
                }
                break;
            }
            case OBJ_LIST:     SET_REG_PRIM(RA, (field->length == 6 && !memcmp(field->chars, "length", 6)) ? make_int(list_length((ObjList*)AS_OBJ(obj))) : make_null()); break;
            case OBJ_MODULE: {
                ObjModule *mod = (ObjModule*)AS_OBJ(obj);
                SET_REG_PRIM(RA, dict_get(mod->exports, make_obj((Object*)field)));
                break;
            }
            case OBJ_DICT: {
                ObjFunction *mfn = vm_dict_method_lookup(field->chars, field->length);
                if (mfn) {
                    ObjBoundMethod *bm = new_bound_method(obj, mfn);
                    SET_REG(RA, make_obj((Object*)bm));
                } else {
                    SET_REG(RA, dict_get((ObjDict*)AS_OBJ(obj), make_obj((Object*)field)));
                }
                break;
            }
            case OBJ_STRING:   SET_REG_PRIM(RA, (field->length == 6 && !memcmp(field->chars, "length", 6)) ? make_int(((ObjString*)AS_OBJ(obj))->length) : make_null()); break;
            case OBJ_ENUM: {
                ObjEnum *e = (ObjEnum*)AS_OBJ(obj);
                int fi = -1;
                for (int i = 0; i < e->count; i++) {
                    if (strcmp(e->names[i], field->chars) == 0) { fi = i; break; }
                }
                if (fi >= 0) SET_REG_PRIM(RA, make_int(e->values[fi]));
                else SET_REG_PRIM(RA, make_null());
                break;
            }
            case OBJ_CLASS: {
                ObjClass *cls = (ObjClass*)AS_OBJ(obj);
                if (strcmp(field->chars, "name") == 0)
                    SET_REG(RA, make_obj((Object*)new_string(cls->name, (int)strlen(cls->name))));
                else if (strcmp(field->chars, "base") == 0)
                    SET_REG(RA, cls->base ? make_obj((Object*)cls->base) : make_null());
                else SET_REG_PRIM(RA, make_null());
                break;
            }
            default: SET_REG_PRIM(RA, make_null()); break;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 51. OP_MEMBERSET */
    op_memberset: {
        Value obj = REG(RA);
        ObjString *field = KSTROBJ(RC);
        if (IS_INSTANCE(obj)) {
            ObjInstance *inst = (ObjInstance*)AS_OBJ(obj);
            uint32_t h = ((uint32_t)(uintptr_t)inst ^ field->hash);
            int idx = h & (IC_CACHE_SIZE - 1);
            IC_MemberEntry *ic = &vm->member_ic[idx];
            if (ic->inst == inst && ic->name == field) {
                SET_FIELD(inst, ic->index, REG(RB));
            } else {
                int fi = -1;
                for (int i = 0; i < inst->field_count; i++) {
                    if (strcmp(inst->field_names[i], field->chars) == 0) { fi = i; break; }
                }
                if (fi >= 0) {
                    SET_FIELD(inst, fi, REG(RB));
                    ic->inst = inst; ic->name = field; ic->index = fi;
                } else {
                    instance_set_field(inst, field->chars, REG(RB));
                }
            }
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Invocation ---- */
    /* 52. OP_INVOKE */
    op_invoke: {
        uint8_t ret_reg = RA;
        uint8_t obj_reg = B;
        uint8_t nargs   = C;
        Value method_val = REG(obj_reg + 1);
        Value obj = REG(obj_reg);
        if (!IS_STRING(method_val)) { SET_REG_PRIM(ret_reg, make_null()); DECODE; goto *op_labels[OP(instr)]; }
        const char *mname = ((ObjString*)AS_OBJ(method_val))->chars;

        Value scratch[256];
        for (int i = 0; i < nargs; i++) scratch[i] = REG(obj_reg + 2 + i);

        Value result = make_null();
        bool handled = false;
        if (IS_OBJ(obj) && AS_OBJ(obj)) {
            switch (AS_OBJ(obj)->type) {
                case OBJ_LIST: handled = vm_invoke_list(vm, (ObjList*)AS_OBJ(obj), mname, scratch, nargs, &result); break;
                case OBJ_DICT: handled = vm_invoke_dict(vm, (ObjDict*)AS_OBJ(obj), mname, scratch, nargs, &result); break;
                case OBJ_ENUM: handled = vm_invoke_enum(vm, (ObjEnum*)AS_OBJ(obj), mname, scratch, nargs, &result); break;
                case OBJ_MODULE: {
                    ObjModule *mod = (ObjModule*)AS_OBJ(obj);
                    Value fn_val = dict_get(mod->exports, make_obj((Object*)((ObjString*)AS_OBJ(method_val))));
                    if (IS_FUNCTION(fn_val)) {
                        ObjFunction *fn = (ObjFunction*)AS_OBJ(fn_val);
                        if (fn->is_native) {
                            result = fn->native_fn(vm, scratch, nargs);
                        } else if (vm->frame_count < MAX_FRAMES) {
                            CallFrame *caller = &FRAME;
                            CallFrame *callee = &vm->frames[vm->frame_count];
                            callee->chunk = fn->chunk;
                            callee->ip = 0;
                            callee->base = caller->base + obj_reg + 2;
                            callee->closure = NULL;
                            callee->ret_reg = ret_reg;
                            callee->nargs = nargs;
                            callee->kw_args = make_null();
                            callee->fn = fn;
                            callee->saved_globals = NULL;
                            int need = callee->base + fn->chunk->max_registers;
                            if (need > vm->stack_cap) {
                                vm->stack_cap = need < 64 ? 64 : need * 2;
                                vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                            }
                            for (int j = nargs; j < fn->chunk->max_registers; j++)
                                vm->stack[callee->base + j] = make_null();
                            vm->stack_count = need > vm->stack_count ? need : vm->stack_count;
                            vm->frame_count++;
                            CHUNK = callee->chunk;
                            IP = 0;
                            DECODE;
                            goto *op_labels[OP(instr)];
                        }
                        handled = true;
                    } else if (IS_CLOSURE(fn_val)) {
                        ObjClosure *cl = (ObjClosure*)AS_OBJ(fn_val);
                        ObjFunction *fn = cl->function;
                        if (fn->is_native) {
                            result = fn->native_fn(vm, scratch, nargs);
                        } else if (vm->frame_count < MAX_FRAMES) {
                            CallFrame *caller = &FRAME;
                            CallFrame *callee = &vm->frames[vm->frame_count];
                            callee->chunk = fn->chunk;
                            callee->ip = 0;
                            callee->base = caller->base + obj_reg + 2;
                            callee->closure = cl;
                            callee->ret_reg = ret_reg;
                            callee->nargs = nargs;
                            callee->kw_args = make_null();
                            callee->fn = fn;
                            callee->saved_globals = NULL;
                            int need = callee->base + fn->chunk->max_registers;
                            if (need > vm->stack_cap) {
                                vm->stack_cap = need < 64 ? 64 : need * 2;
                                vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                            }
                            for (int j = nargs; j < fn->chunk->max_registers; j++)
                                vm->stack[callee->base + j] = make_null();
                            vm->stack_count = need > vm->stack_count ? need : vm->stack_count;
                            vm->frame_count++;
                            CHUNK = callee->chunk;
                            IP = 0;
                            DECODE;
                            goto *op_labels[OP(instr)];
                        }
                        handled = true;
                    }
                    break;
                }
                case OBJ_INSTANCE: {
                    ObjInstance *obj_inst = (ObjInstance*)AS_OBJ(obj);
                    ObjString *mname_obj = (ObjString*)AS_OBJ(method_val);
                    uint32_t h = ((uint32_t)(uintptr_t)obj_inst ^ mname_obj->hash);
                    int idx = h & (IC_CACHE_SIZE - 1);
                    IC_MemberEntry *ic = &vm->method_ic[idx];
                    int mi = -1;
                    ObjClass *k = obj_inst->klass;
                    if (k) {
                        if (ic->inst == obj_inst && ic->name == mname_obj) {
                            mi = ic->index;
                        } else {
                            /* Search backwards so child methods shadow inherited ones */
                            for (int i = k->method_count - 1; i >= 0; i--) {
                                ObjFunction *m = k->methods[i];
                                if (m && strcmp(k->method_names[i], mname_obj->chars) == 0) { mi = i; break; }
                            }
                            if (mi >= 0) { ic->inst = obj_inst; ic->name = mname_obj; ic->index = mi; }
                        }
                        if (mi >= 0) {
                            ObjFunction *m = k->methods[mi];
                            SET_REG(obj_reg + 1, obj); // Bind 'self' to first arg slot
                            if (m->is_native) {
                                result = m->native_fn(vm, scratch, nargs);
                            } else {
                                if (vm->frame_count < MAX_FRAMES) {
                                    CallFrame *caller = &FRAME;
                                    CallFrame *callee = &vm->frames[vm->frame_count];
                                    callee->chunk = m->chunk;
                                    callee->ip = 0;
                                    callee->base = caller->base + obj_reg + 1;
                                    callee->ret_reg = ret_reg;
                                    callee->nargs = nargs;
                                    callee->kw_args = make_null();
                                    callee->fn = m;
                                    callee->saved_globals = NULL;
                                    int need = callee->base + m->chunk->max_registers;
                                    if (need > vm->stack_cap) {
                                        vm->stack_cap = need < 64 ? 64 : need * 2;
                                        vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                                    }
                                    vm->stack[callee->base] = obj; /* self at reg 0 */
                                    /* args already aligned: callee reg 1 = caller reg obj_reg+2 */
                                    for (int j = 1 + nargs; j < m->chunk->max_registers; j++)
                                        vm->stack[callee->base + j] = make_null();
                                    vm->stack_count = need > vm->stack_count ? need : vm->stack_count;
                                    vm->frame_count++;
                                    CHUNK = callee->chunk;
                                    IP = 0;
                                    DECODE;
                                    goto *op_labels[OP(instr)];
                                }
                            }
                            handled = true;
                        }
                    }
                    break;
                }
                default: break;
            }
        }
        if (handled) SET_REG(ret_reg, result);
        else {
            char buf[256];
            snprintf(buf, sizeof(buf), "vm: unknown method '%s'", mname);
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 53. OP_SUPER */
    op_super: {
        uint8_t ret_reg = RA;
        uint8_t self_reg = RB;
        uint8_t nargs   = RC;
        Value method_val = REG(ret_reg + 1);
        Value self = REG(self_reg);
        if (!IS_STRING(method_val) || !IS_INSTANCE(self)) {
            SET_REG_PRIM(ret_reg, make_null());
            DECODE; goto *op_labels[OP(instr)];
        }
        const char *mname = ((ObjString*)AS_OBJ(method_val))->chars;
        ObjInstance *inst = (ObjInstance*)AS_OBJ(self);

        Value result = make_null();
        bool handled = false;

        /* Look up parent class via klass->base */
        if (inst->klass && inst->klass->base) {
            ObjClass *base = inst->klass->base;
            for (int i = 0; i < base->method_count; i++) {
                ObjFunction *m = base->methods[i];
                if (m && strcmp(base->method_names[i], mname) == 0) {
                    SET_REG(self_reg + 1, self);
                    if (m->is_native) {
                        Value scratch[256];
                        for (int j = 0; j < nargs; j++)
                            scratch[j] = REG(ret_reg + 2 + j);
                        result = m->native_fn(vm, scratch, nargs);
                    } else {
                        if (vm->frame_count < MAX_FRAMES) {
                            CallFrame *caller = &FRAME;
                            CallFrame *callee = &vm->frames[vm->frame_count];
                            callee->chunk = m->chunk;
                            callee->ip = 0;
                            callee->base = caller->base + self_reg + 1;
                            callee->ret_reg = ret_reg;
                            callee->nargs = nargs;
                            callee->kw_args = make_null();
                            callee->fn = m;
                            callee->saved_globals = NULL;
                            int need = callee->base + m->chunk->max_registers;
                            if (need > vm->stack_cap) {
                                vm->stack_cap = need < 64 ? 64 : need * 2;
                                vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                            }
                            vm->stack[callee->base] = self;
                            for (int j = 1 + nargs; j < m->chunk->max_registers; j++)
                                vm->stack[callee->base + j] = make_null();
                            vm->stack_count = need > vm->stack_count ? need : vm->stack_count;
                            vm->frame_count++;
                            CHUNK = callee->chunk;
                            IP = 0;
                            DECODE;
                            goto *op_labels[OP(instr)];
                        }
                    }
                    handled = true;
                    break;
                }
            }
        }

        if (handled) SET_REG(ret_reg, result);
        else {
            char buf[256];
            snprintf(buf, sizeof(buf), "vm: unknown super method '%s'", mname);
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Exceptions ---- */
    /* 54. OP_THROW */
    op_throw: {
        if (!IS_OBJ(_exc) || !AS_OBJ(_exc)) {
            _exc = REG(RA);
        }
        vm->last_exception = _exc;
        /* unwind to the nearest catch */
        while (vm->try_stack) {
            TryFrame *tf = vm->try_stack;
            if (tf->frame_depth <= vm->frame_count) {
                /* unwind call frames */
                while (vm->frame_count > tf->frame_depth) {
                    close_upvalues(vm, vm->frame_count);
                    vm->frame_count--;
                }
                vm->stack_count = tf->stack_count;
                SET_REG(tf->exc_reg, _exc);
                int catch_ip = tf->catch_ip;
                vm->try_stack = tf->next;
                free(tf);
                _exc = make_null();
                CHUNK = FRAME.chunk;
                IP = catch_ip;
                DECODE;
                goto *op_labels[OP(instr)];
            }
            /* stale frame from deeper call, pop it */
            vm->try_stack = tf->next;
            free(tf);
        }
        return VM_EXCEPTION;
    }

    /* 55. OP_TRY */
    op_try: {
        TryFrame *tf = malloc(sizeof(TryFrame));
        tf->catch_ip   = IP + sBx;
        tf->frame_depth = vm->frame_count;
        tf->stack_count = vm->stack_count;
        tf->exc_reg    = RA;
        tf->next       = vm->try_stack;
        vm->try_stack  = tf;
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 56. OP_ENDTRY */
    op_endtry: {
        if (vm->try_stack) {
            TryFrame *tf = vm->try_stack;
            vm->try_stack = tf->next;
            free(tf);
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 58. OP_ISINSTANCE */
    op_isinstance: {
        Value obj = REG(RB);
        Value cls = REG(RC);
        bool result = false;
        if (IS_OBJ(obj) && AS_OBJ(obj) && AS_OBJ(obj)->type == OBJ_INSTANCE) {
            ObjClass *obj_class = ((ObjInstance*)AS_OBJ(obj))->klass;
            if (IS_OBJ(cls) && AS_OBJ(cls) && AS_OBJ(cls)->type == OBJ_CLASS) {
                ObjClass *target = (ObjClass*)AS_OBJ(cls);
                while (obj_class) {
                    if (obj_class == target) { result = true; break; }
                    obj_class = obj_class->base;
                }
            }
        }
        SET_REG_PRIM(RA, make_bool(result));
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 59. OP_DEFAULT */
    op_default: {
        uint8_t param_idx = RA;
        int32_t skip = sBx;
        bool has_arg = (FRAME.nargs > (int)param_idx);
        if (!has_arg && FRAME.fn && FRAME.fn->param_name_objs &&
            IS_OBJ(FRAME.kw_args) && AS_OBJ(FRAME.kw_args)->type == OBJ_DICT) {
            ObjDict *kw = (ObjDict*)AS_OBJ(FRAME.kw_args);
            Value key_val = make_obj((Object*)FRAME.fn->param_name_objs[param_idx]);
            Value v = dict_get(kw, key_val);
            has_arg = !IS_NIL(v);
        }
        if (has_arg) {
            IP += skip;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 60. OP_KWARGS */
    op_kwargs: {
        if (FRAME.fn && FRAME.fn->param_name_objs &&
            IS_OBJ(FRAME.kw_args) && AS_OBJ(FRAME.kw_args)->type == OBJ_DICT) {
            ObjDict *kw = (ObjDict*)AS_OBJ(FRAME.kw_args);
            for (int i = 0; i < FRAME.fn->param_count; i++) {
                ObjString *key_str = FRAME.fn->param_name_objs[i];
                Value key_val = make_obj((Object*)key_str);
                Value v = dict_get(kw, key_val);
                if (!IS_NIL(v)) {
                    SET_REG(i, v);
                }
            }
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 61. OP_KCALL */
    op_kcall: {
        uint8_t ret_reg = RA;
        uint8_t fn_reg  = B;
        uint8_t nargs   = C;
        Value fn_val = REG(fn_reg);
        Value kw_args_val = REG(fn_reg + nargs + 1);
        if (IS_FUNCTION(fn_val)) {
            ObjFunction *fn = (ObjFunction *)AS_OBJ(fn_val);
            if (fn->is_native) {
                Value scratch[256];
                for (int i = 0; i < nargs; i++) scratch[i] = REG(fn_reg + 1 + i);
                Value result = fn->native_fn(vm, scratch, nargs);
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            } else {
                if (vm->frame_count >= MAX_FRAMES) {
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->exception_class, "vm: call stack overflow");
                    goto op_throw;
                }
                CallFrame *caller = &FRAME;
                CallFrame *callee = &vm->frames[vm->frame_count];
                callee->chunk = fn->chunk;
                callee->ip = 0;
                callee->base = caller->base + fn_reg + 1;
                callee->closure = NULL;
                callee->ret_reg = ret_reg;
                callee->nargs = nargs;
                callee->kw_args = kw_args_val;
                retain_value(kw_args_val);
                callee->fn = fn;
                callee->saved_globals = NULL;
                int needed = callee->base + fn->chunk->max_registers;
                if (needed > vm->stack_cap) {
                    vm->stack_cap = needed < 64 ? 64 : needed * 2;
                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                }
                for (int i = nargs; i < fn->chunk->max_registers; i++)
                    vm->stack[callee->base + i] = make_null();
                vm->stack_count = needed > vm->stack_count ? needed : vm->stack_count;
                vm->frame_count++;
                CHUNK = callee->chunk;
                IP = 0;
                DECODE;
                goto *op_labels[OP(instr)];
            }
        } else if (IS_CLOSURE(fn_val)) {
            ObjClosure *cl = (ObjClosure *)AS_OBJ(fn_val);
            ObjFunction *fn = cl->function;
            if (fn->is_native) {
                Value scratch[256];
                for (int i = 0; i < nargs; i++) scratch[i] = REG(fn_reg + 1 + i);
                Value result = fn->native_fn(vm, scratch, nargs);
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            } else {
                if (vm->frame_count >= MAX_FRAMES) {
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->exception_class, "vm: call stack overflow");
                    goto op_throw;
                }
                CallFrame *caller = &FRAME;
                CallFrame *callee = &vm->frames[vm->frame_count];
                callee->chunk = fn->chunk;
                callee->ip = 0;
                callee->base = caller->base + fn_reg + 1;
                callee->closure = cl;
                callee->ret_reg = ret_reg;
                callee->nargs = nargs;
                callee->kw_args = kw_args_val;
                retain_value(kw_args_val);
                callee->fn = fn;
                callee->saved_globals = NULL;
                int needed = callee->base + fn->chunk->max_registers;
                if (needed > vm->stack_cap) {
                    vm->stack_cap = needed < 64 ? 64 : needed * 2;
                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                }
                for (int i = nargs; i < fn->chunk->max_registers; i++)
                    vm->stack[callee->base + i] = make_null();
                vm->stack_count = needed > vm->stack_count ? needed : vm->stack_count;
                vm->frame_count++;
                CHUNK = callee->chunk;
                IP = 0;
                DECODE;
                goto *op_labels[OP(instr)];
            }
        } else if (IS_BOUND_METHOD(fn_val)) {
            ObjBoundMethod *bm = (ObjBoundMethod *)AS_OBJ(fn_val);
            ObjFunction *fn = bm->fn;
            if (fn->is_native) {
                Value scratch[256];
                scratch[0] = bm->self;
                for (int i = 0; i < nargs; i++) scratch[i + 1] = REG(fn_reg + 1 + i);
                Value result = fn->native_fn(vm, scratch, nargs + 1);
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            } else {
                if (vm->frame_count >= MAX_FRAMES) {
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->exception_class, "vm: call stack overflow");
                    goto op_throw;
                }
                CallFrame *caller = &FRAME;
                CallFrame *callee = &vm->frames[vm->frame_count];
                callee->chunk = fn->chunk;
                callee->ip = 0;
                SET_REG(fn_reg, bm->self);
                callee->base = caller->base + fn_reg;
                callee->closure = NULL;
                callee->ret_reg = ret_reg;
                callee->nargs = nargs;
                callee->kw_args = kw_args_val;
                retain_value(kw_args_val);
                callee->fn = fn;
                callee->saved_globals = NULL;
                int needed = callee->base + fn->chunk->max_registers;
                if (needed > vm->stack_cap) {
                    vm->stack_cap = needed < 64 ? 64 : needed * 2;
                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                }
                for (int i = nargs + 1; i < fn->chunk->max_registers; i++)
                    vm->stack[callee->base + i] = make_null();
                vm->stack_count = needed > vm->stack_count ? needed : vm->stack_count;
                vm->frame_count++;
                CHUNK = callee->chunk;
                IP = 0;
                DECODE;
                goto *op_labels[OP(instr)];
            }
        } else {
            char *s = value_to_string(fn_val);
            char buf[256];
            if (IS_OBJ(fn_val)) {
                snprintf(buf, sizeof(buf), "vm: attempt to call non-function (type=%d, value=%s)", (int)AS_OBJ(fn_val)->type, s);
            } else {
                snprintf(buf, sizeof(buf), "vm: attempt to call non-function (got %s)", s);
            }
            free(s);
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 62. OP_COALESCE */
    op_coalesce: {
        if (!IS_NIL(REG(RA))) {
            IP += sBx;
            DECODE;
            goto *op_labels[OP(instr)];
        }
        DECODE;
        goto *op_labels[OP(instr)];
    }

    /* 66. OP_IMPORT */
    op_import: {
        uint8_t dst = RA;
        Value mod_name_val = CONST(BX);
        if (!IS_STRING(mod_name_val)) {
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, "import: module name must be a string");
            goto op_throw;
        }
        const char *mod_name = ((ObjString*)AS_OBJ(mod_name_val))->chars;

        Value cached = dict_get(vm->module_cache, mod_name_val);
        if (!IS_NIL(cached) && IS_MODULE(cached)) {
            SET_REG(dst, cached);
            DECODE; goto *op_labels[OP(instr)];
        }

        char *filepath = module_resolve_path(mod_name);
        if (!filepath) {
            char buf[256];
            snprintf(buf, sizeof(buf), "import: module '%s' not found", mod_name);
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }

        char *source = module_read_source(filepath);
        free(filepath);
        if (!source) {
            char buf[256];
            snprintf(buf, sizeof(buf), "import: cannot read module '%s'", mod_name);
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }

        Lexer *lexer = lexer_new(source);
        TokenList *tokens = lexer_tokenize(lexer);
        free(source);

        bool had_lex_errors = false;
        for (Token *t = tokens->head; t; t = t->next) {
            if (t->type == TOK_ERROR) {
                had_lex_errors = true;
                fprintf(stderr, "Lexer error in '%s': %s\n", mod_name, t->value);
            }
        }

        if (had_lex_errors) {
            token_list_free(tokens);
            lexer_free(lexer);
            char buf[256];
            snprintf(buf, sizeof(buf), "import: lexer errors in '%s'", mod_name);
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }

        Parser *parser = parser_new(tokens);
        Program *program = parser_parse(parser);

        if (parser->had_error) {
            parser_free(parser);
            token_list_free(tokens);
            lexer_free(lexer);
            char buf[256];
            snprintf(buf, sizeof(buf), "import: parser errors in '%s'", mod_name);
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }

        /* Save current globals, create fresh table for module compilation+execution */
        GlobalEntry **saved_globals = vm_globals_save(vm);
        vm_globals_fresh(vm);

        Chunk mod_chunk;
        if (!compile_program(program, &mod_chunk, vm, false, true)) {
            vm_globals_restore(vm, saved_globals);
            parser_free(parser);
            token_list_free(tokens);
            lexer_free(lexer);
            char buf[256];
            snprintf(buf, sizeof(buf), "import: compilation errors in '%s'", mod_name);
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }

        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);

        /* Create ObjFunction wrapper for the module chunk */
        ObjFunction *mod_fn = new_function(mod_name);
        mod_fn->chunk = malloc(sizeof(Chunk));
        *mod_fn->chunk = mod_chunk;
        retain_obj((Object*)mod_fn);

        /* Push function onto stack and set up call frame */
        SET_REG(dst, make_obj((Object*)mod_fn));

        if (vm->frame_count >= MAX_FRAMES) {
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, "vm: call stack overflow");
            goto op_throw;
        }

        CallFrame *caller = &FRAME;
        CallFrame *callee = &vm->frames[vm->frame_count];
        callee->chunk         = mod_fn->chunk;
        callee->ip            = 0;
        callee->base          = caller->base + dst + 1; /* registers start after the fn slot */
        callee->closure       = NULL;
        callee->ret_reg       = dst;
        callee->nargs         = 0;
        callee->kw_args       = make_null();
        callee->fn            = mod_fn;
        callee->saved_globals = saved_globals;

        int needed = callee->base + mod_fn->chunk->max_registers;
        if (needed > vm->stack_cap) {
            vm->stack_cap = needed < 64 ? 64 : needed * 2;
            vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
        }
        for (int i = 0; i < mod_fn->chunk->max_registers; i++)
            vm->stack[callee->base + i] = make_null();
        vm->stack_count = needed > vm->stack_count ? needed : vm->stack_count;
        vm->frame_count++;
        CHUNK = callee->chunk;
        IP = 0;
        DECODE;
        goto *op_labels[OP(instr)];
    }

    /* 67. OP_HALT */
    op_halt:
        return VM_OK;

    /* ============================================================ */
    
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
