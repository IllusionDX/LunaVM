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

/* GC hook for embedder API stack — defined in luna.c */
extern void luna_mark_roots(VM *vm);

/* Declared in vm_builtins.c */
void vm_register_builtins(VM *vm);
void vm_register_canonical_classes(VM *vm);

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

static const char *val_type_name(Value v) {
    if (IS_NIL(v)) return "null";
    if (IS_BOOL(v)) return "bool";
    if (IS_INT(v) || IS_INT64(v)) return "int";
    if (IS_DOUBLE(v)) return "float";
    if (IS_STRING(v)) return "str";
    if (IS_LIST(v)) return "list";
    if (IS_DICT(v)) return "dict";
    if (IS_OBJ(v)) {
        Object *obj = AS_OBJ(v);
        switch (obj->type->kind) {
            case OBJ_FUNCTION: return "function";
            case OBJ_UPVALUE: return "upvalue";
            case OBJ_CLOSURE: return "closure";
            case OBJ_ENUM: return "enum";
            case OBJ_CLASS: return "class";
            case OBJ_BOUND_METHOD: return "method";
            case OBJ_MODULE: return "module";
            case OBJ_BUFFER: return "buffer";
            case OBJ_USERDATA: return "userdata";
            case OBJ_VECTOR: return "vector";
            case OBJ_MATRIX: return "matrix";
            case OBJ_INSTANCE: {
                ObjInstance *inst = (ObjInstance*)obj;
                return inst->class_name;
            }
            default: return "object";
        }
    }
    return "unknown";
}

static inline Value do_arith(VM *vm, Value L, Value R, OpCode op) {
    /* OP_NEG: unary negative — delegate to vtable if object, else native */
    if (op == OP_NEG) {
        if (IS_OBJ(L) && AS_OBJ(L) && AS_OBJ(L)->type && AS_OBJ(L)->type->neg)
            return AS_OBJ(L)->type->neg(vm, L);
        if (is_int_type(L)) return make_int_result(-to_i64(L));
        return make_double(-to_f64(L));
    }
    /* MOP vtable dispatch: if L is a heap object with a vtable operation */
    if (IS_OBJ(L) && AS_OBJ(L) && AS_OBJ(L)->type) {
        Type *t = AS_OBJ(L)->type;
        MOP_Bin fn = NULL;
        switch (op) {
            case OP_ADD: fn = t->add; break;
            case OP_SUB: fn = t->sub; break;
            case OP_MUL: fn = t->mul; break;
            case OP_DIV: fn = t->div; break;
            case OP_MOD: fn = t->mod; break;
            default: break;
        }
        if (fn) return fn(vm, L, R);
    }
    /* Symmetric: for ADD/MUL, if R is the object (e.g. int + string), try R's vtable */
    if ((op == OP_ADD || op == OP_MUL) && IS_OBJ(R) && AS_OBJ(R) && AS_OBJ(R)->type) {
        Type *t = AS_OBJ(R)->type;
        MOP_Bin fn = NULL;
        switch (op) {
            case OP_ADD: fn = t->add; break;
            case OP_MUL: fn = t->mul; break;
            default: break;
        }
        if (fn) return fn(vm, R, L);  /* self = R, other = L */
    }
    /* Native arithmetic (int/float) — kept intact for correctness */
    if (!is_num(L) || !is_num(R)) return make_null();
    if (is_int_type(L) && is_int_type(R)) {
        int64_t li = to_i64(L); int64_t ri = to_i64(R);
        switch (op) {
            case OP_ADD: return make_int_result(li + ri);
            case OP_SUB: return make_int_result(li - ri);
            case OP_MUL: return make_int_result(li * ri);
            case OP_DIV: { if (!ri) return make_null(); return make_int_result(li / ri); }
            case OP_MOD: { if (!ri) return make_null(); return make_int_result(li % ri); }
            default: return make_null();
        }
    }
    double l = to_f64(L); double r = to_f64(R);
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
static inline Value do_cmp(VM *vm, Value L, Value R, OpCode op) {
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
    /* MOP vtable dispatch for comparison (only for ordering ops, not EQ/NE) */
    if (op != OP_EQ && op != OP_NE) {
        if (IS_OBJ(L) && AS_OBJ(L) && AS_OBJ(L)->type && AS_OBJ(L)->type->cmp) {
            int r = AS_OBJ(L)->type->cmp(vm, L, R);
            if (r == 2) return make_null(); /* unsupported */
            switch (op) {
                case OP_LT: return make_bool(r < 0);
                case OP_LE: return make_bool(r <= 0);
                case OP_GT: return make_bool(r > 0);
                case OP_GE: return make_bool(r >= 0);
                default: break;
            }
        }
        if (IS_OBJ(R) && AS_OBJ(R) && AS_OBJ(R)->type && AS_OBJ(R)->type->cmp) {
            int r = AS_OBJ(R)->type->cmp(vm, R, L);
            if (r == 2) return make_null();
            switch (op) {
                case OP_LT: return make_bool(r > 0);
                case OP_LE: return make_bool(r >= 0);
                case OP_GT: return make_bool(r < 0);
                case OP_GE: return make_bool(r <= 0);
                default: break;
            }
        }
        return make_null();
    }
    /* EQ / NE: universal equality (includes pointer identity for objects) */
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
extern int gc_state;
extern Object *sweep_cursor;

/* ---- Explicit gray stack (replaces C recursion in mark phase) ---- */

static Object **gray_stack = NULL;
static int gray_cap = 0;
static int gray_count = 0;

static void gray_push(Object *obj) {
    if (gray_count >= gray_cap) {
        gray_cap = gray_cap ? gray_cap * 2 : 512;
        gray_stack = realloc(gray_stack, sizeof(Object*) * gray_cap);
    }
    gray_stack[gray_count++] = obj;
}

static void mark_value(Value v) {
    if (IS_OBJ(v)) {
        Object *obj = AS_OBJ(v);
        if (obj && obj->gc_color == GC_COLOR_WHITE) {
            obj->gc_color = GC_COLOR_GRAY;
            gray_push(obj);
        }
    }
}

static void mark_object(Object *obj) {
    if (obj && obj->gc_color == GC_COLOR_WHITE) {
        obj->gc_color = GC_COLOR_GRAY;
        gray_push(obj);
    }
}

/* Drain the gray stack iteratively — no C recursion. */
static void mark_drain(void) {
    while (gray_count > 0) {
        Object *obj = gray_stack[--gray_count];
        obj->gc_color = GC_COLOR_BLACK;

        switch (obj->type->kind) {
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
                if (d->entries == NULL) {
                    for (int i = 0; i < d->entry_count; i++) {
                        mark_value(d->inline_entries[i].key);
                        mark_value(d->inline_entries[i].value);
                    }
                } else {
                    for (int i = 0; i < d->capacity; i++) {
                        if (d->entries[i].key != EMPTY_VAL && d->entries[i].key != TOMBSTONE_VAL) {
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
                mark_object((Object*)inst->klass);
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
            case OBJ_ENUM: break;
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
                mark_object((Object*)cls->base);
                mark_object((Object*)cls->prototype);
                for (int i = 0; i < cls->method_count; i++) {
                    mark_object((Object*)cls->methods[i]);
                }
                mark_object((Object*)cls->fields);
                break;
            }
            case OBJ_BOUND_METHOD: {
                ObjBoundMethod *bm = (ObjBoundMethod *)obj;
                mark_value(bm->self);
                mark_object((Object*)bm->fn);
                break;
            }
            case OBJ_MODULE: {
                ObjModule *mod = (ObjModule *)obj;
                mark_object((Object*)mod->name);
                mark_object((Object*)mod->exports);
                break;
            }
            case OBJ_BUFFER: break;
            case OBJ_INT64: break;
            default: break;
        }
    }
    /* gray_stack persists across cycles for incremental GC */
}

/* Write barrier — preserves tri-color invariant during incremental GC.
 * If parent is BLACK and child is WHITE, shade child GRAY so it gets
 * traced on the next gc_step() call. */
static inline void gc_write_barrier(Value parent_val, Value child_val) {
    if (gc_state != GC_STATE_MARK) return;
    
    /* Crucial safety check: Prevents segfaults if a primitive (Int/Double/Bool) 
     * is passed (e.g. from invalid user code like 1.x = 2). 
     * AS_OBJ would unmask garbage pointers otherwise. */
    if (!IS_OBJ(parent_val)) return;
    Object *p = AS_OBJ(parent_val);
    if (!p) return;
    if (!IS_OBJ(child_val)) return;
    Object *c = AS_OBJ(child_val);
    if (!c) return;
    /* ((BLACK - BLACK) | WHITE) == WHITE → need barrier */
    if ((p->gc_color - GC_COLOR_BLACK) | c->gc_color) return;
    c->gc_color = GC_COLOR_GRAY;
    gray_push(c);
}

/* Incremental GC step: processes a bounded number of objects.
 * Call from CHECK_GC. If cycle already in progress, continues it.
 * If idle and threshold exceeded, starts new cycle. */
static void gc_step(VM *vm) {
    if (gc_state == GC_STATE_IDLE) {
        if (bytes_allocated <= next_gc_threshold) return;
        // Start new cycle: mark all roots gray
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
        for (int i = 0; i < VM_GLOBAL_BUCKETS; i++) {
            GlobalEntry *e = vm->system_globals[i];
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
        if (vm->string_class) mark_object((Object*)vm->string_class);
        if (vm->list_class) mark_object((Object*)vm->list_class);
        if (vm->dict_class) mark_object((Object*)vm->dict_class);
        if (vm->enum_class) mark_object((Object*)vm->enum_class);
        if (vm->buffer_class) mark_object((Object*)vm->buffer_class);
        if (vm->vector_class) mark_object((Object*)vm->vector_class);
        if (vm->matrix_class) mark_object((Object*)vm->matrix_class);
        if (vm->function_class) mark_object((Object*)vm->function_class);
        if (vm->closure_class) mark_object((Object*)vm->closure_class);
        if (vm->bound_method_class) mark_object((Object*)vm->bound_method_class);
        if (vm->class_class) mark_object((Object*)vm->class_class);
        if (vm->module_class) mark_object((Object*)vm->module_class);
        if (vm->userdata_class) mark_object((Object*)vm->userdata_class);
        gc_state = GC_STATE_MARK;
    }

    if (gc_state == GC_STATE_MARK) {
        // Drain up to 256 objects from gray stack
        int limit = 256;
        while (gray_count > 0 && limit-- > 0) {
            Object *obj = gray_stack[--gray_count];
            obj->gc_color = GC_COLOR_BLACK;

            switch (obj->type->kind) {
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
                    if (d->entries == NULL) {
                        for (int i = 0; i < d->entry_count; i++) {
                            mark_value(d->inline_entries[i].key);
                            mark_value(d->inline_entries[i].value);
                        }
                    } else {
                        for (int i = 0; i < d->capacity; i++) {
                            if (d->entries[i].key != EMPTY_VAL && d->entries[i].key != TOMBSTONE_VAL) {
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
                    mark_object((Object*)inst->klass);
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
                case OBJ_ENUM: break;
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
                    mark_object((Object*)cls->base);
                    mark_object((Object*)cls->prototype);
                    for (int i = 0; i < cls->method_count; i++) {
                        mark_object((Object*)cls->methods[i]);
                    }
                    mark_object((Object*)cls->fields);
                    break;
                }
                case OBJ_BOUND_METHOD: {
                    ObjBoundMethod *bm = (ObjBoundMethod *)obj;
                    mark_value(bm->self);
                    mark_object((Object*)bm->fn);
                    break;
                }
                case OBJ_MODULE: {
                    ObjModule *mod = (ObjModule *)obj;
                    mark_object((Object*)mod->name);
                    mark_object((Object*)mod->exports);
                    break;
                }
                case OBJ_BUFFER: break;
                case OBJ_INT64: break;
                default: break;
            }
        }
        if (gray_count == 0) {
            // Invalidate inline caches
            memset(vm->global_ic, 0, sizeof(vm->global_ic));
            memset(vm->member_ic, 0, sizeof(vm->member_ic));
            memset(vm->method_ic, 0, sizeof(vm->method_ic));
            memset(vm->invoke_ic, 0, sizeof(vm->invoke_ic));
            memset(vm->call_ic, 0, sizeof(vm->call_ic));
            sweep_cursor = all_objects;
            gc_state = GC_STATE_SWEEP;
        }
    }

    if (gc_state == GC_STATE_SWEEP) {
        int limit = 256;
        while (sweep_cursor && limit-- > 0) {
            Object *obj = sweep_cursor;
            sweep_cursor = obj->next;
            if (obj->gc_color == GC_COLOR_WHITE) {
                // Remove from all_objects linked list
                if (obj->prev) obj->prev->next = obj->next;
                else all_objects = obj->next;
                if (obj->next) obj->next->prev = obj->prev;
                // Free the object
                obj->next = NULL;
                obj->prev = NULL;
                free_object_container(obj);
            } else {
                // Reset to white for next cycle
                obj->gc_color = GC_COLOR_WHITE;
            }
        }
        if (!sweep_cursor) {
            gc_state = GC_STATE_IDLE;
            if (bytes_allocated > next_gc_threshold) {
                next_gc_threshold = bytes_allocated * 2;
            } else {
                next_gc_threshold = 64 * 1024 * 1024;
            }
            // If write barrier created new gray objects during sweep, restart mark
            if (gray_count > 0) {
                gc_state = GC_STATE_MARK;
            }
        }
    }
}

void mark_and_sweep(VM *vm) {
    gc_state = GC_STATE_IDLE;

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
    mark_value(vm->last_return_value);
    if (vm->api_state) luna_mark_roots(vm);
    if (vm->module_cache) mark_object((Object*)vm->module_cache);
    if (vm->exception_class) mark_object((Object*)vm->exception_class);
    if (vm->type_error_class) mark_object((Object*)vm->type_error_class);
    if (vm->key_error_class) mark_object((Object*)vm->key_error_class);
    if (vm->index_error_class) mark_object((Object*)vm->index_error_class);
    if (vm->attribute_error_class) mark_object((Object*)vm->attribute_error_class);
    if (vm->value_error_class) mark_object((Object*)vm->value_error_class);
    if (vm->runtime_error_class) mark_object((Object*)vm->runtime_error_class);
    if (vm->string_class) mark_object((Object*)vm->string_class);
    if (vm->list_class) mark_object((Object*)vm->list_class);
    if (vm->dict_class) mark_object((Object*)vm->dict_class);
    if (vm->enum_class) mark_object((Object*)vm->enum_class);
    if (vm->buffer_class) mark_object((Object*)vm->buffer_class);
    if (vm->vector_class) mark_object((Object*)vm->vector_class);
    if (vm->matrix_class) mark_object((Object*)vm->matrix_class);
    if (vm->function_class) mark_object((Object*)vm->function_class);
    if (vm->closure_class) mark_object((Object*)vm->closure_class);
    if (vm->bound_method_class) mark_object((Object*)vm->bound_method_class);
    if (vm->class_class) mark_object((Object*)vm->class_class);
    if (vm->module_class) mark_object((Object*)vm->module_class);
    if (vm->userdata_class) mark_object((Object*)vm->userdata_class);

    // Drain gray stack iteratively (no C recursion)
    mark_drain();

    // Invalidate inline caches — any cached object may have been collected.
    memset(vm->global_ic, 0, sizeof(vm->global_ic));
    memset(vm->member_ic, 0, sizeof(vm->member_ic));
    memset(vm->method_ic, 0, sizeof(vm->method_ic));
    memset(vm->invoke_ic, 0, sizeof(vm->invoke_ic));

    /* 2. Collect unmarked garbage */
    Object *garbage = NULL;
    Object *obj = all_objects;
    while (obj) {
        if (obj->gc_color == GC_COLOR_WHITE) {
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
            obj->gc_color = GC_COLOR_WHITE; // Reset for next GC
            obj = obj->next;
        }
    }

    /* 3. Free garbage */
    Object *g = garbage;
    while (g) {
        Object *next = g->next;
        g->next = NULL;
        g->prev = NULL;
        free_object_container(g);
        g = next;
    }
    gc_state = GC_STATE_IDLE;

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
    vm_register_canonical_classes(vm);

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
    free(gray_stack);
    gray_stack = NULL;
    gray_cap = 0;
    gray_count = 0;

    for (int i = 0; i < VM_GLOBAL_BUCKETS; i++) {
        GlobalEntry *e = vm->globals[i];
        while (e) {
            GlobalEntry *nx = e->next;
            free(e->name); free(e); e = nx;
        }
        vm->globals[i] = NULL;
    }
    for (int i = 0; i < VM_GLOBAL_BUCKETS; i++) {
        GlobalEntry *e = vm->system_globals[i];
        while (e) {
            GlobalEntry *nx = e->next;
            free(e->name); free(e); e = nx;
        }
        vm->system_globals[i] = NULL;
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
}

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

static void frame_set_refs(CallFrame *frame, Object *closure, Object *fn) {
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

int mat4_invert(float *m, float *out) {
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

void mat4_transpose(float *m, float *out) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            out[r + c*4] = m[c + r*4];
}

void mat4_mul_translate(float *m, float tx, float ty, float tz) {
    for (int i = 0; i < 4; i++)
        m[12 + i] = m[0 + i]*tx + m[4 + i]*ty + m[8 + i]*tz + m[12 + i];
}

void mat4_mul_rotate_x(float *m, float angle) {
    float c = cosf(angle), s = sinf(angle);
    for (int i = 0; i < 4; i++) {
        float a = m[4 + i], b = m[8 + i];
        m[4 + i] = a * c + b * s;
        m[8 + i] = -a * s + b * c;
    }
}

void mat4_mul_rotate_y(float *m, float angle) {
    float c = cosf(angle), s = sinf(angle);
    for (int i = 0; i < 4; i++) {
        float a = m[0 + i], b = m[8 + i];
        m[0 + i] = a * c - b * s;
        m[8 + i] = a * s + b * c;
    }
}

void mat4_mul_rotate_z(float *m, float angle) {
    float c = cosf(angle), s = sinf(angle);
    for (int i = 0; i < 4; i++) {
        float a = m[0 + i], b = m[4 + i];
        m[0 + i] = a * c + b * s;
        m[4 + i] = -a * s + b * c;
    }
}

void mat4_mul_scale(float *m, float sx, float sy, float sz) {
    for (int i = 0; i < 4; i++)
        m[0 + i] *= sx;
    for (int i = 0; i < 4; i++)
        m[4 + i] *= sy;
    for (int i = 0; i < 4; i++)
        m[8 + i] *= sz;
}

VMResult vm_execute_loop(VM *vm, Chunk *chunk) {
    (void)chunk;
    uint32_t instr;
    Value _exc = make_null();

    /* Local hot state */
    CallFrame *current_frame = &vm->frames[vm->frame_count - 1];
    register uint32_t *pc    = current_frame->chunk->code + current_frame->ip;
    register Value    *REG_BASE = vm->stack + current_frame->base;
    register Chunk    *current_chunk = current_frame->chunk;

#define DECODE (instr = *pc++)

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
        gc_step(vm); \
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
        &&op_addk,          // 19 OP_ADDK
        &&op_mulk,          // 20 OP_MULK
        &&op_addi,          // 21 OP_ADDI
        &&op_subi,          // 22 OP_SUBI
        &&op_addi_from,     // 23 OP_ADDI_FROM
        &&op_subi_from,     // 24 OP_SUBI_FROM
        &&op_eq,            // 25 OP_EQ
        &&op_ne,            // 26 OP_NE
        &&op_lt,            // 27 OP_LT
        &&op_le,            // 28 OP_LE
        &&op_gt,            // 29 OP_GT
        &&op_ge,            // 30 OP_GE
        &&op_in,            // 31 OP_IN
        &&op_not,           // 32 OP_NOT
        &&op_jmp,           // 33 OP_JMP
        &&op_jz,            // 34 OP_JZ
        &&op_jnz,           // 35 OP_JNZ
        &&op_jnil,          // 36 OP_JNIL
        &&op_call,          // 37 OP_CALL
        &&op_ret,           // 38 OP_RET
        &&op_enter,         // 39 OP_ENTER
        &&op_leave,         // 40 OP_LEAVE
        &&op_closure,       // 41 OP_CLOSURE
        &&op_getglobal,     // 42 OP_GETGLOBAL
        &&op_setglobal,     // 43 OP_SETGLOBAL
        &&op_getupval,      // 44 OP_GETUPVAL
        &&op_setupval,      // 45 OP_SETUPVAL
        &&op_new,           // 46 OP_NEW
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
        &&op_getfield,      // 53 OP_GETFIELD
        &&op_setfield,      // 54 OP_SETFIELD
        &&op_invoke,        // 55 OP_INVOKE
        &&op_super,         // 56 OP_SUPER
        &&op_throw,         // 57 OP_THROW
        &&op_try,           // 58 OP_TRY
        &&op_endtry,        // 59 OP_ENDTRY
        &&op_isinstance,    // 60 OP_ISINSTANCE
        &&op_default,       // 61 OP_DEFAULT
        &&op_kwargs,        // 62 OP_KWARGS
        &&op_kcall,         // 63 OP_KCALL
        &&op_coalesce,      // 64 OP_COALESCE
        &&op_memberget_safe,// 65 OP_MEMBERGET_SAFE
        &&op_indexget_safe, // 66 OP_INDEXGET_SAFE
        &&op_slice_safe,    // 67 OP_SLICE_SAFE
        &&op_import,        // 68 OP_IMPORT
        &&op_tryinit,       // 69 OP_TRYINIT
        &&op_halt,          // 70 OP_HALT
        &&op_lt_jz,         // 71 OP_LT_JZ
        &&op_le_jz,         // 72 OP_LE_JZ
        &&op_gt_jz,         // 73 OP_GT_JZ
        &&op_ge_jz,         // 74 OP_GE_JZ
        &&op_eq_jz,         // 75 OP_EQ_JZ
        &&op_ne_jz,         // 76 OP_NE_JZ
        &&op_lt_jz_imm,     // 77 OP_LT_JZ_IMM
        &&op_le_jz_imm,     // 78 OP_LE_JZ_IMM
        &&op_gt_jz_imm,     // 79 OP_GT_JZ_IMM
        &&op_ge_jz_imm,     // 80 OP_GE_JZ_IMM
        &&op_eq_jz_imm,     // 81 OP_EQ_JZ_IMM
        &&op_ne_jz_imm      // 82 OP_NE_JZ_IMM
    };

    DECODE;
    goto *op_labels[OP(instr)];


#define SYNC_LOCALS() do { \
    current_frame = &vm->frames[vm->frame_count - 1]; \
    pc            = current_frame->chunk->code + current_frame->ip; \
    REG_BASE      = (vm)->stack + current_frame->base; \
    current_chunk = current_frame->chunk; \
} while (0)

#define PUSH_FRAME(fn, cl, base_reg, retreg, n, extra) do { \
    current_frame->ip = (int)(pc - current_chunk->code); \
    if (LUNA_UNLIKELY((vm)->frame_count >= MAX_FRAMES)) { \
        _exc = make_exception_instance(vm, vm->exception_class, "call stack overflow"); \
        goto op_throw; \
    } \
    CallFrame *_c = &(vm)->frames[(vm)->frame_count]; \
    Chunk *_cf = ((Object*)(fn))->type->get_chunk(make_obj((Object*)(fn))); \
    _c->chunk = _cf; \
    _c->ip = 0; \
    _c->base = (base_reg); \
    _c->ret_reg = (retreg); \
    _c->nargs = (n); \
    _c->kw_args = make_null(); \
    _c->leaf_ret_ip = 0; \
    _c->leaf_ret_chunk = NULL; \
    _c->leaf_ret_base = 0; \
    _c->leaf_ret_reg = 0; \
    _c->leaf_ret_closure = NULL; \
    _c->leaf_ret_fn = NULL; \
    frame_set_refs(_c, (Object*)(cl), (Object*)(fn)); \
    _c->saved_globals = NULL; \
    int _needed = _c->base + _cf->max_registers; \
    if (_needed > (vm)->stack_cap) { \
        (vm)->stack_cap = _needed < 64 ? 64 : _needed * 2; \
        (vm)->stack = realloc((vm)->stack, (vm)->stack_cap * sizeof(Value)); \
    } \
    for (int _i = (n) + (extra); _i < _cf->max_registers; _i++) \
        (vm)->stack[_c->base + _i] = make_null(); \
    (vm)->stack_count = _needed > (vm)->stack_count ? _needed : (vm)->stack_count; \
    (vm)->frame_count++; \
    SYNC_LOCALS(); \
} while (0)

#include "vm_opcodes.inc"

    
#undef PUSH_FRAME
#undef SYNC_LOCALS
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
    if (bytes_allocated > next_gc_threshold) {
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
    frame->leaf_ret_ip = 0;
    frame->leaf_ret_chunk = NULL;
    frame->leaf_ret_base = 0;
    frame->leaf_ret_reg = 0;
    frame->leaf_ret_closure = NULL;
    frame->leaf_ret_fn = NULL;
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

/* Call a Luna function/closure/bound-method/native from C.
 * Handles frame setup, execution, and result extraction.
 * Returns VM_OK on success, VM_EXCEPTION on unhandled error. */
VMResult vm_call_value(VM *vm, Value fn_val, Value *args, int arg_count, Value *out) {
    int saved_stack_count = vm->stack_count;
    int saved_frame_count = vm->frame_count;
    Type *t = (IS_OBJ(fn_val) && AS_OBJ(fn_val)->type) ? AS_OBJ(fn_val)->type : NULL;
    if (!t || !t->call) {
        vm->last_exception = make_exception_instance(vm, vm->exception_class,
            "attempt to call a non-function value");
        return VM_EXCEPTION;
    }

    /* Native callable: dispatch synchronously through the vtable. The vtable
       propagates exceptions via longjmp when a native_jump is active; we add a
       local jump so a top-level native failure still surfaces as VM_EXCEPTION. */
    Chunk *chunk = t->get_chunk ? t->get_chunk(fn_val) : NULL;
    if (!chunk) {
        LunaJump jump;
        jump.prev = vm->native_jump;
        vm->native_jump = &jump;
        VMResult result;
        if (setjmp(jump.env) == 0) {
            if (out) *out = t->call(vm, fn_val, args, arg_count);
            result = VM_OK;
        } else {
            result = VM_EXCEPTION;
        }
        vm->native_jump = jump.prev;
        return result;
    }

    /* Bytecode callable: push a real call frame. The core never names a concrete
       Luna callable kind; self (if any) and the chunk come from the vtable. */
    Value self_val = (t->get_self ? t->get_self(fn_val) : make_null());
    Object *callee = AS_OBJ(fn_val);
    int base_extra = is_null(self_val) ? 1 : 2; /* slot for fn, or self+fn */
    int needed_base = saved_stack_count + base_extra;
    int needed = needed_base + chunk->max_registers;
    if (needed > vm->stack_cap) {
        vm->stack_cap = needed < 64 ? 64 : (size_t)needed * 2;
        vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
        if (!vm->stack) return VM_ERROR;
    }

    /* Top-level call (no ghost frame needed) */
    if (saved_frame_count == 0) {
        vm->stack[saved_stack_count] = is_null(self_val) ? fn_val : self_val;
        for (int i = 0; i < arg_count; i++) {
            vm->stack[saved_stack_count + base_extra + i] = args[i];
        }
        int callee_base = saved_stack_count + base_extra;
        CallFrame *frame = &vm->frames[0];
        frame->chunk = chunk;
        frame->ip = 0;
        frame->base = callee_base;
        frame->ret_reg = 0;
        frame->nargs = arg_count;
        frame->kw_args = make_null();
        frame->closure = callee;
        frame->fn = callee;
        frame->saved_globals = NULL;
        frame->leaf_ret_ip = 0;
        frame->leaf_ret_chunk = NULL;
        frame->leaf_ret_base = 0;
        frame->leaf_ret_reg = 0;
        frame->leaf_ret_closure = NULL;
        frame->leaf_ret_fn = NULL;
        for (int i = arg_count; i < chunk->max_registers; i++) {
            vm->stack[callee_base + i] = make_null();
        }
        vm->stack_count = needed;
        vm->frame_count = 1;

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

        if (out) *out = vm->last_return_value;
        vm->stack_count = saved_stack_count;
        vm->frame_count = 0;
        return result;
    }

    /* Nested call: use ghost frame to return cleanly */
    {
        vm->stack[saved_stack_count] = is_null(self_val) ? fn_val : self_val;
        for (int i = 0; i < arg_count; i++) {
            vm->stack[saved_stack_count + base_extra + i] = args[i];
        }
        int callee_base = saved_stack_count + base_extra;

        /* Build ghost chunk */
        Chunk ghost;
        chunk_init(&ghost, "<ghost>");
        chunk_emit_ABC(&ghost, 0, OP_HALT, 0, 0, 0);
        ghost.max_registers = 1;
        ghost.source_path = NULL;

        CallFrame *dummy = &vm->frames[saved_frame_count];
        dummy->chunk = &ghost;
        dummy->ip = 0;
        dummy->base = saved_stack_count;
        dummy->ret_reg = 0;
        dummy->nargs = 0;
        dummy->kw_args = make_null();
        dummy->closure = NULL;
        dummy->fn = NULL;
        dummy->saved_globals = NULL;
        dummy->leaf_ret_ip = 0;
        dummy->leaf_ret_chunk = NULL;
        dummy->leaf_ret_base = 0;
        dummy->leaf_ret_reg = 0;
        dummy->leaf_ret_closure = NULL;
        dummy->leaf_ret_fn = NULL;

        CallFrame *frame = &vm->frames[saved_frame_count + 1];
        frame->chunk = chunk;
        frame->ip = 0;
        frame->base = callee_base;
        frame->ret_reg = 0;
        frame->nargs = arg_count;
        frame->kw_args = make_null();
        frame->closure = callee;
        frame->fn = callee;
        frame->saved_globals = NULL;
        frame->leaf_ret_ip = 0;
        frame->leaf_ret_chunk = NULL;
        frame->leaf_ret_base = 0;
        frame->leaf_ret_reg = 0;
        frame->leaf_ret_closure = NULL;
        frame->leaf_ret_fn = NULL;
        for (int i = arg_count; i < chunk->max_registers; i++) {
            vm->stack[callee_base + i] = make_null();
        }
        vm->stack_count = needed;
        vm->frame_count = saved_frame_count + 2;

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

        if (out) *out = vm->last_return_value;
        chunk_free(&ghost);
        vm->stack_count = saved_stack_count;
        vm->frame_count = saved_frame_count;
        return result;
    }
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

void vm_mark_value(VM *vm, Value v) {
    (void)vm;
    mark_value(v);
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
        if (obj->type->kind == OBJ_INSTANCE) {
            ObjInstance *inst = (ObjInstance*)obj;
            class_name = inst->class_name ? inst->class_name : "Error";
        }
    }
    /* Get the raw message from the exception instance (not value_to_string which wraps in <>) */
    const char *raw_msg = error_msg;
    if (IS_OBJ(vm->last_exception) && AS_OBJ(vm->last_exception)) {
        Object *obj = AS_OBJ(vm->last_exception);
        if (obj->type->kind == OBJ_INSTANCE) {
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
