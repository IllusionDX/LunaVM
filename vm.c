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
    if (op == OP_ADD && IS_OBJ(L) && AS_OBJ(L) && AS_OBJ(L)->type == OBJ_STRING) {
        ObjString *ls = (ObjString*)AS_OBJ(L);
        const char *rs;
        int rs_len;
        char *rs_tmp = NULL;
        if (IS_OBJ(R) && AS_OBJ(R) && AS_OBJ(R)->type == OBJ_STRING) {
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
                if (!ri) { fprintf(stderr, "vm: div/0\n"); return make_null(); }
                return make_int(li / ri);
            }
            case OP_MOD: {
                if (!ri) { fprintf(stderr, "vm: mod/0\n"); return make_null(); }
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
            if (r == 0.0) { fprintf(stderr, "vm: div/0\n"); return make_null(); }
            return make_double(l / r);
        }
        case OP_MOD: {
            if (r == 0.0) { fprintf(stderr, "vm: mod/0\n"); return make_null(); }
            return make_double(fmod(l, r));
        }
        default: return make_null();
    }
}
static inline Value do_cmp(Value L, Value R, OpCode op) {
    if (is_num(L) && is_num(R)) {
        double a = to_f64(L), b = to_f64(R);
        switch (op) { case OP_LT: return make_bool(a < b); case OP_LE: return make_bool(a <= b); case OP_GT: return make_bool(a > b); case OP_GE: return make_bool(a >= b); default: break; }
    }
    /* Fast path: interned string equality is pointer comparison */
    if (IS_OBJ(L) && IS_OBJ(R) && AS_OBJ(L) && AS_OBJ(R) &&
        AS_OBJ(L)->type == OBJ_STRING && AS_OBJ(R)->type == OBJ_STRING) {
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
    Object *o = vm->objects;
    while (o) { Object *nx = o->next; free_object(o); o = nx; }
    while (vm->try_stack) {
        TryFrame *tf = vm->try_stack;
        vm->try_stack = tf->next;
        free(tf);
    }
    close_upvalues(vm, 0);
    value_free_intern_table();
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
    if (vm->frame_count >= MAX_FRAMES) { fprintf(stderr, "vm: frame overflow\n"); return VM_ERROR; }
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

#define DECODE \
    do { \
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
        &&op_eq,            // 20 OP_EQ
        &&op_ne,            // 21 OP_NE
        &&op_lt,            // 22 OP_LT
        &&op_le,            // 23 OP_LE
        &&op_gt,            // 24 OP_GT
        &&op_ge,            // 25 OP_GE
        &&op_not,           // 26 OP_NOT
        &&op_jmp,           // 27 OP_JMP
        &&op_jz,            // 28 OP_JZ
        &&op_jnz,           // 29 OP_JNZ
        &&op_call,          // 30 OP_CALL
        &&op_ret,           // 31 OP_RET
        &&op_enter,         // 32 OP_ENTER
        &&op_leave,         // 33 OP_LEAVE
        &&op_closure,       // 34 OP_CLOSURE
        &&op_getglobal,     // 35 OP_GETGLOBAL
        &&op_setglobal,     // 36 OP_SETGLOBAL
        &&op_getupval,      // 37 OP_GETUPVAL
        &&op_setupval,      // 38 OP_SETUPVAL
        &&op_new,           // 39 OP_NEW
        &&op_newdict,       // 40 OP_NEWDICT
        &&op_newlist,       // 41 OP_NEWLIST
        &&op_listappend,    // 42 OP_LISTAPPEND
        &&op_getiter,       // 43 OP_GETITER
        &&op_forloop,       // 44 OP_FORLOOP
        &&op_indexget,      // 45 OP_INDEXGET
        &&op_indexset,      // 46 OP_INDEXSET
        &&op_memberget,     // 47 OP_MEMBERGET
        &&op_memberset,     // 48 OP_MEMBERSET
        &&op_invoke,        // 49 OP_INVOKE
        &&op_unimplemented, // 50 OP_SUPER
        &&op_throw,         // 51 OP_THROW
        &&op_try,           // 52 OP_TRY
        &&op_endtry,        // 53 OP_ENDTRY
        &&op_halt           // 54 OP_HALT
    };

    DECODE;
    goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 0.  MOVE                                                 */
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
    /* 1-4.  LOADK / LOADBOOL / LOADNULL / LOADI               */
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
    /* 35-36.  LOADTRUE / LOADFALSE                             */
    /* -------------------------------------------------------- */
    op_loadtrue:
        SET_REG_PRIM(RA, make_bool(true));
        DECODE; goto *op_labels[OP(instr)];
    op_loadfalse:
        SET_REG_PRIM(RA, make_bool(false));
        DECODE; goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 5-9.  Arithmetic                                         */
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
    op_sub:  SET_REG_PRIM(RA, do_arith(RKB, RKC, OP_SUB));  DECODE; goto *op_labels[OP(instr)];
    op_mul:  SET_REG_PRIM(RA, do_arith(RKB, RKC, OP_MUL));  DECODE; goto *op_labels[OP(instr)];
    op_div:  SET_REG_PRIM(RA, do_arith(RKB, RKC, OP_DIV));  DECODE; goto *op_labels[OP(instr)];
    op_mod:  SET_REG_PRIM(RA, do_arith(RKB, RKC, OP_MOD));  DECODE; goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 11-16.  Comparisons                                      */
    /* -------------------------------------------------------- */
    op_lt: SET_REG_PRIM(RA, do_cmp(RKB, RKC, OP_LT)); DECODE; goto *op_labels[OP(instr)];
    op_le: SET_REG_PRIM(RA, do_cmp(RKB, RKC, OP_LE)); DECODE; goto *op_labels[OP(instr)];
    op_eq: SET_REG_PRIM(RA, do_cmp(RKB, RKC, OP_EQ)); DECODE; goto *op_labels[OP(instr)];
    op_ne: SET_REG_PRIM(RA, do_cmp(RKB, RKC, OP_NE)); DECODE; goto *op_labels[OP(instr)];
    op_gt: SET_REG_PRIM(RA, do_cmp(RKB, RKC, OP_GT)); DECODE; goto *op_labels[OP(instr)];
    op_ge: SET_REG_PRIM(RA, do_cmp(RKB, RKC, OP_GE)); DECODE; goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 17-22.  Bitwise                                          */
    /* -------------------------------------------------------- */
    op_band: SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) & to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    op_bor:  SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) | to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    op_bxor: SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) ^ to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    op_shl:  SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) << to_i64(REG(RC)))); DECODE; goto *op_labels[OP(instr)];
    op_shr:  SET_REG_PRIM(RA, make_int(to_i64(REG(RB)) >> to_i64(REG(RC)))); DECODE; goto *op_labels[OP(instr)];
    op_bnot: SET_REG_PRIM(RA, make_int(~to_i64(REG(RB)))); DECODE; goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 23-24.  NOT / NEG                                        */
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
    /* 25-27.  Jumps                                            */
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
    /* 28.  CALL                                                */
    /* -------------------------------------------------------- */
    op_call: {
        uint8_t ret_reg = RA;
        uint8_t fn_reg  = B;
        uint8_t nargs   = C;
        Value fn_val = REG(fn_reg);
        if (!IS_OBJ(fn_val) || !AS_OBJ(fn_val)) {
            char *s = value_to_string(fn_val);
            fprintf(stderr, "vm: attempt to call non-function (got %s)\n", s);
            free(s);
            SET_REG_PRIM(ret_reg, make_null());
            DECODE; goto *op_labels[OP(instr)];
        }
        if (AS_OBJ(fn_val)->type == OBJ_FUNCTION) {
            ObjFunction *fn = (ObjFunction *)AS_OBJ(fn_val);
            if (fn->is_native) {
                Value scratch[256];
                for (int i = 0; i < nargs; i++) scratch[i] = REG(fn_reg + 1 + i);
                Value result = fn->native_fn(vm, scratch, nargs);
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            } else {
                if (vm->frame_count >= MAX_FRAMES) {
                    fprintf(stderr, "vm: call stack overflow\n");
                    SET_REG_PRIM(ret_reg, make_null());
                    DECODE; goto *op_labels[OP(instr)];
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
        } else if (AS_OBJ(fn_val)->type == OBJ_CLOSURE) {
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
                    fprintf(stderr, "vm: call stack overflow\n");
                    SET_REG_PRIM(ret_reg, make_null());
                    DECODE; goto *op_labels[OP(instr)];
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
            fprintf(stderr, "vm: attempt to call non-function (type=%d, value=%s)\n", AS_OBJ(fn_val)->type, s);
            free(s);
            SET_REG_PRIM(ret_reg, make_null());
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 29.  RET                                                 */
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
    /* 30.  CLS  (close upvalues)                               */
    /* -------------------------------------------------------- */
    op_leave:
        close_upvalues(vm, vm->frame_count);
        DECODE; goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 31-32.  NEWLIST / NEWDICT                                */
    /* -------------------------------------------------------- */
    op_newlist: SET_REG(RA, make_obj((Object*)new_list((int)Bx))); DECODE; goto *op_labels[OP(instr)];
    op_newdict: SET_REG(RA, make_obj((Object*)new_dict())); DECODE; goto *op_labels[OP(instr)];
    
    op_listappend: {
        Value lst = REG(RA);
        Value val = REG(RB);
        if (IS_OBJ(lst) && AS_OBJ(lst) && AS_OBJ(lst)->type == OBJ_LIST) {
            list_add((ObjList*)AS_OBJ(lst), val);
        } else {
            fprintf(stderr, "vm: listappend on non-list\n");
            return VM_ERROR;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    op_getiter: {
        Value iter = REG(RB);
        if (IS_OBJ(iter) && AS_OBJ(iter) && AS_OBJ(iter)->type == OBJ_DICT) {
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
                    fprintf(stderr, "vm: object is not iterable\n");
                    return VM_ERROR;
            }
        } else {
            fprintf(stderr, "vm: non-object is not iterable\n");
            return VM_ERROR;
        }

        if (has_next) {
            IP += SBX;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 37.  NEWINSTANCE                                         */
    /* -------------------------------------------------------- */
    op_new: {
        ObjString *cls_str = KSTROBJ(BX);
        if (!cls_str) { SET_REG_PRIM(RA, make_null()); DECODE; goto *op_labels[OP(instr)]; }
        const char *cls_name = cls_str->chars;
        Value proto_val;
        ObjInstance *proto = NULL;
        if (vm_get_global(vm, cls_name, &proto_val) && IS_OBJ(proto_val) && AS_OBJ(proto_val) && AS_OBJ(proto_val)->type == OBJ_INSTANCE) {
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
    /* 33-34.  INDEXGET / INDEXSET                              */
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
            if (AS_OBJ(obj)->type == OBJ_LIST && IS_INT(key)) list_set((ObjList*)AS_OBJ(obj), AS_INT(key), val);
            else if (AS_OBJ(obj)->type == OBJ_DICT) dict_set((ObjDict*)AS_OBJ(obj), key, val);
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 38-39.  GETUPVAL / SETUPVAL                              */
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
    /* 35-36.  MEMBERGET / MEMBERSET                            */
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
        if (IS_OBJ(obj) && AS_OBJ(obj) && AS_OBJ(obj)->type == OBJ_INSTANCE) {
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
    /* 37.  INVOKE                                              */
    /* -------------------------------------------------------- */
    op_invoke: {
        uint8_t ret_reg = RA;
        uint8_t obj_reg = B;
        uint8_t nargs   = C;
        Value method_val = REG(obj_reg + 1);
        Value obj = REG(obj_reg);
        if (!IS_OBJ(method_val) || !AS_OBJ(method_val) || AS_OBJ(method_val)->type != OBJ_STRING) { SET_REG_PRIM(ret_reg, make_null()); DECODE; goto *op_labels[OP(instr)]; }
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
        else { fprintf(stderr, "vm: unknown method '%s'\n", mname); SET_REG_PRIM(ret_reg, make_null()); }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 40.  CLOSURE                                             */
    /* -------------------------------------------------------- */
    op_closure: {
        uint8_t k = B;
        uint8_t dst = RA;
        Value fn_val = CONST(k);
        if (!IS_OBJ(fn_val) || AS_OBJ(fn_val)->type != OBJ_FUNCTION) {
            fprintf(stderr, "vm: CLOSURE needs function constant\n");
            SET_REG(dst, make_null());
            DECODE; goto *op_labels[OP(instr)];
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
    /* 32.  ENTER                                               */
    /* -------------------------------------------------------- */
    op_enter:
        /* Stack is pre-allocated by vm_run_chunk based on max_registers */
        DECODE; goto *op_labels[OP(instr)];

    /* -------------------------------------------------------- */
    /* 35.  GETGLOBAL                                           */
    /* -------------------------------------------------------- */
    op_getglobal: {
        Value v;
        if (vm_get_global_fast(vm, KSTROBJ(BX), &v)) SET_REG(RA, v);
        else SET_REG_PRIM(RA, make_null());
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 36.  SETGLOBAL                                           */
    /* -------------------------------------------------------- */
    op_setglobal: {
        vm_set_global(vm, KSTR(BX), REG(RA), false);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* -------------------------------------------------------- */
    /* 48.  THROW                                               */
    /* -------------------------------------------------------- */
    op_throw: {
        Value exc = REG(RA);
        vm->last_exception = exc;
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
                SET_REG(tf->exc_reg, exc);
                int catch_ip = tf->catch_ip;
                vm->try_stack = tf->next;
                free(tf);
                CHUNK = FRAME.chunk;
                IP = catch_ip;
                DECODE;
                goto *op_labels[OP(instr)];
            }
            /* stale frame from deeper call, pop it */
            vm->try_stack = tf->next;
            free(tf);
        }
        fprintf(stderr, "vm: unhandled exception\n");
        return VM_EXCEPTION;
    }

    /* -------------------------------------------------------- */
    /* 49.  TRY                                                 */
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
    /* 50.  ENDTRY                                              */
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
    /* 51.  HALT                                               */
    /* -------------------------------------------------------- */
    op_halt:
        return VM_OK;

    /* -------------------------------------------------------- */
    /* Unimplemented / stub opcodes                             */
    /* -------------------------------------------------------- */
    op_unimplemented:
        fprintf(stderr, "vm: unimplemented opcode %d\n", OP(instr));
        return VM_ERROR;

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
    if (IS_OBJ(exception) && AS_OBJ(exception) && AS_OBJ(exception)->type == OBJ_EXCEPTION) {
        ObjException *ex = (ObjException*)AS_OBJ(exception);
        fprintf(stderr, "Uncaught exception: %s\n", ex->message);
    } else {
        char *s = value_to_string(exception);
        fprintf(stderr, "Uncaught exception: %s\n", s);
        free(s);
    }
    return 1;
}
