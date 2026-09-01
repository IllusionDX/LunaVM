
    /* Apply a Luna function's default parameters / keyword remapping after the
       callee frame has been pushed.  kw_names is a List of names for keyword
       calls, or make_null() for positional-only calls (OP_INVOKE / OP_SUPER),
       where only default-argument filling matters.  The MOP early-returns when
       there is nothing to do. */
#define APPLY_DEFAULTS(fn_val, nargs, kw_names) do { \
        Type *_t = (IS_OBJ(fn_val) && AS_OBJ(fn_val)->type) ? AS_OBJ(fn_val)->type : NULL; \
        if (_t && _t->bind_keyword_arguments) { \
            if (!_t->bind_keyword_arguments(vm, (fn_val), (uint8_t)(nargs), (kw_names))) \
                goto op_throw; \
        } \
        /* A default thunk may have realloc'd vm->stack (nested call), leaving the
           loop's cached REG_BASE stale.  Refresh it against the current frame. */ \
        REG_BASE = vm->stack + FRAME.base; \
    } while (0)

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
        Value _L = RKB, _R = RKC;
        if (LUNA_LIKELY(IS_INT(_L) && IS_INT(_R))) {
            int64_t res = (int64_t)AS_INT(_L) + (int64_t)AS_INT(_R);
            if (LUNA_LIKELY(res >= INT32_MIN && res <= INT32_MAX)) {
                SET_REG(RA, INT_VAL((int32_t)res));
                CHECK_GC; DECODE; goto *op_labels[OP(instr)];
            }
        }
        if (LUNA_LIKELY(IS_DOUBLE(_L) && IS_DOUBLE(_R))) {
            SET_REG_PRIM(RA, make_double(AS_DOUBLE(_L) + AS_DOUBLE(_R)));
            CHECK_GC; DECODE; goto *op_labels[OP(instr)];
        }
        Value _v = do_arith(vm, _L, _R, OP_ADD);
        if (IS_NIL(_v)) {
            _exc = vm->last_exception;
            if (IS_NIL(_exc)) { _exc = vm_make_exception(vm, VM_EXCEPTION_TYPE, "unsupported addition"); }
            goto op_throw;
        }
        SET_REG(RA, _v);
        CHECK_GC; DECODE; goto *op_labels[OP(instr)];
    }

    /* 8.  OP_SUB */
    op_sub: {
        Value _L = RKB;
        Value _R = RKC;
        Value _v = do_arith(vm, _L, _R, OP_SUB);
        if (IS_NIL(_v)) {
            _exc = vm->last_exception;
            if (IS_NIL(_exc)) { _exc = vm_make_exception(vm, VM_EXCEPTION_TYPE, "unsupported subtraction"); }
            goto op_throw;
        }
        SET_REG(RA, _v);
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 9.  OP_MUL */
    op_mul: {
        Value _L = RKB, _R = RKC;
        if (LUNA_LIKELY(IS_INT(_L) && IS_INT(_R))) {
            int64_t res = (int64_t)AS_INT(_L) * (int64_t)AS_INT(_R);
            if (LUNA_LIKELY(res >= INT32_MIN && res <= INT32_MAX)) {
                SET_REG(RA, INT_VAL((int32_t)res));
                CHECK_GC; DECODE; goto *op_labels[OP(instr)];
            }
        }
        if (LUNA_LIKELY(IS_DOUBLE(_L) && IS_DOUBLE(_R))) {
            SET_REG_PRIM(RA, make_double(AS_DOUBLE(_L) * AS_DOUBLE(_R)));
            CHECK_GC; DECODE; goto *op_labels[OP(instr)];
        }
        Value _v = do_arith(vm, _L, _R, OP_MUL);
        if (IS_NIL(_v)) { _exc = vm->last_exception; if (IS_NIL(_exc)) _exc = vm_make_exception(vm, VM_EXCEPTION_TYPE, "unsupported multiplication"); goto op_throw; }
        SET_REG(RA, _v);
        CHECK_GC; DECODE; goto *op_labels[OP(instr)];
    }
    
/* 10. OP_DIV */
    op_div: {
        Value _L = RKB, _R = RKC;
        Value _v = do_arith(vm, _L, _R, OP_DIV);
        if (IS_NIL(_v)) { _exc = vm->last_exception; if (IS_NIL(_exc)) _exc = vm_make_exception(vm, VM_EXCEPTION_RUNTIME, "unsupported division"); goto op_throw; }
        SET_REG(RA, _v);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 11. OP_MOD */
    op_mod: {
        Value _L = RKB, _R = RKC;
        Value _v = do_arith(vm, _L, _R, OP_MOD);
        if (IS_NIL(_v)) { _exc = vm->last_exception; if (IS_NIL(_exc)) _exc = vm_make_exception(vm, VM_EXCEPTION_RUNTIME, "unsupported modulo"); goto op_throw; }
        SET_REG(RA, _v);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 12. OP_IDIV */
    op_idiv: {
        Value _L = RKB, _R = RKC;
        Value _v = do_arith(vm, _L, _R, OP_IDIV);
        if (IS_NIL(_v)) { _exc = vm->last_exception; if (IS_NIL(_exc)) _exc = vm_make_exception(vm, VM_EXCEPTION_RUNTIME, "unsupported floor division"); goto op_throw; }
        SET_REG(RA, _v);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 13. OP_POW */
    op_pow: {
        Value _L = RKB, _R = RKC;
        Value _v = do_arith(vm, _L, _R, OP_POW);
        if (IS_NIL(_v)) { _exc = vm->last_exception; if (IS_NIL(_exc)) _exc = vm_make_exception(vm, VM_EXCEPTION_RUNTIME, "unsupported power"); goto op_throw; }
        SET_REG(RA, _v);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 14. OP_NEG */
    op_neg: {
        Value v = REG(RB);
        Value result = make_null();
        if (vm_unary(vm, VM_OP_NEG, v, &result)) {
            SET_REG(RA, result);
            DECODE; goto *op_labels[OP(instr)];
        }
        _exc = vm->last_exception;
        if (IS_NIL(_exc)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "bad operand type for unary -: '%s'", val_type_name(v));
            _exc = vm_make_exception(vm, VM_EXCEPTION_TYPE, buf);
        }
        goto op_throw;
    }

    /* 15. OP_BAND */
    op_band: { Value _v=do_arith(vm,RKB,RKC,OP_BAND); if(IS_NIL(_v)){_exc=vm->last_exception; if(IS_NIL(_exc)) _exc=vm_make_exception(vm,VM_EXCEPTION_TYPE,"unsupported bitwise and");goto op_throw;} SET_REG(RA,_v); DECODE; goto *op_labels[OP(instr)]; }
    /* 16. OP_BOR */
    op_bor:  { Value _v=do_arith(vm,RKB,RKC,OP_BOR); if(IS_NIL(_v)){_exc=vm->last_exception; if(IS_NIL(_exc)) _exc=vm_make_exception(vm,VM_EXCEPTION_TYPE,"unsupported bitwise or");goto op_throw;} SET_REG(RA,_v); DECODE; goto *op_labels[OP(instr)]; }
    /* 17. OP_BXOR */
    op_bxor: { Value _v=do_arith(vm,RKB,RKC,OP_BXOR); if(IS_NIL(_v)){_exc=vm->last_exception; if(IS_NIL(_exc)) _exc=vm_make_exception(vm,VM_EXCEPTION_TYPE,"unsupported bitwise xor");goto op_throw;} SET_REG(RA,_v); DECODE; goto *op_labels[OP(instr)]; }
    /* 18. OP_BNOT */
    op_bnot: { Value _v=make_null(); if(!vm_unary(vm,VM_OP_BNOT,REG(RB),&_v)){_exc=vm->last_exception; if(IS_NIL(_exc)) _exc=vm_make_exception(vm,VM_EXCEPTION_TYPE,"unsupported bitwise not");goto op_throw;} SET_REG(RA,_v); DECODE; goto *op_labels[OP(instr)]; }
    /* 19. OP_SHL */
    op_shl:  { Value _v=do_arith(vm,RKB,RKC,OP_SHL); if(IS_NIL(_v)){_exc=vm->last_exception; if(IS_NIL(_exc)) _exc=vm_make_exception(vm,VM_EXCEPTION_TYPE,"unsupported shift left");goto op_throw;} SET_REG(RA,_v); DECODE; goto *op_labels[OP(instr)]; }
    /* 20. OP_SHR */
    op_shr:  { Value _v=do_arith(vm,RKB,RKC,OP_SHR); if(IS_NIL(_v)){_exc=vm->last_exception; if(IS_NIL(_exc)) _exc=vm_make_exception(vm,VM_EXCEPTION_TYPE,"unsupported shift right");goto op_throw;} SET_REG(RA,_v); DECODE; goto *op_labels[OP(instr)]; }

    /* ---- Integer-immediate arithmetic (ABC: A = B op (int8_t)C) ---- */
    /* 21. OP_ADDI */
    op_addi: {
        Value v = REG(RB);
        if (LUNA_LIKELY(IS_INT(v))) {
            int64_t res = (int64_t)AS_INT(v) + (int8_t)C;
            if (LUNA_LIKELY(res >= INT32_MIN && res <= INT32_MAX)) {
                REG(RA) = INT_VAL((int32_t)res);
                DECODE; goto *op_labels[OP(instr)];
            }
        }
        if (LUNA_LIKELY(IS_DOUBLE(v))) {
            REG(RA) = make_double(AS_DOUBLE(v) + (double)(int8_t)C);
            DECODE; goto *op_labels[OP(instr)];
        }
        Value _v = do_arith(vm, v, make_int((int8_t)C), OP_ADD);
        if (IS_NIL(_v)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "unsupported operand type(s) for +: '%s' and 'int'", val_type_name(v));
            _exc = vm_make_exception(vm, VM_EXCEPTION_TYPE, buf);
            goto op_throw;
        }
        REG(RA) = _v;
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 22. OP_SUBI */
    op_subi: {
        Value v = REG(RB);
        if (LUNA_LIKELY(IS_INT(v))) {
            int64_t res = (int64_t)AS_INT(v) - (int8_t)C;
            if (LUNA_LIKELY(res >= INT32_MIN && res <= INT32_MAX)) {
                REG(RA) = INT_VAL((int32_t)res);
                DECODE; goto *op_labels[OP(instr)];
            }
        }
        if (LUNA_LIKELY(IS_DOUBLE(v))) {
            REG(RA) = make_double(AS_DOUBLE(v) - (double)(int8_t)C);
            DECODE; goto *op_labels[OP(instr)];
        }
        Value _v = do_arith(vm, v, make_int((int8_t)C), OP_SUB);
        if (IS_NIL(_v)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "unsupported operand type(s) for -: '%s' and 'int'", val_type_name(v));
            _exc = vm_make_exception(vm, VM_EXCEPTION_TYPE, buf);
            goto op_throw;
        }
        REG(RA) = _v;
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Comparison (ABC: A = B cmp C, result is bool) ---- */
    /* 23. OP_EQ */
    op_eq: {
        Value _L = RKB, _R = RKC;
        SET_REG_PRIM(RA, do_cmp(vm, _L, _R, OP_EQ));
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 24. OP_NE */
    op_ne: {
        Value _L = RKB, _R = RKC;
        SET_REG_PRIM(RA, do_cmp(vm, _L, _R, OP_NE));
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 75. OP_RAW_EQ / 76. OP_RAW_NE: identity (`is` / `is not`).  No deep
     * comparison, no frontend hook: immediates compare by bits, heap objects
     * by pointer.  Matches CPython identity semantics. */
    op_raw_eq: {
        Value _L = RKB, _R = RKC;
        SET_REG_PRIM(RA, make_bool(_L == _R));
        DECODE; goto *op_labels[OP(instr)];
    }
    op_raw_ne: {
        Value _L = RKB, _R = RKC;
        SET_REG_PRIM(RA, make_bool(_L != _R));
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 25. OP_LT */
    op_lt: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,do_cmp(vm, _L,_R,OP_LT)); DECODE; goto *op_labels[OP(instr)]; }
    /* 26. OP_LE */
    op_le: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,do_cmp(vm, _L,_R,OP_LE)); DECODE; goto *op_labels[OP(instr)]; }
    /* 27. OP_GT */
    op_gt: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,do_cmp(vm, _L,_R,OP_GT)); DECODE; goto *op_labels[OP(instr)]; }
    /* 28. OP_GE */
    op_ge: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,do_cmp(vm, _L,_R,OP_GE)); DECODE; goto *op_labels[OP(instr)]; }
    /* 29. OP_IN */
    op_in: {
        Value needle = RKB;
        Value haystack = RKC;
        bool found = false;
        vm_contains(vm, needle, haystack, &found);
        SET_REG_PRIM(RA, make_bool(found));
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Logical ---- */
    /* 30. OP_NOT */
    op_not: SET_REG_PRIM(RA, make_bool(!is_truthy(REG(RB)))); DECODE; goto *op_labels[OP(instr)];

    /* ---- Control Flow ---- */
    /* 31. OP_JMP */
    op_jmp:
        pc += sBx;
        DECODE; goto *op_labels[OP(instr)];

    /* 32. OP_JZ */
    op_jz:
        if (!is_truthy(REG(RA))) pc += sBx;
        DECODE; goto *op_labels[OP(instr)];

    /* 33. OP_JNZ */
    op_jnz:
        if (is_truthy(REG(RA))) pc += sBx;
        DECODE; goto *op_labels[OP(instr)];

    /* 34. OP_JNIL */
    op_jnil:
        if (IS_NIL(REG(RA))) pc += sBx;
        DECODE; goto *op_labels[OP(instr)];

    /* ---- Compare-and-branch (ABC: A=left, B=right, C=offset 0..511) ---- */
    /* 61. OP_LT_JZ, 62. OP_LE_JZ, 63. OP_GT_JZ,
     * 64. OP_GE_JZ, 65. OP_EQ_JZ, 66. OP_NE_JZ,
     * 67. OP_LT_JZ_IMM, 68. OP_LE_JZ_IMM, 69. OP_GT_JZ_IMM,
     * 70. OP_GE_JZ_IMM, 71. OP_EQ_JZ_IMM, 72. OP_NE_JZ_IMM */

#define DEFINE_CMP_JZ(name, op_sym, mop_op) \
    op_##name##_jz: { \
        Value _L = REG(RA), _R = REG(RB); \
        bool _c; \
        if (LUNA_LIKELY(IS_INT(_L) && IS_INT(_R))) { \
            _c = ((int64_t)AS_INT(_L) op_sym (int64_t)AS_INT(_R)); \
        } else if (_L == _R && !IS_DOUBLE(_L) && (mop_op == OP_EQ || mop_op == OP_NE)) { \
            _c = (mop_op == OP_EQ); \
        } else if (is_num(_L) && is_num(_R)) { \
            _c = (to_f64(_L) op_sym to_f64(_R)); \
        } else { \
            _c = is_truthy(do_cmp(vm, _L, _R, mop_op)); \
        } \
        if (!_c) pc += C; \
        DECODE; goto *op_labels[OP(instr)]; \
    }

#define DEFINE_CMP_JZ_IMM(name, op_sym, mop_op) \
    op_##name##_jz_imm: { \
        Value _L = REG(RA); \
        int64_t _imm = (int8_t)RB; \
        bool _c; \
        if (LUNA_LIKELY(IS_INT(_L))) { \
            _c = ((int64_t)AS_INT(_L) op_sym _imm); \
        } else if (LUNA_LIKELY(IS_DOUBLE(_L))) { \
            _c = (AS_DOUBLE(_L) op_sym (double)_imm); \
        } else { \
            _c = is_truthy(do_cmp(vm, _L, make_int(_imm), mop_op)); \
        } \
        if (!_c) pc += C; \
        DECODE; goto *op_labels[OP(instr)]; \
    }

DEFINE_CMP_JZ(lt, <,  OP_LT)
DEFINE_CMP_JZ(le, <=, OP_LE)
DEFINE_CMP_JZ(gt, >,  OP_GT)
DEFINE_CMP_JZ(ge, >=, OP_GE)
DEFINE_CMP_JZ(eq, ==, OP_EQ)
DEFINE_CMP_JZ(ne, !=, OP_NE)

DEFINE_CMP_JZ_IMM(lt, <,  OP_LT)
DEFINE_CMP_JZ_IMM(le, <=, OP_LE)
DEFINE_CMP_JZ_IMM(gt, >,  OP_GT)
DEFINE_CMP_JZ_IMM(ge, >=, OP_GE)
DEFINE_CMP_JZ_IMM(eq, ==, OP_EQ)
DEFINE_CMP_JZ_IMM(ne, !=, OP_NE)

#undef DEFINE_CMP_JZ
#undef DEFINE_CMP_JZ_IMM

    /* ---- Functions ---- */
    /* 35. OP_CALL */
    op_call: {
        uint8_t ret_reg = RA;
        uint8_t fn_reg  = B;
        uint8_t nargs   = C;
        Value fn_val = REG(fn_reg);
        Type *t = (IS_OBJ(fn_val) && AS_OBJ(fn_val)->type) ? AS_OBJ(fn_val)->type : NULL;

        /* MOP guard: only objects with a `call` vtable are callable. The vtable is
           the single source of truth for "is this invokable?". */
        if (!t || !t->call) {
            _exc = vm_make_exception(vm, VM_EXCEPTION_TYPE, "attempt to call non-function");
            goto op_throw;
        }

        /* Keyword-prefix state, set by the preceding OP_KW_PREFIX (if any) and
           reset immediately so the positional fast path pays only a compare. */
        uint16_t kw_idx   = vm->next_call_kw_idx;
        uint8_t  kw_count = vm->next_call_kw_count;
        /* Capture the kw-names constant now: PUSH_FRAME switches CHUNK to the
           callee, so the index is only valid against the caller's constant pool. */
        Value kw_names = (kw_idx != 0) ? CONST(kw_idx) : make_null();
        vm->next_call_kw_idx   = 0;
        vm->next_call_kw_count = 0;

        /* Bytecode callables expose their chunk via the vtable; native callables
           return NULL and run synchronously through `call`. The core never names a
           concrete Luna callable kind (function / closure / bound_method / instance). */
        Chunk *chunk = t->get_chunk ? t->get_chunk(fn_val) : NULL;
        if (chunk) {
            Value self = (t->get_self ? t->get_self(fn_val) : make_null());
            int base = FRAME.base + fn_reg + 1;
            int argc = nargs;
            int extra = 0;
            if (!is_null(self)) { SET_REG(fn_reg, self); base = FRAME.base + fn_reg; argc = nargs + 1; extra = 1; }
            PUSH_FRAME(AS_OBJ(fn_val), AS_OBJ(fn_val), base, ret_reg, argc, extra);
            /* Frontend-owned mapping: remap keyword args into the callee's
               parameter registers and apply defaults (runs after PUSH_FRAME, on
               the callee frame).  Called for every bytecode call; the MOP
               early-returns when there is nothing to do. */
            APPLY_DEFAULTS(fn_val, nargs, kw_names);
            DECODE; goto *op_labels[OP(instr)];
        }

        /* Native callable: gather ALL args contiguously (positional first,
         * then kwarg values) and dispatch via the vtable with the kw_names
         * list.  The callee's Type decides: kw-aware natives unpack by name,
         * plain natives reject kwargs with a TypeError.  The vtable prepends
         * `self` for bound methods, so we never do it here. */
        Value scratch[256];
        for (int i = 0; i < nargs; i++) scratch[i] = REG(fn_reg + 1 + i);
        Value result;
        /* Dispatch through vm_call_value so a native function that throws surfaces
           as a catchable exception instead of being swallowed. */
        if (vm_call_value_kw(vm, fn_val, scratch, nargs, kw_names, &result) == VM_EXCEPTION) {
            _exc = vm->last_exception;
            goto op_throw;
        }
        SET_REG(ret_reg, result);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 58. OP_KW_PREFIX: record kw state, then fall through directly into OP_CALL. */
    op_kw_prefix: {
        vm->next_call_kw_count = A;
        vm->next_call_kw_idx   = (uint16_t)Bx;
        DECODE; goto op_call;
    }
    /* 36. OP_RET */
    op_ret: {
        uint8_t ret_reg = RA;
        Value retval = REG(ret_reg);
        /* Leaf return: fast path */
        if (LUNA_UNLIKELY(FRAME.leaf_ret_ip != 0)) {
            CallFrame *cf = &FRAME;
            int leaf_rip = cf->leaf_ret_ip;
            Chunk *leaf_rchunk = cf->leaf_ret_chunk;
            int leaf_rbase = cf->leaf_ret_base;
            int leaf_rreg = cf->leaf_ret_reg;
            Object *leaf_rclosure = cf->leaf_ret_closure;
            Object *leaf_rfn     = cf->leaf_ret_fn;
            cf->leaf_ret_ip = 0;
            cf->leaf_ret_chunk = NULL;
            cf->leaf_ret_base = 0;
            cf->leaf_ret_reg = 0;
            cf->leaf_ret_closure = NULL;
            cf->leaf_ret_fn = NULL;
            frame_set_refs(cf, (Object*)leaf_rclosure, (Object*)leaf_rfn);
            FRAME.base = leaf_rbase;
            FRAME.chunk = leaf_rchunk;
            FRAME.ip = leaf_rip;
            SYNC_LOCALS();

            SET_REG(leaf_rreg, retval);
            DECODE; goto *op_labels[OP(instr)];
        }
        if (vm->frame_count <= 1) {
            vm->last_return_value = retval;
            if (LUNA_UNLIKELY(vm->open_upvalues)) close_upvalues(vm, 0);
            for (int i = 0; i < current_chunk->max_registers; i++) {
                REG_BASE[i] = make_null();
            }
            vm->stack_count = 0;
            vm->frame_count = 0;
            return VM_OK;
        }
        CallFrame *callee = &FRAME;
        int old_base = callee->base;
        
        if (LUNA_LIKELY(vm->open_upvalues)) close_upvalues(vm, vm->frame_count);

        vm->frame_count--;
        SYNC_LOCALS();
        SET_REG(callee->ret_reg, retval);
        frame_release_refs(callee);
        vm->stack_count = old_base;
        DECODE;
        goto *op_labels[OP(instr)];
    }

    /* 37. OP_LEAVE */
    op_leave:
        close_upvalues(vm, vm->frame_count);
        DECODE; goto *op_labels[OP(instr)];

    /* 38. OP_CLOSURE */
    op_closure: {
        uint8_t k = B;
        uint8_t dst = RA;
        Value fn_val = CONST(k);
        /* Closure construction is frontend-owned: the core supplies the function
           constant and the current frame; the frontend builds the closure and
           captures upvalues (it owns the closure layout). */
        if (!vm->frontend || !vm->frontend->new_closure) {
            _exc = vm_make_exception(vm, VM_EXCEPTION_GENERIC, "CLOSURE not supported by frontend");
            goto op_throw;
        }
        Object *cl = vm->frontend->new_closure(vm, fn_val);
        if (!cl) {
            _exc = (IS_OBJ(vm->last_exception) && AS_OBJ(vm->last_exception))
                   ? vm->last_exception
                   : vm_make_exception(vm, VM_EXCEPTION_GENERIC, "CLOSURE failed");
            goto op_throw;
        }
        SET_REG(dst, make_obj(cl));
        CHECK_GC;
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Globals ---- */
    /* 39. OP_GETGLOBAL */
    op_getglobal: {
        Value v;
        if (vm_get_global_fast(vm, KSTROBJ(BX), &v)) SET_REG(RA, v);
        else SET_REG_PRIM(RA, make_null());
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 40. OP_SETGLOBAL */
    op_setglobal: {
        vm_set_global(vm, KSTR(BX), REG(RA), false);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Upvalues ---- */
    /* 41. OP_GETUPVAL */
    op_getupval: {
        uint8_t upv_idx = B;
        uint8_t dst = RA;
        if (FRAME.closure && FRAME.closure->type->get_upvalue) {
            SET_REG(dst, FRAME.closure->type->get_upvalue(vm, make_obj(FRAME.closure), upv_idx));
        } else {
            SET_REG_PRIM(dst, make_null());
        }
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 42. OP_SETUPVAL */
    op_setupval: {
        uint8_t upv_idx = B;
        uint8_t src = RA;
        if (FRAME.closure && FRAME.closure->type->set_upvalue) {
            FRAME.closure->type->set_upvalue(vm, make_obj(FRAME.closure), upv_idx, REG(src));
        }
        DECODE; goto *op_labels[OP(instr)];
    }

/* ---- Object Ops ---- */

    /* 43. OP_NEW */
    op_new: {
        Value _inst;
        if (!vm_construct(vm, CONST(BX), &_inst)) {
            _exc = vm->last_exception;
            goto op_throw;
        }
        SET_REG(RA, _inst);
        CHECK_GC;
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 44. OP_NEWDICT */
    op_newdict: {
        Value _dict;
        if (!vm_new_dict(vm, &_dict)) {
            _exc = vm->last_exception;
            goto op_throw;
        }
        SET_REG(RA, _dict);
        CHECK_GC;
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 45. OP_NEWLIST */
    op_newlist: {
        Value _list;
        if (!vm_new_list(vm, (int)Bx, &_list)) {
            _exc = vm->last_exception;
            goto op_throw;
        }
        SET_REG(RA, _list);
        CHECK_GC;
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 46. OP_LISTAPPEND */
    op_listappend: {
        Value lst = REG(RA);
        Value val = REG(RB);
        if (!vm_list_append(vm, lst, val)) {
            _exc = vm->last_exception;
            goto op_throw;
        }
        gc_write_barrier(lst, val);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 47. OP_GETITER */
    op_getiter: {
        Value iter_obj, iter_state;
        if (!vm_iterate(vm, REG(RB), &iter_obj, &iter_state)) {
            _exc = vm->last_exception;
            goto op_throw;
        }
        SET_REG(RA, iter_obj);
        SET_REG(RA + 1, iter_state);
        SET_REG(RA + 2, make_null());
        CHECK_GC;
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 48. OP_FORITER : generic iterator (lists / dicts / strings / iterables) */
    op_foriter: {
        Value _state = REG(RA + 1);
        Value _elem;
        if (vm_iter_next(vm, REG(RA), &_state, &_elem)) {
            SET_REG(RA + 2, _elem);
            SET_REG(RA + 1, _state);
            pc += SBX;
        } else if (!IS_NIL(vm->last_exception)) {
            _exc = vm->last_exception;
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 73. OP_FORLOOP : numeric range loop, e.g. for i in range(a, b, step).
     *   RA   = current index (the loop variable)
     *   RA+1 = limit (exclusive)
     *   RA+2 = step
     *
     * SPECIALISED to int32 + double only. The compiler gatekeeper (see
     * py/compiler.c and luna/compiler.c, the for-range emission site) emits
     * OP_FORLOOP only when start/stop/step are statically known to fit
     * int32 literals. Anything else goes through OP_GETITER + OP_FORITER,
     * which materialises a real list and walks it with full precision.
     *
     * The body is intentionally a 6-instruction tight loop:
     *   1. type-tag test (IS_INT vs IS_DOUBLE)
     *   2. load step
     *   3. compute idx += step
     *   4. load limit
     *   5. store idx
     *   6. conditional PC += SBX
     * No MOP, no IS_* switches on obj types, no do_arith/do_cmp dispatch. */
    op_forloop: {
        Value *r = REG_BASE + RA;
        if (LUNA_LIKELY(IS_INT(r[0]))) {
            int32_t step = AS_INT(r[2]);
            int32_t idx  = AS_INT(r[0]) + step;
            int32_t lim  = AS_INT(r[1]);
            r[0] = make_int(idx);
            if (step > 0 ? (idx < lim) : (idx > lim)) pc += SBX;
        } else if (LUNA_LIKELY(IS_DOUBLE(r[0]))) {
            double step = AS_DOUBLE(r[2]);
            double idx  = AS_DOUBLE(r[0]) + step;
            double lim  = AS_DOUBLE(r[1]);
            r[0] = make_double(idx);
            if (step > 0.0 ? (idx < lim) : (idx > lim)) pc += SBX;
        }
        /* Anything else: defensive no-op. The compiler shouldn't have emitted
         * OP_FORLOOP for non-int32/double bounds; we silently fail to advance
         * so the user doesn't get silent wrong answers. */
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 74. OP_FORPREP : numeric range prepare (companion to OP_FORLOOP).
     *   RA   = current index (the loop variable, already = start)
     *   RA+1 = limit (exclusive)
     *   RA+2 = step
     *
     * SPECIALISED to int32 + double only (same gatekeeper as OP_FORLOOP).
     * If the range is already exhausted up front (or step == 0), PC += sBx
     * to skip the loop. Otherwise fall through into the body (happy path:
     * no branch taken, no seed arithmetic). */
    op_forprep: {
        Value *r = REG_BASE + RA;
        if (LUNA_LIKELY(IS_INT(r[0]) && IS_INT(r[1]) && IS_INT(r[2]))) {
            int32_t step = AS_INT(r[2]);
            int32_t idx  = AS_INT(r[0]);
            int32_t lim  = AS_INT(r[1]);
            bool skip;
            if (LUNA_LIKELY(step > 0))      skip = (idx >= lim);
            else if (LUNA_LIKELY(step < 0)) skip = (idx <= lim);
            else                            skip = true;  /* step == 0 */
            if (LUNA_UNLIKELY(skip)) pc += SBX;
        } else if (LUNA_LIKELY(IS_DOUBLE(r[0]) && IS_DOUBLE(r[1]) && IS_DOUBLE(r[2]))) {
            double step = AS_DOUBLE(r[2]);
            double idx  = AS_DOUBLE(r[0]);
            double lim  = AS_DOUBLE(r[1]);
            bool skip;
            if (LUNA_LIKELY(step > 0.0))      skip = (idx >= lim);
            else if (LUNA_LIKELY(step < 0.0)) skip = (idx <= lim);
            else                              skip = true;
            if (LUNA_UNLIKELY(skip)) pc += SBX;
        }
        /* Defensive: non-int32/double reached here, just fall through. */
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 49. OP_INDEXGET (strict — throws on missing or null obj) */
    op_indexget: {
        /* Semantic indexing belongs to the frontend.  The legacy switch below
         * remains temporarily as a source-compatibility fallback while the
         * remaining opcode extraction is completed. */
        {
            Value _result;
            if (vm_getitem(vm, REG(RB), REG(RC), false, &_result)) {
                SET_REG(RA, _result);
                DECODE; goto *op_labels[OP(instr)];
            }
            _exc = vm->last_exception;
            goto op_throw;
        }
    }
    /* 50. OP_INDEXSET */
    op_indexset: {
        if (vm_setitem(vm, REG(RA), REG(RB), REG(RC))) {
            gc_write_barrier(REG(RA), REG(RC));
            DECODE; goto *op_labels[OP(instr)];
        }
        _exc = vm->last_exception;
        goto op_throw;
        /* Legacy type switch below is unreachable during frontend dispatch. */
    }

    /* 51. OP_MEMBERGET (strict — throws on missing) */
    op_memberget: {
        {
            Value _result;
            Value _name = make_obj((Object *)KSTROBJ(RC));
            if (vm_getattr(vm, REG(RB), _name, false, &_result)) {
                SET_REG(RA, _result);
                DECODE; goto *op_labels[OP(instr)];
            }
        }
        _exc = vm->last_exception;
        goto op_throw;
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 52. OP_MEMBERSET */
    op_memberset: {
        {
            Value _name = make_obj((Object *)KSTROBJ(RC));
            if (vm_setattr(vm, REG(RA), _name, REG(RB))) {
                gc_write_barrier(REG(RA), REG(RB));
                DECODE; goto *op_labels[OP(instr)];
            }
        }
        _exc = vm->last_exception;
        goto op_throw;
    }

    /* 53. OP_GETFIELD (slot-based field get: A = B->fields[C]) */
    op_getfield: {
        Value obj = REG(RB);
        Value _f;
        vm_get_field_slot(vm, obj, (int)RC, &_f);
        SET_REG(RA, _f);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 54. OP_SETFIELD (slot-based field set: A->fields[C] = B) */
    op_setfield: {
        Value obj = REG(RA);
        vm_set_field_slot(vm, obj, (int)RC, REG(RB));
        gc_write_barrier(obj, REG(RB));
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Invocation ---- */
    /* 55. OP_INVOKE */
    op_invoke: {
        uint8_t ret_reg = RA;
        uint8_t obj_reg = B;
        uint8_t nargs   = C;
        Value method_val = REG(obj_reg + 1);
        Value obj = REG(obj_reg);
        /* method_val must be a string constant; the frontend owns the string
           concept, so ask it through the is_string hook (legacy no-op guard). */
        if (vm->frontend && vm->frontend->is_string &&
            !vm->frontend->is_string(vm, method_val)) {
            SET_REG_PRIM(ret_reg, make_null());
            DECODE; goto *op_labels[OP(instr)];
        }

        Value scratch[256];
        for (int i = 0; i < nargs; i++) scratch[i] = REG(obj_reg + 2 + i);

        /* Resolve the callable + self-binding layout via the frontend. */
        Value self_arg, callable;
        if (!vm_invoke(vm, obj, method_val, &self_arg, &callable)) {
            _exc = vm->last_exception;
            goto op_throw;
        }
        if (IS_NIL(callable)) { /* optional _init / no-op */
            SET_REG_PRIM(ret_reg, make_null());
            DECODE; goto *op_labels[OP(instr)];
        }
        Type *t = (IS_OBJ(callable) && AS_OBJ(callable)->type) ? AS_OBJ(callable)->type : NULL;
        if (!t || !t->call) {
            _exc = vm_make_exception(vm, VM_EXCEPTION_TYPE, "attempt to call non-function");
            goto op_throw;
        }
        bool is_method = !IS_NIL(self_arg);

        /* Bytecode callables expose their chunk via the vtable; native callables
           return NULL and run synchronously through `call`. The core never names
           a concrete Luna callable kind (function / closure / bound_method). */
        Chunk *chunk = t->get_chunk ? t->get_chunk(callable) : NULL;
        if (chunk) {
            Value self = is_method ? self_arg
                         : (t->get_self ? t->get_self(callable) : make_null());
            /* The method-name slot (obj_reg+1) doubles as the callee fn slot;
               self lands at callee reg 0 and the args follow at reg 1+ (already
               sitting in the caller's obj_reg+2.. registers, so re-basing maps
               them into place with no copy). */
            uint8_t fn_slot = (uint8_t)(obj_reg + 1);
            int base = FRAME.base + fn_slot + 1;   /* = obj_reg + 2 */
            int argc = nargs;
            int extra = 0;
            if (!is_null(self)) {
                SET_REG(fn_slot, self);
                base = FRAME.base + fn_slot;       /* self at callee reg 0 */
                argc = nargs + 1;
                extra = 1;
            }
            PUSH_FRAME(AS_OBJ(callable), AS_OBJ(callable), base, ret_reg, argc, extra);
            APPLY_DEFAULTS(callable, nargs + (is_method ? 1 : 0), make_null());
            DECODE; goto *op_labels[OP(instr)];
        }

        /* Native callable: prepend the explicit self for methods, else pass args
           as-is.  Dispatch through vm_call_value so a native function that throws
           (via longjmp) surfaces as a catchable exception instead of being
           swallowed — mirroring the old inline `goto op_throw` on native failure. */
        int off = is_method ? 1 : 0;
        Value native_scratch[256];
        if (is_method) native_scratch[0] = self_arg;
        for (int i = 0; i < nargs; i++) native_scratch[off + i] = scratch[i];
        Value result;
        if (vm_call_value(vm, callable, native_scratch, nargs + off, &result) == VM_EXCEPTION) {
            _exc = vm->last_exception;
            goto op_throw;
        }
        SET_REG(ret_reg, result);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 56. OP_SUPER */
    op_super: {
        uint8_t ret_reg = RA;
        uint8_t self_reg = RB;
        uint8_t nargs   = RC;
        Value method_val = REG(ret_reg + 1);
        Value self = REG(self_reg);

        Value scratch[256];
        for (int i = 0; i < nargs; i++) scratch[i] = REG(ret_reg + 2 + i);

        /* The frontend (luna_super) validates method_val/self and no-ops by
           returning a NIL callable, so no inline IS_STRING/IS_INSTANCE guard is
           needed here. */
        Value self_arg, callable;
        if (!vm_super_fn(vm, self, method_val, &self_arg, &callable)) {
            _exc = vm->last_exception;
            goto op_throw;
        }
        if (IS_NIL(callable)) { /* no-op */
            SET_REG_PRIM(ret_reg, make_null());
            DECODE; goto *op_labels[OP(instr)];
        }
        Type *t = (IS_OBJ(callable) && AS_OBJ(callable)->type) ? AS_OBJ(callable)->type : NULL;
        if (!t || !t->call) {
            _exc = vm_make_exception(vm, VM_EXCEPTION_TYPE, "attempt to call non-function");
            goto op_throw;
        }

        /* Bytecode callable: push a real frame (mirrors op_call).  self is always
           present for a super method and lives at callee reg 0. */
        Chunk *chunk = t->get_chunk ? t->get_chunk(callable) : NULL;
        if (chunk) {
            uint8_t fn_slot = (uint8_t)(ret_reg + 1); /* self sits just before the args */
            SET_REG(fn_slot, self);
            int base = FRAME.base + fn_slot;          /* self at callee reg 0 */
            PUSH_FRAME(AS_OBJ(callable), AS_OBJ(callable), base, ret_reg, nargs + 1, 1);
            APPLY_DEFAULTS(callable, nargs + 1, make_null());
            DECODE; goto *op_labels[OP(instr)];
        }

        /* Native super method: self prepended as first argument.  Dispatch through
           vm_call_value so a throwing native surfaces as a catchable exception. */
        Value native_scratch[256];
        native_scratch[0] = self;
        for (int i = 0; i < nargs; i++) native_scratch[i + 1] = scratch[i];
        Value result;
        if (vm_call_value(vm, callable, native_scratch, nargs + 1, &result) == VM_EXCEPTION) {
            _exc = vm->last_exception;
            goto op_throw;
        }
        SET_REG(ret_reg, result);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Exceptions ---- */
    /* 57. OP_THROW
     *
     * Cold path only.  The happy path of every try-block is zero-cost:
     * no opcode, no malloc, no per-frame state.  When a throw fires we
     * consult chunk->exceptions for the current IP and walk the call
     * stack only if no handler is found here.
     */
    op_throw: {
        if (!IS_OBJ(_exc) || !AS_OBJ(_exc)) {
            /* _exc was not pre-set (user OP_THROW or a re-throw). The value
               lives in REG(RA); only fall back to the still-active exception
               when REG(RA) itself is empty (a genuine argument-less re-throw). */
            _exc = REG(RA);
            if (!IS_OBJ(_exc) || !AS_OBJ(_exc)) {
                if (IS_OBJ(vm->last_exception) && AS_OBJ(vm->last_exception))
                    _exc = vm->last_exception;
            }
        }
        /* The frontend owns the object model, so ask it whether _exc is already
           an instance (a proper exception object) before wrapping it. */
        bool _is_inst = (vm->frontend && vm->frontend->is_instance)
                        ? vm->frontend->is_instance(vm, _exc) : false;
        if (!_is_inst) {
            char *msg = value_to_string(_exc);
            _exc = vm_make_exception(vm, VM_EXCEPTION_GENERIC, msg);
            free(msg);
        }
        vm->last_exception = _exc;

        /* Walk frames from innermost outward, looking for a static exception
         * entry whose [start_ip, end_ip) range contains the throwing IP.
         * All try/catch blocks in the same function share one chunk and one
         * exception table, so we search that table from the innermost entry
         * (last-added) backward; the innermost matching try wins.  We only
         * pop a frame when its entire exception table declines to handle
         * the throw, which means the exception has truly escaped that
         * function.
         *
         * CRITICAL: pc always points into callee's chunk after DECODE.
         * For the origin frame (where the error occurred), throw_ip =
         * (pc - chunk->code) - 1.  For every outer frame reached after
         * unwinding, pc is stale (points into the popped child chunk).
         * Those frames' throw_ip must use cf->ip - 1 instead, since
         * PUSH_FRAME stored return address (= next-after-CALL) into
         * cf->ip before calling down.  Subtracting 1 gives the CALL
         * instruction itself — the point where the exception escapes. */
        bool is_origin_frame = true;
        while (vm->frame_count > 0) {
            CallFrame *cf = &vm->frames[vm->frame_count - 1];
            int throw_ip = is_origin_frame
                           ? (int)(pc - cf->chunk->code) - 1
                           : cf->ip - 1;
            is_origin_frame = false;
            ExceptionEntry *match = NULL;
            /* The compiler adds exception entries innermost-first, so a
             * forward linear scan already picks the innermost match. */
            for (int i = 0; i < cf->chunk->exception_count; i++) {
                ExceptionEntry *e = &cf->chunk->exceptions[i];
                if (throw_ip >= e->start_ip && throw_ip < e->end_ip) {
                    match = e;
                    break;
                }
            }
            if (match) {
                cf->ip = match->catch_ip;
                SYNC_LOCALS();
                SET_REG(match->exc_reg, _exc);
                _exc = make_null();
                DECODE;
                goto *op_labels[OP(instr)];
            }
            /* No handler in this function: pop the frame and try the caller. */
            frame_release_refs(cf);
            close_upvalues(vm, vm->frame_count);
            vm->frame_count--;
        }
        return VM_EXCEPTION;
    }

    /* 59. OP_IMPORT */
    op_import: {
        uint8_t dst = RA;
        Value mod_name_val = CONST(BX);
        {
            char _from_dir[1024];
            path_dirname(FRAME.chunk && FRAME.chunk->source_path ? FRAME.chunk->source_path : NULL,
                         _from_dir, sizeof(_from_dir));
            Value _module;
            if (vm_import_module(vm, mod_name_val, _from_dir[0] ? _from_dir : NULL, &_module)) {
                SET_REG(dst, _module);
                DECODE; goto *op_labels[OP(instr)];
            }
            if (!IS_NIL(vm->last_exception)) {
                _exc = vm->last_exception;
                goto op_throw;
            }
        }
    }

    /* 60. OP_HALT */
    op_halt: {
        CallFrame *frame = &FRAME;
        vm->last_return_value = REG(0);
        close_upvalues(vm, 0);
        
        for (int i = 0; i < frame->chunk->max_registers; i++) SET_REG(i, make_null());
        frame_release_refs(frame);
        vm->stack_count = 0;
        vm->frame_count = 0;
        return VM_OK;
    }
