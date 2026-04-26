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

/* Declared in vm_builtins.c */
void vm_register_builtins(VM *vm);
bool vm_invoke_list(VM *vm, ObjList  *list, const char *method, Value *args, int nargs, Value *result);
bool vm_invoke_dict(VM *vm, ObjDict  *dict, const char *method, Value *args, int nargs, Value *result);

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
            if (r == 0.0) return make_null();
            return make_double(l / r);
        }
        case OP_MOD: {
            if (r == 0.0) return make_null();
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
            for (int i = 0; i < inst->method_count; i++) {
                mark_object((Object*)inst->methods[i]);
            }
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
        if (vm->frames[i].chunk) {
            for (int j = 0; j < vm->frames[i].chunk->const_count; j++) {
                mark_value(vm->frames[i].chunk->constants[j]);
            }
        }
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
                if (inst->methods) {
                    for (int i = 0; i < inst->method_count; i++) {
                        if (inst->methods[i]) release_obj((Object*)inst->methods[i]);
                    }
                    inst->method_count = 0;
                }
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

void vm_init(VM *vm) {
    memset(vm, 0, sizeof(VM));
    vm_register_builtins(vm);
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
        vm->last_exception = make_obj((Object*)new_exception("vm: frame overflow"));
        return VM_EXCEPTION;
    }
    CallFrame *frame = &vm->frames[vm->frame_count++];
    frame->chunk = chunk;
    frame->ip = 0;
    frame->base = vm->stack_count;
    frame->ret_reg = 0;
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
        &&op_copy,          // 6  OP_COPY
        &&op_swap,          // 7  OP_SWAP
        &&op_add,           // 8  OP_ADD
        &&op_sub,           // 9  OP_SUB
        &&op_mul,           // 10 OP_MUL
        &&op_div,           // 11 OP_DIV
        &&op_mod,           // 12 OP_MOD
        &&op_neg,           // 13 OP_NEG
        &&op_band,          // 14 OP_BAND
        &&op_bor,           // 15 OP_BOR
        &&op_bxor,          // 16 OP_BXOR
        &&op_bnot,          // 17 OP_BNOT
        &&op_shl,           // 18 OP_SHL
        &&op_shr,           // 19 OP_SHR
        &&op_addi,          // 20 OP_ADDI
        &&op_subi,          // 21 OP_SUBI
        &&op_eq,            // 22 OP_EQ
        &&op_ne,            // 23 OP_NE
        &&op_lt,            // 24 OP_LT
        &&op_le,            // 25 OP_LE
        &&op_gt,            // 26 OP_GT
        &&op_ge,            // 27 OP_GE
        &&op_not,           // 28 OP_NOT
        &&op_jmp,           // 29 OP_JMP
        &&op_jz,            // 30 OP_JZ
        &&op_jnz,           // 31 OP_JNZ
        &&op_call,          // 32 OP_CALL
        &&op_ret,           // 33 OP_RET
        &&op_enter,         // 34 OP_ENTER
        &&op_leave,         // 35 OP_LEAVE
        &&op_closure,       // 36 OP_CLOSURE
        &&op_getglobal,     // 37 OP_GETGLOBAL
        &&op_setglobal,     // 38 OP_SETGLOBAL
        &&op_getupval,      // 39 OP_GETUPVAL
        &&op_setupval,      // 40 OP_SETUPVAL
        &&op_new,           // 41 OP_NEW
        &&op_newdict,       // 42 OP_NEWDICT
        &&op_newlist,       // 43 OP_NEWLIST
        &&op_listappend,    // 44 OP_LISTAPPEND
        &&op_getiter,       // 45 OP_GETITER
        &&op_forloop,       // 46 OP_FORLOOP
        &&op_indexget,      // 47 OP_INDEXGET
        &&op_indexset,      // 48 OP_INDEXSET
        &&op_memberget,     // 49 OP_MEMBERGET
        &&op_memberset,     // 50 OP_MEMBERSET
        &&op_invoke,        // 51 OP_INVOKE
        &&op_unimplemented, // 52 OP_SUPER
        &&op_throw,         // 53 OP_THROW
        &&op_try,           // 54 OP_TRY
        &&op_endtry,        // 55 OP_ENDTRY
        &&op_halt           // 56 OP_HALT
    };

    DECODE;
    goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 5.  MOVE                                                 */
    /* -------------------------------------------------------- */
    op_move:
        SET_REG(RA, REG(RB));
        DECODE; goto *op_labels[OP(instr)];

    op_copy: {
        SET_REG(RA, REG(RB));
        DECODE; goto *op_labels[OP(instr)];
    }

    op_swap: {
        Value t = REG(RA); REG(RA) = REG(RB); REG(RB) = t;
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 0-4.  LOADK / LOADNULL / LOADTRUE / LOADFALSE / LOADI               */
    /* -------------------------------------------------------- */
    op_loadk:
        SET_REG(RA, CONST(BX));
        DECODE; goto *op_labels[OP(instr)];
    op_loadnull:
        SET_REG_PRIM(RA, make_null());
        DECODE; goto *op_labels[OP(instr)];
    op_loadi:
        SET_REG_PRIM(RA, make_int(SBX));
        DECODE; goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 2-3.  LOADTRUE / LOADFALSE                             */
    /* -------------------------------------------------------- */
    op_loadtrue:
        SET_REG_PRIM(RA, make_bool(true));
        DECODE; goto *op_labels[OP(instr)];
    op_loadfalse:
        SET_REG_PRIM(RA, make_bool(false));
        DECODE; goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 8-12.  Arithmetic                                         */
    /* -------------------------------------------------------- */
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
    op_sub: { Value _L=RKB,_R=RKC; if(IS_INT(_L)&&IS_INT(_R)){SET_REG_PRIM(RA,make_int(AS_INT(_L)-AS_INT(_R)));}else{SET_REG_PRIM(RA,do_arith(_L,_R,OP_SUB));} DECODE; goto *op_labels[OP(instr)]; }
    op_mul: { Value _L=RKB,_R=RKC; if(IS_INT(_L)&&IS_INT(_R)){SET_REG_PRIM(RA,make_int(AS_INT(_L)*AS_INT(_R)));}else{SET_REG_PRIM(RA,do_arith(_L,_R,OP_MUL));} DECODE; goto *op_labels[OP(instr)]; }
    op_div: {
        Value _L = RKB, _R = RKC;
        if (IS_INT(_L) && IS_INT(_R)) {
            int64_t ri = AS_INT(_R);
            if (!ri) { release_value(_exc); _exc = make_obj((Object*)new_exception("vm: div/0")); goto op_throw; }
            SET_REG_PRIM(RA, make_int(AS_INT(_L) / ri));
        } else if (is_num(_L) && is_num(_R)) {
            double r = to_f64(_R);
            if (r == 0.0) { release_value(_exc); _exc = make_obj((Object*)new_exception("vm: div/0")); goto op_throw; }
            SET_REG_PRIM(RA, make_double(to_f64(_L) / r));
        } else {
            SET_REG_PRIM(RA, make_null());
        }
        DECODE; goto *op_labels[OP(instr)];
    }
    op_mod: {
        Value _L = RKB, _R = RKC;
        if (IS_INT(_L) && IS_INT(_R)) {
            int64_t ri = AS_INT(_R);
            if (!ri) { release_value(_exc); _exc = make_obj((Object*)new_exception("vm: mod/0")); goto op_throw; }
            SET_REG_PRIM(RA, make_int(AS_INT(_L) % ri));
        } else if (is_num(_L) && is_num(_R)) {
            double r = to_f64(_R);
            if (r == 0.0) { release_value(_exc); _exc = make_obj((Object*)new_exception("vm: mod/0")); goto op_throw; }
            SET_REG_PRIM(RA, make_double(fmod(to_f64(_L), r)));
        } else {
            SET_REG_PRIM(RA, make_null());
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Integer-immediate fast paths ---- */
    op_addi: {
        Value v = REG(RA);
        if (IS_INT(v))         REG(RA) = make_int(AS_INT(v) + (int8_t)B);
        else if (IS_DOUBLE(v)) REG(RA) = make_double(AS_DOUBLE(v) + (double)(int8_t)B);
        DECODE; goto *op_labels[OP(instr)];
    }
    op_subi: {
        Value v = REG(RA);
        if (IS_INT(v))         REG(RA) = make_int(AS_INT(v) - (int8_t)B);
        else if (IS_DOUBLE(v)) REG(RA) = make_double(AS_DOUBLE(v) - (double)(int8_t)B);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 22-27.  Comparisons                                      */
    /* -------------------------------------------------------- */
    op_lt: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,IS_INT(_L)&&IS_INT(_R)?make_bool(AS_INT(_L)< AS_INT(_R)):do_cmp(_L,_R,OP_LT)); DECODE; goto *op_labels[OP(instr)]; }
    op_le: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,IS_INT(_L)&&IS_INT(_R)?make_bool(AS_INT(_L)<=AS_INT(_R)):do_cmp(_L,_R,OP_LE)); DECODE; goto *op_labels[OP(instr)]; }
    op_gt: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,IS_INT(_L)&&IS_INT(_R)?make_bool(AS_INT(_L)> AS_INT(_R)):do_cmp(_L,_R,OP_GT)); DECODE; goto *op_labels[OP(instr)]; }
    op_ge: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,IS_INT(_L)&&IS_INT(_R)?make_bool(AS_INT(_L)>=AS_INT(_R)):do_cmp(_L,_R,OP_GE)); DECODE; goto *op_labels[OP(instr)]; }
    op_eq: SET_REG_PRIM(RA, do_cmp(RKB, RKC, OP_EQ)); DECODE; goto *op_labels[OP(instr)];
    op_ne: SET_REG_PRIM(RA, do_cmp(RKB, RKC, OP_NE)); DECODE; goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 14-19.  Bitwise                                          */
    /* -------------------------------------------------------- */
    op_band: SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) & to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    op_bor:  SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) | to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    op_bxor: SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) ^ to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    op_shl:  SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) << to_i64(REG(RC)))); DECODE; goto *op_labels[OP(instr)];
    op_shr:  SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) >> to_i64(REG(RC)))); DECODE; goto *op_labels[OP(instr)];
    op_bnot: SET_REG_PRIM(RA, make_int(~to_i64(REG(RB)))); DECODE; goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 13 & 28.  NEG / NOT                                        */
    /* -------------------------------------------------------- */
    op_not: SET_REG_PRIM(RA, make_bool(!is_truthy(REG(RB)))); DECODE; goto *op_labels[OP(instr)];

    op_neg: {
        Value v = REG(RB);
        if (IS_INT(v))    { SET_REG_PRIM(RA, make_int(-AS_INT(v)));     DECODE; goto *op_labels[OP(instr)]; }
        if (IS_DOUBLE(v)) { SET_REG_PRIM(RA, make_double(-AS_DOUBLE(v))); DECODE; goto *op_labels[OP(instr)]; }
        SET_REG_PRIM(RA, make_null());
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 29-31.  Jumps                                            */
    /* -------------------------------------------------------- */
    op_jmp:
        IP += sBx;
        DECODE; goto *op_labels[OP(instr)];
    op_jz:
        if (!is_truthy(REG(RA))) IP += sBx;
        DECODE; goto *op_labels[OP(instr)];
    op_jnz:
        if (is_truthy(REG(RA))) IP += sBx;
        DECODE; goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 32.  CALL                                                */
    /* -------------------------------------------------------- */
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
                    _exc = make_obj((Object*)new_exception("vm: call stack overflow"));
                    goto op_throw;
                }
                CallFrame *caller = &FRAME;
                CallFrame *callee = &vm->frames[vm->frame_count];
                callee->chunk = fn->chunk;
                callee->ip = 0;
                callee->base = caller->base + fn_reg + 1;
                callee->closure = NULL;
                callee->ret_reg = ret_reg;
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
                    _exc = make_obj((Object*)new_exception("vm: call stack overflow"));
                    goto op_throw;
                }
                CallFrame *caller = &FRAME;
                CallFrame *callee = &vm->frames[vm->frame_count];
                callee->chunk = fn->chunk;
                callee->ip = 0;
                callee->base = caller->base + fn_reg + 1;
                callee->closure = cl;
                callee->ret_reg = ret_reg;
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
            _exc = make_obj((Object*)new_exception(buf));
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 33.  RET                                                 */
    /* -------------------------------------------------------- */
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
        close_upvalues(vm, vm->frame_count);
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

    /* -------------------------------------------------------- */
    /* 35.  LEAVE (close upvalues)                               */
    /* -------------------------------------------------------- */
    op_leave:
        close_upvalues(vm, vm->frame_count);
        DECODE; goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 42-43.  NEWDICT / NEWLIST                                */
    /* -------------------------------------------------------- */
    op_newlist: SET_REG(RA, make_obj((Object*)new_list((int)Bx))); DECODE; goto *op_labels[OP(instr)];
    op_newdict: SET_REG(RA, make_obj((Object*)new_dict())); DECODE; goto *op_labels[OP(instr)];
    
    op_listappend: {
        Value lst = REG(RA);
        Value val = REG(RB);
        if (IS_LIST(lst)) {
            list_add((ObjList*)AS_OBJ(lst), val);
        } else {
            release_value(_exc);
            _exc = make_obj((Object*)new_exception("vm: listappend on non-list"));
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

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
                    _exc = make_obj((Object*)new_exception("vm: object is not iterable"));
                    goto op_throw;
            }
        } else {
            release_value(_exc);
            _exc = make_obj((Object*)new_exception("vm: non-object is not iterable"));
            goto op_throw;
        }

        if (has_next) {
            IP += SBX;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 41.  NEW                                             */
    /* -------------------------------------------------------- */
    op_new: {
        ObjString *cls_str = KSTROBJ(BX);
        if (!cls_str) { SET_REG_PRIM(RA, make_null()); DECODE; goto *op_labels[OP(instr)]; }
        const char *cls_name = cls_str->chars;
        Value proto_val;
        ObjInstance *proto = NULL;
        if (vm_get_global(vm, cls_name, &proto_val) && IS_INSTANCE(proto_val)) {
            proto = (ObjInstance*)AS_OBJ(proto_val);
        }
        ObjInstance *new_inst = new_instance(cls_name, proto ? proto->base_class : NULL, 4);
        if (proto) {
            for (int i = 0; i < proto->field_count; i++)
                instance_set_field(new_inst, proto->field_names[i], proto->fields[i]);
            
            if (proto->method_count > 0) {
                new_inst->methods = malloc(sizeof(ObjFunction*) * proto->method_count);
                memcpy(new_inst->methods, proto->methods, sizeof(ObjFunction*) * proto->method_count);
                new_inst->method_count = proto->method_count;
                new_inst->method_capacity = proto->method_count;
                for (int i = 0; i < proto->method_count; i++) {
                    if (new_inst->methods[i]) retain_obj((Object*)new_inst->methods[i]);
                }
            }
        }
        SET_REG(RA, make_obj((Object*)new_inst));
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 47-48.  INDEXGET / INDEXSET                              */
    /* -------------------------------------------------------- */
    op_indexget: {
        Value obj = REG(RB);
        Value key = REG(RC);
        if (IS_OBJ(obj) && AS_OBJ(obj)) {
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
        } else SET_REG_PRIM(RA, make_null());
        DECODE; goto *op_labels[OP(instr)];
    }
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

    /* -------------------------------------------------------- */
    /* 39-40.  GETUPVAL / SETUPVAL                              */
    /* -------------------------------------------------------- */
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

    /* -------------------------------------------------------- */
    /* 49-50.  MEMBERGET / MEMBERSET                            */
    /* -------------------------------------------------------- */
    op_memberget: {
        Value obj = REG(RB);
        ObjString *field = KSTROBJ(RC);
        if (!IS_OBJ(obj) || !AS_OBJ(obj)) { REG(RA) = make_null(); DECODE; goto *op_labels[OP(instr)]; }
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
                        SET_REG_PRIM(RA, make_null());
                    }
                }
                break;
            }
            case OBJ_LIST:     SET_REG_PRIM(RA, (field->length == 6 && !memcmp(field->chars, "length", 6)) ? make_int(list_length((ObjList*)AS_OBJ(obj))) : make_null()); break;
            case OBJ_DICT:     SET_REG_PRIM(RA, (field->length == 6 && !memcmp(field->chars, "length", 6)) ? make_int(dict_length((ObjDict*)AS_OBJ(obj))) : make_null()); break;
            case OBJ_STRING:   SET_REG_PRIM(RA, (field->length == 6 && !memcmp(field->chars, "length", 6)) ? make_int(((ObjString*)AS_OBJ(obj))->length) : make_null()); break;
            default: SET_REG_PRIM(RA, make_null()); break;
        }
        DECODE; goto *op_labels[OP(instr)];
    }
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

    /* -------------------------------------------------------- */
    /* 51.  INVOKE                                              */
    /* -------------------------------------------------------- */
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
                case OBJ_INSTANCE: {
                    ObjInstance *obj_inst = (ObjInstance*)AS_OBJ(obj);
                    ObjString *mname_obj = (ObjString*)AS_OBJ(method_val);
                    uint32_t h = ((uint32_t)(uintptr_t)obj_inst ^ mname_obj->hash);
                    int idx = h & (IC_CACHE_SIZE - 1);
                    IC_MemberEntry *ic = &vm->method_ic[idx];
                    int mi = -1;
                    if (ic->inst == obj_inst && ic->name == mname_obj) {
                        mi = ic->index;
                    } else {
                        for (int i = 0; i < obj_inst->method_count; i++) {
                            ObjFunction *m = obj_inst->methods[i];
                            if (m && strcmp(m->name, mname_obj->chars) == 0) { mi = i; break; }
                        }
                        if (mi >= 0) { ic->inst = obj_inst; ic->name = mname_obj; ic->index = mi; }
                    }
                    if (mi >= 0) {
                        ObjFunction *m = obj_inst->methods[mi];
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
            _exc = make_obj((Object*)new_exception(buf));
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 36.  CLOSURE                                             */
    /* -------------------------------------------------------- */
    op_closure: {
        uint8_t k = B;
        uint8_t dst = RA;
        Value fn_val = CONST(k);
        if (!IS_FUNCTION(fn_val)) {
            release_value(_exc);
            _exc = make_obj((Object*)new_exception("vm: CLOSURE needs function constant"));
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

    /* -------------------------------------------------------- */
    /* 34.  ENTER                                               */
    /* -------------------------------------------------------- */
    op_enter:
        /* Stack is pre-allocated by vm_run_chunk based on max_registers */
        DECODE; goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 37.  GETGLOBAL                                           */
    /* -------------------------------------------------------- */
    op_getglobal: {
        Value v;
        if (vm_get_global_fast(vm, KSTROBJ(BX), &v)) SET_REG(RA, v);
        else SET_REG_PRIM(RA, make_null());
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 38.  SETGLOBAL                                           */
    /* -------------------------------------------------------- */
    op_setglobal: {
        vm_set_global(vm, KSTR(BX), REG(RA), false);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 53.  THROW                                               */
    /* -------------------------------------------------------- */
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

    /* -------------------------------------------------------- */
    /* 54.  TRY                                                 */
    /* -------------------------------------------------------- */
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

    /* -------------------------------------------------------- */
    /* 55.  ENDTRY                                              */
    /* -------------------------------------------------------- */
    op_endtry: {
        if (vm->try_stack) {
            TryFrame *tf = vm->try_stack;
            vm->try_stack = tf->next;
            free(tf);
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 56.  HALT                                               */
    /* -------------------------------------------------------- */
    op_halt:
        return VM_OK;

    /* -------------------------------------------------------- */
    /* Unimplemented / stub opcodes                             */
    /* -------------------------------------------------------- */
    op_unimplemented:
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "vm: unimplemented opcode %d", OP(instr));
            release_value(_exc);
            _exc = make_obj((Object*)new_exception(buf));
            goto op_throw;
        }

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
/* Upvalue helpers                                               */
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
    if (IS_EXCEPTION(exception)) {
        ObjException *ex = (ObjException*)AS_OBJ(exception);
        fprintf(stderr, "Uncaught exception: %s\n", ex->message);
    } else {
        char *s = value_to_string(exception);
        fprintf(stderr, "Uncaught exception: %s\n", s);
        free(s);
    }
    return 1;
}
