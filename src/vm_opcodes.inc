
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
        if (LUNA_LIKELY(IS_INT(_L) && IS_INT(_R))) {
            int64_t res = (int64_t)AS_INT(_L) + (int64_t)AS_INT(_R);
            if (LUNA_LIKELY(res >= INT32_MIN && res <= INT32_MAX)) {
                REG(RA) = INT_VAL((int32_t)res);
                DECODE; goto *op_labels[OP(instr)];
            }
        }
        Value _v = do_arith(_L, _R, OP_ADD);
        if (IS_NIL(_v) && IS_LIST(_L) && !IS_LIST(_R)) {
            const char *tn = val_type_name(_R);
            char buf[256];
            snprintf(buf, sizeof(buf), "can only concatenate list (not \"%s\") to list", tn);
            _exc = make_exception_instance(vm, vm->type_error_class, buf);
            goto op_throw;
        }
        if (LUNA_UNLIKELY(!IS_NIL(_exc))) goto op_throw;
        SET_REG(RA, _v);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 8.  OP_SUB */
    op_sub: {
        Value _L = RKB;
        Value _R = RKC;
        if (LUNA_LIKELY(IS_INT(_L) && IS_INT(_R))) {
            int64_t res = (int64_t)AS_INT(_L) - (int64_t)AS_INT(_R);
            if (LUNA_LIKELY(res >= INT32_MIN && res <= INT32_MAX)) {
                REG(RA) = INT_VAL((int32_t)res);
                DECODE; goto *op_labels[OP(instr)];
            }
        }
        Value _v = do_arith(_L, _R, OP_SUB);
        if (LUNA_UNLIKELY(!IS_NIL(_exc))) goto op_throw;
        SET_REG(RA, _v);
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 9.  OP_MUL */
    op_mul: { Value _L=RKB,_R=RKC; if(is_int_type(_L)&&is_int_type(_R)){SET_REG(RA, make_int_result(to_i64(_L)*to_i64(_R)));}else if(IS_DOUBLE(_L)&&IS_DOUBLE(_R)){SET_REG_PRIM(RA,make_double(AS_DOUBLE(_L)*AS_DOUBLE(_R)));}else{SET_REG(RA,do_arith(_L,_R,OP_MUL));} CHECK_GC; DECODE; goto *op_labels[OP(instr)]; }

    /* 10. OP_DIV */
    op_div: {
        Value _L = RKB, _R = RKC;
        if (is_int_type(_L) && is_int_type(_R)) {
            int64_t ri = to_i64(_R);
            if (!ri) { _exc = make_exception_instance(vm, vm->runtime_error_class, "division by zero"); goto op_throw; }
            SET_REG(RA, make_int_result(to_i64(_L) / ri));
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
        if (is_int_type(_L) && is_int_type(_R)) {
            int64_t ri = to_i64(_R);
            if (!ri) { _exc = make_exception_instance(vm, vm->exception_class, "mod/0"); goto op_throw; }
            SET_REG(RA, make_int_result(to_i64(_L) % ri));
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
        if (is_int_type(v)) { SET_REG(RA, make_int_result(-to_i64(v))); DECODE; goto *op_labels[OP(instr)]; }
        if (IS_DOUBLE(v)) { SET_REG_PRIM(RA, make_double(-AS_DOUBLE(v))); DECODE; goto *op_labels[OP(instr)]; }
        SET_REG_PRIM(RA, make_null());
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 13. OP_BAND */
    op_band: SET_REG(RA, make_int_result(to_i64(REG(RB)) & to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    /* 14. OP_BOR */
    op_bor:  SET_REG(RA, make_int_result(to_i64(REG(RB)) | to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    /* 15. OP_BXOR */
    op_bxor: SET_REG(RA, make_int_result(to_i64(REG(RB)) ^ to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    /* 16. OP_BNOT */
    op_bnot: SET_REG(RA, make_int_result(~to_i64(REG(RB)))); DECODE; goto *op_labels[OP(instr)];
    /* 17. OP_SHL */
    op_shl:  SET_REG(RA, make_int_result(to_i64(REG(RB)) << to_i64(REG(RC)))); DECODE; goto *op_labels[OP(instr)];
    /* 18. OP_SHR */
    op_shr:  SET_REG(RA, make_int_result(to_i64(REG(RB)) >> to_i64(REG(RC)))); DECODE; goto *op_labels[OP(instr)];

    /* ---- Arithmetic with constant pool operand ---- */
    /* OP_ADDK */
    op_addk: {
        Value _L = REG(RB);
        Value _R = CONST(RC);
        if (is_int_type(_L) && is_int_type(_R)) {
            SET_REG(RA, make_int_result(to_i64(_L) + to_i64(_R)));
        } else if (IS_DOUBLE(_L) && IS_DOUBLE(_R)) {
            SET_REG_PRIM(RA, make_double(AS_DOUBLE(_L) + AS_DOUBLE(_R)));
        } else {
            Value _v = do_arith(_L, _R, OP_ADD);
            if (IS_NIL(_v) && IS_LIST(_L) && !IS_LIST(_R)) {
                const char *tn = val_type_name(_R);
                char buf[256];
                snprintf(buf, sizeof(buf), "can only concatenate list (not \"%s\") to list", tn);
                _exc = make_exception_instance(vm, vm->type_error_class, buf);
                goto op_throw;
            }
            SET_REG(RA, _v);
        }
        CHECK_GC;
        DECODE; goto *op_labels[OP(instr)];
    }
    /* OP_MULK */
    op_mulk: {
        Value _L = REG(RB);
        Value _R = CONST(RC);
        if (is_int_type(_L) && is_int_type(_R)) {
            SET_REG(RA, make_int_result(to_i64(_L) * to_i64(_R)));
        } else if (IS_DOUBLE(_L) && IS_DOUBLE(_R)) {
            SET_REG_PRIM(RA, make_double(AS_DOUBLE(_L) * AS_DOUBLE(_R)));
        } else {
            SET_REG(RA, do_arith(_L, _R, OP_MUL));
        }
        CHECK_GC;
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Integer-immediate arithmetic (ABC: A = A op (int8_t)B) ---- */
    /* 19. OP_ADDI */
    op_addi: {
        Value v = REG(RA);
        if (LUNA_LIKELY(IS_INT(v))) {
            int64_t res = (int64_t)AS_INT(v) + (int8_t)RB;
            if (LUNA_LIKELY(res >= INT32_MIN && res <= INT32_MAX)) {
                REG(RA) = INT_VAL((int32_t)res);
            } else {
                REG(RA) = make_int64(res);
            }
        } else {
            Value _v = do_arith(v, make_int((int8_t)RB), OP_ADD);
            if (IS_NIL(_v) && IS_LIST(v)) {
                char buf[256];
                snprintf(buf, sizeof(buf), "can only concatenate list (not \"int\") to list");
                _exc = make_exception_instance(vm, vm->type_error_class, buf);
                goto op_throw;
            }
            REG(RA) = _v;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 20. OP_SUBI */
    op_subi: {
        Value v = REG(RA);
        if (LUNA_LIKELY(IS_INT(v))) {
            int64_t res = (int64_t)AS_INT(v) - (int8_t)RB;
            if (LUNA_LIKELY(res >= INT32_MIN && res <= INT32_MAX)) {
                REG(RA) = INT_VAL((int32_t)res);
            } else {
                REG(RA) = make_int64(res);
            }
        } else {
            REG(RA) = do_arith(v, make_int((int8_t)RB), OP_SUB);
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* OP_ADDI_FROM */
    op_addi_from: {
        Value v = REG(RB);
        if (is_int_type(v)) {
            Value _r = make_int_result(to_i64(v) + (int8_t)C);
            REG(RA) = _r;
        } else if (IS_DOUBLE(v)) {
            REG(RA) = make_double(AS_DOUBLE(v) + (double)(int8_t)C);
        } else if (IS_LIST(v)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "can only concatenate list (not \"int\") to list");
            _exc = make_exception_instance(vm, vm->type_error_class, buf);
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* OP_SUBI_FROM */
    op_subi_from: {
        Value v = REG(RB);
        if (LUNA_LIKELY(IS_INT(v))) {
            int64_t res = (int64_t)AS_INT(v) - (int8_t)C;
            if (LUNA_LIKELY(res >= INT32_MIN && res <= INT32_MAX)) {
                REG(RA) = INT_VAL((int32_t)res);
                DECODE; goto *op_labels[OP(instr)];
            }
        }
        if (IS_DOUBLE(v)) {
            REG(RA) = make_double(AS_DOUBLE(v) - (double)(int8_t)C);
        } else {
            SET_REG(RA, make_int_result(to_i64(v) - (int8_t)C));
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Comparison (ABC: A = B cmp C, result is bool) ---- */
    /* 21. OP_EQ */
    op_eq: {
        Value _L = RKB, _R = RKC;
        SET_REG_PRIM(RA, (_L == _R) ? TRUE_VAL : (IS_STRING(_L) && IS_STRING(_R)) ? FALSE_VAL : do_cmp(_L, _R, OP_EQ));
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 22. OP_NE */
    op_ne: {
        Value _L = RKB, _R = RKC;
        SET_REG_PRIM(RA, (_L == _R) ? FALSE_VAL : (IS_STRING(_L) && IS_STRING(_R)) ? TRUE_VAL : do_cmp(_L, _R, OP_NE));
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 23. OP_LT */
    op_lt: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,is_int_type(_L)&&is_int_type(_R)?make_bool(to_i64(_L)< to_i64(_R)):do_cmp(_L,_R,OP_LT)); DECODE; goto *op_labels[OP(instr)]; }
    /* 24. OP_LE */
    op_le: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,is_int_type(_L)&&is_int_type(_R)?make_bool(to_i64(_L)<=to_i64(_R)):do_cmp(_L,_R,OP_LE)); DECODE; goto *op_labels[OP(instr)]; }
    /* 25. OP_GT */
    op_gt: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,is_int_type(_L)&&is_int_type(_R)?make_bool(to_i64(_L)> to_i64(_R)):do_cmp(_L,_R,OP_GT)); DECODE; goto *op_labels[OP(instr)]; }
    /* 26. OP_GE */
    op_ge: { Value _L=RKB,_R=RKC; SET_REG_PRIM(RA,is_int_type(_L)&&is_int_type(_R)?make_bool(to_i64(_L)>=to_i64(_R)):do_cmp(_L,_R,OP_GE)); DECODE; goto *op_labels[OP(instr)]; }
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
        pc += sBx;
        DECODE; goto *op_labels[OP(instr)];

    /* 30. OP_JZ */
    op_jz:
        if (!is_truthy(REG(RA))) pc += sBx;
        DECODE; goto *op_labels[OP(instr)];

    /* 31. OP_JNZ */
    op_jnz:
        if (is_truthy(REG(RA))) pc += sBx;
        DECODE; goto *op_labels[OP(instr)];

    /* 32. OP_JNIL */
    op_jnil:
        if (IS_NIL(REG(RA))) pc += sBx;
        DECODE; goto *op_labels[OP(instr)];

    /* ---- Compare-and-branch (ABC: A=left, B=right, C=offset) ---- */
    op_lt_jz: {
        Value _L = REG(RA), _R = REG(RB);
        bool _c;
        if (LUNA_LIKELY(IS_INT(_L) && IS_INT(_R))) {
            _c = AS_INT(_L) < AS_INT(_R);
        } else {
            if (is_num(_L) && is_num(_R)) _c = to_f64(_L) < to_f64(_R);
            else _c = is_truthy(do_cmp(_L, _R, OP_LT));
        }
        if (!_c) pc += (int16_t)C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_le_jz: {
        Value _L = REG(RA), _R = REG(RB);
        bool _c;
        if (LUNA_LIKELY(IS_INT(_L) && IS_INT(_R))) {
            _c = AS_INT(_L) <= AS_INT(_R);
        } else {
            if (is_num(_L) && is_num(_R)) _c = to_f64(_L) <= to_f64(_R);
            else _c = is_truthy(do_cmp(_L, _R, OP_LE));
        }
        if (!_c) pc += C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_gt_jz: {
        Value _L = REG(RA), _R = REG(RB);
        bool _c;
        if (LUNA_LIKELY(IS_INT(_L) && IS_INT(_R))) {
            _c = AS_INT(_L) > AS_INT(_R);
        } else {
            if (is_num(_L) && is_num(_R)) _c = to_f64(_L) > to_f64(_R);
            else _c = is_truthy(do_cmp(_L, _R, OP_GT));
        }
        if (!_c) pc += C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_ge_jz: {
        Value _L = REG(RA), _R = REG(RB);
        bool _c;
        if (LUNA_LIKELY(IS_INT(_L) && IS_INT(_R))) {
            _c = AS_INT(_L) >= AS_INT(_R);
        } else {
            if (is_num(_L) && is_num(_R)) _c = to_f64(_L) >= to_f64(_R);
            else _c = is_truthy(do_cmp(_L, _R, OP_GE));
        }
        if (!_c) pc += C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_eq_jz: {
        Value _L = REG(RA), _R = REG(RB);
        bool _c;
        if (_L == _R) {
            _c = true;
        } else if (IS_STRING(_L) && IS_STRING(_R)) {
            _c = false;
        } else {
            _c = is_truthy(do_cmp(_L, _R, OP_EQ));
        }
        if (!_c) pc += C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_ne_jz: {
        Value _L = REG(RA), _R = REG(RB);
        bool _c;
        if (_L == _R) {
            _c = false;
        } else if (IS_STRING(_L) && IS_STRING(_R)) {
            _c = true;
        } else {
            _c = is_truthy(do_cmp(_L, _R, OP_NE));
        }
        if (!_c) pc += C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_lt_jz_imm: {
        Value _L = REG(RA);
        int64_t _imm = (int8_t)RB;
        bool _c;
        if (LUNA_LIKELY(IS_INT(_L))) {
            _c = (int64_t)AS_INT(_L) < _imm;
        } else if (IS_DOUBLE(_L)) {
            _c = AS_DOUBLE(_L) < (double)_imm;
        } else {
            _c = is_truthy(do_cmp(_L, make_int(_imm), OP_LT));
        }
        if (!_c) pc += (int16_t)C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_le_jz_imm: {
        Value _L = REG(RA);
        int64_t _imm = (int8_t)RB;
        bool _c;
        if (LUNA_LIKELY(IS_INT(_L))) {
            _c = (int64_t)AS_INT(_L) <= _imm;
        } else if (IS_DOUBLE(_L)) {
            _c = AS_DOUBLE(_L) <= (double)_imm;
        } else {
            _c = is_truthy(do_cmp(_L, make_int(_imm), OP_LE));
        }
        if (!_c) pc += C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_gt_jz_imm: {
        Value _L = REG(RA);
        int64_t _imm = (int8_t)RB;
        bool _c;
        if (LUNA_LIKELY(IS_INT(_L))) {
            _c = (int64_t)AS_INT(_L) > _imm;
        } else if (IS_DOUBLE(_L)) {
            _c = AS_DOUBLE(_L) > (double)_imm;
        } else {
            _c = is_truthy(do_cmp(_L, make_int(_imm), OP_GT));
        }
        if (!_c) pc += C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_ge_jz_imm: {
        Value _L = REG(RA);
        int64_t _imm = (int8_t)RB;
        bool _c;
        if (LUNA_LIKELY(IS_INT(_L))) {
            _c = (int64_t)AS_INT(_L) >= _imm;
        } else if (IS_DOUBLE(_L)) {
            _c = AS_DOUBLE(_L) >= (double)_imm;
        } else {
            _c = is_truthy(do_cmp(_L, make_int(_imm), OP_GE));
        }
        if (!_c) pc += C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_eq_jz_imm: {
        Value _L = REG(RA);
        int64_t _imm = (int8_t)RB;
        bool _c;
        if (LUNA_LIKELY(IS_INT(_L))) {
            _c = (int64_t)AS_INT(_L) == _imm;
        } else if (IS_DOUBLE(_L)) {
            _c = AS_DOUBLE(_L) == (double)_imm;
        } else {
            _c = is_truthy(do_cmp(_L, make_int(_imm), OP_EQ));
        }
        if (!_c) pc += C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_ne_jz_imm: {
        Value _L = REG(RA);
        int64_t _imm = (int8_t)RB;
        bool _c;
        if (LUNA_LIKELY(IS_INT(_L))) {
            _c = (int64_t)AS_INT(_L) != _imm;
        } else if (IS_DOUBLE(_L)) {
            _c = AS_DOUBLE(_L) != (double)_imm;
        } else {
            _c = is_truthy(do_cmp(_L, make_int(_imm), OP_NE));
        }
        if (!_c) pc += C;
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Functions ---- */
    /* 33. OP_CALL */
    op_call: {
        uint8_t ret_reg = RA;
        uint8_t fn_reg  = B;
        uint8_t nargs   = C;
        Value fn_val = REG(fn_reg);

        /* Inline cache: monomorphic call-site cache */
        uint32_t ic_idx = ((uint32_t)(uintptr_t)current_chunk ^ (uint32_t)(pc - current_chunk->code)) & (IC_CACHE_SIZE - 1);
        IC_CallEntry *ic = &vm->call_ic[ic_idx];
        if (LUNA_LIKELY(ic->fn_val == fn_val)) {
            ObjFunction *fn = ic->fn;
            if (LUNA_LIKELY(!fn->is_native)) {
                /* Leaf fast path: skip full frame push */
                if (LUNA_LIKELY(fn->is_leaf)) {
                    CallFrame *cf = &FRAME;
                    cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                    cf->leaf_ret_chunk = current_chunk;
                    cf->leaf_ret_base = FRAME.base;
                    cf->leaf_ret_reg = ret_reg;
                    cf->leaf_ret_closure = FRAME.closure;
                    cf->leaf_ret_fn      = FRAME.fn;
                    frame_set_refs(&FRAME, ic->cl, fn);
                    FRAME.base = cf->base + fn_reg + 1;
                    FRAME.nargs = nargs;
                    FRAME.chunk = fn->chunk;
                    FRAME.ip = 0;
                    SYNC_LOCALS();
                    int _needed = FRAME.base + fn->chunk->max_registers;
                    if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                        vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                        vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                        REG_BASE = vm->stack + FRAME.base;
                    }
                    vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                    for (int _i = nargs; _i < fn->chunk->max_registers; _i++)
                        REG_BASE[_i] = make_null();
                    DECODE; goto *op_labels[OP(instr)];
                }
                PUSH_FRAME(fn, ic->cl, FRAME.base + fn_reg + 1, ret_reg, nargs, 0);
                DECODE; goto *op_labels[OP(instr)];
            }
        }

        /* Slow path */
        if (IS_FUNCTION(fn_val)) {
            ObjFunction *fn = (ObjFunction *)AS_OBJ(fn_val);
            if (fn->is_native) {
                Value scratch[256];
                for (int i = 0; i < nargs; i++) scratch[i] = REG(fn_reg + 1 + i);
                Value result;
                if (fn->cfunc) {
                    result = luna_cfunc_dispatch(vm, fn, scratch, nargs);
                } else {
                    if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                }
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            }
            ic->fn_val = fn_val; ic->fn = fn; ic->cl = NULL;
            if (LUNA_LIKELY(fn->is_leaf)) {
                CallFrame *cf = &FRAME;
                cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                cf->leaf_ret_chunk = current_chunk;
                cf->leaf_ret_base = FRAME.base;
                cf->leaf_ret_reg = ret_reg;
                cf->leaf_ret_closure = FRAME.closure;
                cf->leaf_ret_fn      = FRAME.fn;
                frame_set_refs(&FRAME, NULL, fn);
                FRAME.base = cf->base + fn_reg + 1;
                FRAME.nargs = nargs;
                FRAME.chunk = fn->chunk;
                FRAME.ip = 0;
                SYNC_LOCALS();
                int _needed = FRAME.base + fn->chunk->max_registers;
                if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                    vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                    REG_BASE = vm->stack + FRAME.base; // Refresh after realloc
                }
                vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                for (int _i = nargs; _i < fn->chunk->max_registers; _i++)
                    REG_BASE[_i] = make_null();
                DECODE; goto *op_labels[OP(instr)];
            }
            PUSH_FRAME(fn, NULL, FRAME.base + fn_reg + 1, ret_reg, nargs, 0);
            DECODE;
            goto *op_labels[OP(instr)];
        } else if (IS_CLOSURE(fn_val)) {
            ObjClosure *cl = (ObjClosure *)AS_OBJ(fn_val);
            ObjFunction *fn = cl->function;
            if (fn->is_native) {
                Value scratch[256];
                for (int i = 0; i < nargs; i++) scratch[i] = REG(fn_reg + 1 + i);
                Value result;
                if (fn->cfunc) {
                    result = luna_cfunc_dispatch(vm, fn, scratch, nargs);
                } else {
                    if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                }
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            }
            ic->fn_val = fn_val; ic->fn = fn; ic->cl = cl;
            if (LUNA_LIKELY(fn->is_leaf)) {
                CallFrame *cf = &FRAME;
                cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                cf->leaf_ret_chunk = current_chunk;
                cf->leaf_ret_base = FRAME.base;
                cf->leaf_ret_reg = ret_reg;
                cf->leaf_ret_closure = FRAME.closure;
                cf->leaf_ret_fn      = FRAME.fn;
                frame_set_refs(&FRAME, cl, fn);
                FRAME.base = cf->base + fn_reg + 1;
                FRAME.nargs = nargs;
                FRAME.chunk = fn->chunk;
                FRAME.ip = 0;
                SYNC_LOCALS();
                int _needed = FRAME.base + fn->chunk->max_registers;
                if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                    vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                }
                vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                for (int _i = nargs; _i < fn->chunk->max_registers; _i++)
                    REG_BASE[_i] = make_null();
                DECODE; goto *op_labels[OP(instr)];
            }
            PUSH_FRAME(fn, cl, FRAME.base + fn_reg + 1, ret_reg, nargs, 0);
            DECODE;
            goto *op_labels[OP(instr)];
        } else if (IS_BOUND_METHOD(fn_val)) {
            ObjBoundMethod *bm = (ObjBoundMethod *)AS_OBJ(fn_val);
            ObjFunction *fn = bm->fn;
            if (fn->is_native) {
                Value scratch[256];
                scratch[0] = bm->self;
                for (int i = 0; i < nargs; i++) scratch[i + 1] = REG(fn_reg + 1 + i);
                Value result;
                if (fn->cfunc) {
                    result = luna_cfunc_dispatch(vm, fn, scratch, nargs + 1);
                } else {
                    if (!vm_call_native(vm, fn->native_fn, scratch, nargs + 1, &result)) goto op_throw;
                }
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            }
            SET_REG(fn_reg, bm->self);
            if (LUNA_LIKELY(fn->is_leaf)) {
                CallFrame *cf = &FRAME;
                cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                cf->leaf_ret_chunk = current_chunk;
                cf->leaf_ret_base = FRAME.base;
                cf->leaf_ret_reg = ret_reg;
                cf->leaf_ret_closure = FRAME.closure;
                cf->leaf_ret_fn      = FRAME.fn;
                frame_set_refs(&FRAME, NULL, fn);
                FRAME.base = cf->base + fn_reg;
                FRAME.nargs = nargs + 1;
                FRAME.chunk = fn->chunk;
                FRAME.ip = 0;
                SYNC_LOCALS();
                REG_BASE[0] = bm->self; /* Set self */
                int _needed = FRAME.base + fn->chunk->max_registers;
                if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                    vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                }
                vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                for (int _i = nargs + 1; _i < fn->chunk->max_registers; _i++)
                    REG_BASE[_i] = make_null();
                DECODE; goto *op_labels[OP(instr)];
            }
            PUSH_FRAME(fn, NULL, FRAME.base + fn_reg, ret_reg, nargs, 1);
            DECODE;
            goto *op_labels[OP(instr)];
        } else if (IS_INSTANCE(fn_val)) {
            ObjInstance *inst = (ObjInstance *)AS_OBJ(fn_val);
            int mi = -1;
            if (inst->klass) {
                for (int i = 0; i < inst->klass->method_count; i++) {
                    if (strcmp(inst->klass->method_names[i], "_call") == 0) { mi = i; break; }
                }
            }
            if (mi >= 0) {
                ObjFunction *fn = inst->klass->methods[mi];
                if (fn->is_native) {
                    Value scratch[256];
                    scratch[0] = fn_val;
                    for (int i = 0; i < nargs; i++) scratch[i + 1] = REG(fn_reg + 1 + i);
                    Value result;
                    if (fn->cfunc) {
                        result = luna_cfunc_dispatch(vm, fn, scratch, nargs + 1);
                    } else {
                        if (!vm_call_native(vm, fn->native_fn, scratch, nargs + 1, &result)) goto op_throw;
                    }
                    SET_REG(ret_reg, result);
                    DECODE; goto *op_labels[OP(instr)];
                }
                if (LUNA_LIKELY(fn->is_leaf)) {
                    CallFrame *cf = &FRAME;
                    cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                    cf->leaf_ret_chunk = current_chunk;
                    cf->leaf_ret_base = FRAME.base;
                    cf->leaf_ret_reg = ret_reg;
                    cf->leaf_ret_closure = FRAME.closure;
                    cf->leaf_ret_fn      = FRAME.fn;
                    frame_set_refs(&FRAME, NULL, fn);
                    FRAME.base = cf->base + fn_reg;
                    FRAME.nargs = nargs + 1;
                    FRAME.chunk = fn->chunk;
                    FRAME.ip = 0;
                    SYNC_LOCALS();
                    REG_BASE[0] = fn_val; /* Set self */
                    int _needed = FRAME.base + fn->chunk->max_registers;
                    if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                        vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                        vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                    }
                    vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                    for (int _i = nargs + 1; _i < fn->chunk->max_registers; _i++)
                        REG_BASE[_i] = make_null();
                    DECODE; goto *op_labels[OP(instr)];
                }
                PUSH_FRAME(fn, NULL, FRAME.base + fn_reg, ret_reg, nargs, 1);
                DECODE;
                goto *op_labels[OP(instr)];
            } else {
                char buf[256];
                snprintf(buf, sizeof(buf), "attempt to call instance of '%s' (no _call method)", inst->klass ? inst->klass->name : "?");
                
                _exc = make_exception_instance(vm, vm->exception_class, buf);
                goto op_throw;
            }
        } else {
            char *s = value_to_string(fn_val);
            char buf[256];
            if (IS_OBJ(fn_val)) {
                snprintf(buf, sizeof(buf), "attempt to call non-function (type=%d, value=%s)", (int)AS_OBJ(fn_val)->type, s);
            } else {
                snprintf(buf, sizeof(buf), "attempt to call non-function (got %s)", s);
            }
            free(s);
            
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 34. OP_RET */
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
            ObjClosure        *leaf_rclosure = cf->leaf_ret_closure;
            struct ObjFunction *leaf_rfn     = cf->leaf_ret_fn;
            cf->leaf_ret_ip = 0;
            cf->leaf_ret_chunk = NULL;
            cf->leaf_ret_base = 0;
            cf->leaf_ret_reg = 0;
            cf->leaf_ret_closure = NULL;
            cf->leaf_ret_fn = NULL;
            frame_set_refs(cf, leaf_rclosure, leaf_rfn);
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
        
        callee->kw_args = make_null();
        if (LUNA_UNLIKELY(vm->open_upvalues)) close_upvalues(vm, vm->frame_count);

        /* Module import cleanup: capture exports before destroying globals */
        if (callee->saved_globals) {
            char *captured_name = callee->fn && callee->fn->name ? strdup(callee->fn->name) : strdup("<module>");
            
            ObjDict *exports = vm_globals_to_dict(vm);
            vm_globals_restore(vm, callee->saved_globals);
            callee->saved_globals = NULL;
            ObjModule *mod = new_module(captured_name);
            mod->exports = exports;
            Value module_val = make_obj((Object*)mod);
            dict_set(vm->module_cache, make_obj((Object*)new_string(captured_name, (int)strlen(captured_name))), module_val);
            retval = module_val;
            free(captured_name);
        }

        vm_pop_try_frames(vm, vm->frame_count - 1);
        vm->frame_count--;
        SYNC_LOCALS();
        SET_REG(callee->ret_reg, retval);
        frame_release_refs(callee);
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
            
            _exc = make_exception_instance(vm, vm->exception_class, "CLOSURE needs function constant");
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
        }
        SET_REG(dst, make_obj((Object*)cl));
        CHECK_GC;
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
                uv->closed = val;
            }
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Object Ops ---- */
    /*
     * OBJECT CREATION CONVENTIONS
     *
     * Two class flavors:
     *   1. Luna-class (script-defined, e.g. "Player"):
     *      Constructor is `def _init(self, ...):`.  Compiler emits OP_NEW
     *      then OP_INVOKE("_init", args).  OP_INVOKE natively handles
     *      missing _init (returns nil, see strcmp("_init") at ~line 1570).
     *
     *   2. Native class (stdlib, e.g. "net.Socket"):
     *      OP_NEW resolves qualified names by lookup: module->exports[class].
     *      Must export the ObjClass itself, not just a factory function.
     *
     * NIL-RETURN TRAP
     *   Luna _init returns nil if there's no explicit `return self`.
     *   Never set ret_reg = obj_reg — _init's nil will clobber the instance.
     *   Always route _init's return to a scratch register (obj_reg + nargs + 1).
     */

    /* 41. OP_NEW */
    op_new: {
        ObjString *cls_str = KSTROBJ(BX);
        if (!cls_str) { SET_REG_PRIM(RA, make_null()); DECODE; goto *op_labels[OP(instr)]; }
        const char *cls_name = cls_str->chars;
        Value cls_val;
        char *dot = strchr(cls_name, '.');
        if (dot) {
            char mod_name[128];
            int len = (int)(dot - cls_name);
            if (len >= 128) len = 127;
            memcpy(mod_name, cls_name, len);
            mod_name[len] = '\0';
            Value mod_val;
            if (!vm_get_global(vm, mod_name, &mod_val) || !IS_MODULE(mod_val)) {
                
                char buf[256];
                snprintf(buf, sizeof(buf), "module '%s' not found for new", mod_name);
                _exc = make_exception_instance(vm, vm->exception_class, buf);
                goto op_throw;
            }
            ObjModule *mod = (ObjModule*)AS_OBJ(mod_val);
            ObjString *field = new_string(dot + 1, (int)strlen(dot + 1));
            cls_val = dict_get(mod->exports, make_obj((Object*)field));
            if (!IS_CLASS(cls_val)) {
                
                char buf[256];
                snprintf(buf, sizeof(buf), "class '%s' not found in module '%s'", dot + 1, mod_name);
                _exc = make_exception_instance(vm, vm->exception_class, buf);
                goto op_throw;
            }
        } else {
            if (!vm_get_global(vm, cls_name, &cls_val) || !IS_CLASS(cls_val)) {
                
                char buf[256];
                snprintf(buf, sizeof(buf), "class '%s' not found for new", cls_name);
                _exc = make_exception_instance(vm, vm->exception_class, buf);
                goto op_throw;
            }
        }
        ObjClass *cls = (ObjClass*)AS_OBJ(cls_val);
        ObjInstance *new_inst = new_instance(cls, 4);
        SET_REG(RA, make_obj((Object*)new_inst));
        CHECK_GC;
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 42. OP_NEWDICT */
    op_newdict: {
        SET_REG(RA, make_obj((Object*)new_dict()));
        CHECK_GC;
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 43. OP_NEWLIST */
    op_newlist: {
        SET_REG(RA, make_obj((Object*)new_list((int)Bx)));
        CHECK_GC;
        DECODE; goto *op_labels[OP(instr)];
    }
    /* 44. OP_LISTAPPEND */
    op_listappend: {
        Value lst = REG(RA);
        Value val = REG(RB);
        if (IS_LIST(lst)) {
            list_add((ObjList*)AS_OBJ(lst), val);
            gc_write_barrier(lst, val);
        } else {
            
            _exc = make_exception_instance(vm, vm->exception_class, "listappend on non-list");
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
            if (dict->entries == NULL) {
                for (int i = 0; i < dict->entry_count; i++)
                    list_add(keys, dict->inline_entries[i].key);
            } else {
                for (int i = 0; i < dict->order_count; i++) {
                    int idx = dict->order[i];
                    if (dict->entries[idx].key != EMPTY_VAL && dict->entries[idx].key != TOMBSTONE_VAL)
                        list_add(keys, dict->entries[idx].key);
                }
            }
            SET_REG(RA, make_obj((Object*)keys));
        } else {
            SET_REG(RA, iter);
        }
        SET_REG_PRIM(RA + 1, make_int(0));
        SET_REG_PRIM(RA + 2, make_null());
        CHECK_GC;
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 46. OP_FORLOOP */
    op_forloop: {
        Value iter = REG(RA);
        Value state = REG(RA + 1);
        int idx = AS_INT(state);
        bool has_next = false;

        if (LUNA_LIKELY(IS_LIST(iter))) {
            ObjList *lst = (ObjList*)AS_OBJ(iter);
            if (idx < lst->count) {
                SET_REG(RA + 2, lst->items ? lst->items[idx] : lst->inline_items[idx]);
                SET_REG_PRIM(RA + 1, make_int(idx + 1));
                has_next = true;
            }
        } else if (LUNA_LIKELY(IS_STRING(iter))) {
            ObjString *str = (ObjString*)AS_OBJ(iter);
            if (idx < str->length) {
                char buf[2] = {str->chars[idx], '\0'};
                SET_REG(RA + 2, make_obj((Object*)new_string(buf, 1)));
                SET_REG_PRIM(RA + 1, make_int(idx + 1));
                has_next = true;
            }
        } else {
            _exc = make_exception_instance(vm, vm->exception_class, "object is not iterable");
            goto op_throw;
        }

        if (LUNA_LIKELY(has_next)) {
            pc += SBX;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 48. OP_INDEXGET (strict — throws on missing or null obj) */
    op_indexget: {
        Value obj = REG(RB);
        Value key = REG(RC);
        if (!IS_OBJ(obj) || !AS_OBJ(obj)) {
            
                _exc = make_exception_instance(vm, vm->type_error_class, "TypeError: cannot index null value");
            goto op_throw;
        }
        switch (AS_OBJ(obj)->type) {
            case OBJ_LIST: {
                if (!IS_INT(key)) {
                    
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
                    
                    _exc = make_exception_instance(vm, vm->index_error_class, buf);
                    goto op_throw;
                }
                SET_REG(RA, lst->items ? lst->items[idx] : lst->inline_items[idx]);
                break;
            }
            case OBJ_DICT: {
                ObjDict *d = (ObjDict*)AS_OBJ(obj);
                if (!dict_has(d, key)) {
                    char *ks = value_to_string(key);
                    char buf[256];
                    snprintf(buf, sizeof(buf), "KeyError: %s not found in dict", ks);
                    free(ks);
                    
                    _exc = make_exception_instance(vm, vm->key_error_class, buf);
                    goto op_throw;
                }
                SET_REG(RA, dict_get(d, key));
                break;
            }
            case OBJ_STRING: {
                ObjString *s = (ObjString*)AS_OBJ(obj);
                if (!IS_INT(key)) {
                    
                    _exc = make_exception_instance(vm, vm->type_error_class, "TypeError: string index must be integer");
                    goto op_throw;
                }
                int idx = (int)AS_INT(key);
                if (idx < 0) idx += s->length;
                if (idx < 0 || idx >= s->length) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "IndexError: string index out of bounds (index: %d, length: %d)", idx, s->length);
                    
                    _exc = make_exception_instance(vm, vm->index_error_class, buf);
                    goto op_throw;
                }
                SET_REG(RA, make_obj((Object*)new_string(&s->chars[idx], 1)));
                break;
            }
            default: {
                
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
            if (IS_LIST(obj) && IS_INT(key)) {
                list_set((ObjList*)AS_OBJ(obj), AS_INT(key), val);
            } else if (IS_DICT(obj)) {
                dict_set((ObjDict*)AS_OBJ(obj), key, val);
            } else {
                _exc = make_exception_instance(vm, vm->type_error_class, "TypeError: invalid index assignment");
                goto op_throw;
            }
        } else {
            _exc = make_exception_instance(vm, vm->type_error_class, "TypeError: invalid index assignment");
            goto op_throw;
        }
        gc_write_barrier(obj, val);
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
                    }
                } else {
                    for (int i = start; i > stop; i += step) {
                        Value v = lst->items ? lst->items[i] : lst->inline_items[i];
                        list_add(result, v);
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
        CHECK_GC;
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
                    }
                } else {
                    for (int i = start; i > stop; i += step) {
                        Value v = lst->items ? lst->items[i] : lst->inline_items[i];
                        list_add(result, v);
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
        CHECK_GC;
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 51. OP_MEMBERGET (strict — throws on missing) */
    op_memberget: {
        Value obj = REG(RB);
        ObjString *field = KSTROBJ(RC);
        if (!IS_OBJ(obj) || !AS_OBJ(obj)) {
            
                _exc = make_exception_instance(vm, vm->attribute_error_class, "AttributeError: cannot access member of null value");
            goto op_throw;
        }
        switch (AS_OBJ(obj)->type) {
            case OBJ_INSTANCE: {
                ObjInstance *inst = (ObjInstance*)AS_OBJ(obj);
                uint32_t ic_idx = ((uint32_t)(uintptr_t)current_chunk ^ (uint32_t)(pc - current_chunk->code)) & (IC_CACHE_SIZE - 1);
                IC_MemberEntry *ic = &vm->member_ic[ic_idx];
                if (ic->klass == inst->klass && ic->name == field) {
                    SET_REG(RA, inst->fields[ic->index]);
                } else {
                    int fi = -1;
                    if (inst->klass && inst->klass->field_slot_map) {
                        Value slot_v = dict_get(inst->klass->field_slot_map, make_obj((Object*)field));
                        if (IS_INT(slot_v)) fi = AS_INT(slot_v);
                    }
                    if (fi >= 0) {
                        SET_REG(RA, inst->fields[fi]);
                        ic->klass = inst->klass; ic->name = field; ic->index = fi;
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
                            
                            char buf[256];
                            snprintf(buf, sizeof(buf), "AttributeError: instance has no field or method '%s'", field->chars);
                            _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                            goto op_throw;
                        }
                    }
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

                    _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                    goto op_throw;
                }
                break;
            }
            case OBJ_DICT: {
                /* Try method lookup first, then dict_get */
                ObjClass *dklass = vm->dict_class;
                ObjFunction *mfn = dklass ? class_find_method(dklass, field->chars) : NULL;
                if (mfn) {
                    ObjBoundMethod *bm = new_bound_method(obj, mfn);
                    SET_REG(RA, make_obj((Object*)bm));
                } else {
                    Value val = dict_get((ObjDict*)AS_OBJ(obj), make_obj((Object*)field));
                    if (IS_NIL(val)) {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "KeyError: '%s' not found in dict", field->chars);
                        _exc = make_exception_instance(vm, vm->key_error_class, buf);
                        goto op_throw;
                    }
                    SET_REG(RA, val);
                }
                break;
            }
            case OBJ_STRING:
            case OBJ_LIST: {
                /* Unified: try find_method on canonical class */
                ObjClass *klass = get_class(vm, obj);
                ObjFunction *mfn = klass ? class_find_method(klass, field->chars) : NULL;
                if (mfn) {
                    if (mfn->is_native && mfn->param_count == 0) {
                        /* Auto-call 0-arg native methods (like .length) */
                        Value result;
                        Value native_args[1] = { obj };
                        if (!vm_call_native(vm, mfn->native_fn, native_args, 1, &result)) goto op_throw;
                        SET_REG(RA, result);
                    } else {
                        ObjBoundMethod *bm = new_bound_method(obj, mfn);
                        SET_REG(RA, make_obj((Object*)bm));
                    }
                } else {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "AttributeError: %s has no field '%s'",
                             AS_OBJ(obj)->type == OBJ_STRING ? "string" : "list", field->chars);
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
                else if (cls->fields) {
                    Value val = dict_get(cls->fields, make_obj((Object*)field));
                    if (!IS_NIL(val)) {
                        SET_REG(RA, val);
                    } else {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "AttributeError: class '%s' has no field '%s'", cls->name, field->chars);
                        
                        _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                        goto op_throw;
                    }
                } else {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "AttributeError: class '%s' has no field '%s'", cls->name, field->chars);
                    
                    _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                    goto op_throw;
                }
                break;
            }
            case OBJ_VECTOR: {
                ObjVector *vec = (ObjVector*)AS_OBJ(obj);
                if (field->length == 1) {
                    char c = field->chars[0];
                    float v = 0.0f;
                    if (c == 'x') v = vec->data[0];
                    else if (c == 'y') v = vec->data[1];
                    else if (c == 'z') v = vec->data[2];
                    else if (c == 'w') v = vec->data[3];
                    else {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "AttributeError: vector has no field '%s'", field->chars);
                        
                        _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                        goto op_throw;
                    }
                    SET_REG_PRIM(RA, make_double((double)v));
                } else {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "AttributeError: vector has no field '%s'", field->chars);
                    
                    _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                    goto op_throw;
                }
                break;
            }
            default: {
                
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
                if (inst->klass && inst->klass->field_slot_map) {
                    Value slot_v = dict_get(inst->klass->field_slot_map, make_obj((Object*)field));
                    if (IS_INT(slot_v)) fi = AS_INT(slot_v);
                }
                if (fi < 0) {
                    for (int i = 0; i < inst->field_count; i++) {
                    if (strcmp(inst->field_names[i], field->chars) == 0) { fi = i; break; }
                }
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
            case OBJ_MODULE: {
                ObjModule *mod = (ObjModule*)AS_OBJ(obj);
                SET_REG(RA, dict_get(mod->exports, make_obj((Object*)field)));
                break;
            }
            case OBJ_DICT: {
                ObjClass *dklass = vm->dict_class;
                ObjFunction *mfn = dklass ? class_find_method(dklass, field->chars) : NULL;
                if (mfn) {
                    ObjBoundMethod *bm = new_bound_method(obj, mfn);
                    SET_REG(RA, make_obj((Object*)bm));
                } else {
                    SET_REG(RA, dict_get((ObjDict*)AS_OBJ(obj), make_obj((Object*)field)));
                }
                break;
            }
            case OBJ_STRING:
            case OBJ_LIST: {
                ObjClass *klass = get_class(vm, obj);
                ObjFunction *mfn = klass ? class_find_method(klass, field->chars) : NULL;
                if (mfn) {
                    if (mfn->is_native && mfn->param_count == 0) {
                        Value result;
                        Value native_args[1] = { obj };
                        if (!vm_call_native(vm, mfn->native_fn, native_args, 1, &result)) { SET_REG_PRIM(RA, make_null()); break; }
                        SET_REG(RA, result);
                    } else {
                        ObjBoundMethod *bm = new_bound_method(obj, mfn);
                        SET_REG(RA, make_obj((Object*)bm));
                    }
                } else {
                    SET_REG_PRIM(RA, make_null());
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
            uint32_t ic_idx = ((uint32_t)(uintptr_t)CHUNK ^ (uint32_t)IP) & (IC_CACHE_SIZE - 1);
            IC_MemberEntry *ic = &vm->member_ic[ic_idx];
            if (ic->klass == inst->klass && ic->name == field) {
                SET_FIELD(inst, ic->index, REG(RB));
            } else {
                int fi = -1;
                if (inst->klass && inst->klass->field_slot_map) {
                    Value slot_v = dict_get(inst->klass->field_slot_map, make_obj((Object*)field));
                    if (IS_INT(slot_v)) fi = AS_INT(slot_v);
                }
                if (fi < 0) {
                    for (int i = 0; i < inst->field_count; i++) {
                        if (strcmp(inst->field_names[i], field->chars) == 0) { fi = i; break; }
                    }
                }
                if (fi >= 0) {
                    SET_FIELD(inst, fi, REG(RB));
                    ic->klass = inst->klass; ic->name = field; ic->index = fi;
                } else {
                    instance_set_field(inst, field->chars, REG(RB));
                }
            }
        } else if (IS_VECTOR(obj)) {
            ObjVector *vec = (ObjVector*)AS_OBJ(obj);
            float v = (float)value_to_double(REG(RB));
            if (field->length == 1) {
                char c = field->chars[0];
                if (c == 'x') vec->data[0] = v;
                else if (c == 'y') vec->data[1] = v;
                else if (c == 'z') vec->data[2] = v;
                else if (c == 'w') vec->data[3] = v;
                else {
                    
                    _exc = make_exception_instance(vm, vm->attribute_error_class, "AttributeError: vector has no field");
                    goto op_throw;
                }
            } else {
                
                _exc = make_exception_instance(vm, vm->attribute_error_class, "AttributeError: vector has no field");
                goto op_throw;
            }
        } else {
            _exc = make_exception_instance(vm, vm->attribute_error_class, "AttributeError: cannot set field on this type");
            goto op_throw;
        }
        gc_write_barrier(obj, REG(RB));
        DECODE; goto *op_labels[OP(instr)];
    }

    /* OP_GETFIELD (slot-based field get: A = B->fields[C]) */
    op_getfield: {
        Value obj = REG(RB);
        if (IS_INSTANCE(obj)) {
            ObjInstance *inst = (ObjInstance*)AS_OBJ(obj);
            int slot = (int)RC;
            if (slot >= 0 && slot < inst->field_count) {
                SET_REG(RA, inst->fields[slot]);
            } else {
                SET_REG_PRIM(RA, make_null());
            }
        } else {
            SET_REG_PRIM(RA, make_null());
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* OP_SETFIELD (slot-based field set: A->fields[C] = B) */
    op_setfield: {
        Value obj = REG(RA);
        if (IS_INSTANCE(obj)) {
            ObjInstance *inst = (ObjInstance*)AS_OBJ(obj);
            int slot = (int)RC;
            if (slot >= 0 && slot < inst->field_count) {
                SET_FIELD(inst, slot, REG(RB));
            } else if (slot >= inst->field_count) {
                /* slot out of range: expand instance to accommodate */
                while (slot >= inst->field_capacity) {
                    inst->field_capacity = inst->field_capacity < 4 ? 4 : inst->field_capacity * 2;
                    inst->field_names = realloc(inst->field_names, inst->field_capacity * sizeof(char *));
                    inst->fields      = realloc(inst->fields,      inst->field_capacity * sizeof(Value));
                }
                while (inst->field_count <= slot) {
                    inst->field_names[inst->field_count] = NULL;
                    inst->fields[inst->field_count] = make_null();
                    inst->field_count++;
                }
                SET_FIELD(inst, slot, REG(RB));
            }
        }
        gc_write_barrier(obj, REG(RB));
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
        Value native_scratch[256];
        if (IS_OBJ(obj) && AS_OBJ(obj)) {
            switch (AS_OBJ(obj)->type) {
                case OBJ_CLASS: {
                    ObjClass *cls = (ObjClass*)AS_OBJ(obj);
                    ObjString *mname_obj = (ObjString*)AS_OBJ(method_val);
                    Value fn_val = cls->fields ? dict_get(cls->fields, make_obj((Object*)mname_obj)) : make_null();
                    if (IS_FUNCTION(fn_val)) {
                        ObjFunction *fn = (ObjFunction*)AS_OBJ(fn_val);
                        if (fn->is_native) {
                            if (fn->cfunc) {
                                result = luna_cfunc_dispatch(vm, fn, scratch, nargs);
                            } else {
                                if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                            }
                        } else if (vm->frame_count < MAX_FRAMES) {
                            /* Leaf fast path: skip full frame push */
                            if (LUNA_LIKELY(fn->is_leaf)) {
                                CallFrame *cf = &FRAME;
                                cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                                cf->leaf_ret_chunk = current_chunk;
                                cf->leaf_ret_base = FRAME.base;
                                cf->leaf_ret_reg = ret_reg;
                                cf->leaf_ret_closure = FRAME.closure;
                                cf->leaf_ret_fn      = FRAME.fn;
                                frame_set_refs(&FRAME, NULL, fn);
                                FRAME.base = cf->base + obj_reg + 2;
                                FRAME.nargs = nargs;
                                FRAME.chunk = fn->chunk;
                                FRAME.ip = 0;
                                SYNC_LOCALS();
                                int _needed = FRAME.base + fn->chunk->max_registers;
                                if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                                    vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                                    REG_BASE = vm->stack + FRAME.base;
                                }
                                vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                                for (int _i = nargs; _i < fn->chunk->max_registers; _i++)
                                    REG_BASE[_i] = make_null();
                                DECODE; goto *op_labels[OP(instr)];
                            }
                            CallFrame *caller = &FRAME;
                            CallFrame *callee = &vm->frames[vm->frame_count];
                            callee->chunk = fn->chunk;
                            callee->ip = 0;
                            callee->base = caller->base + obj_reg + 2;
                            callee->ret_reg = ret_reg;
                            callee->nargs = nargs;
                            callee->kw_args = make_null();
                            frame_set_refs(callee, NULL, fn);
                            callee->saved_globals = NULL;
                            int need = callee->base + fn->chunk->max_registers;
                            if (need > vm->stack_cap) {
                                vm->stack_cap = need < 64 ? 64 : need * 2;
                                vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                            }
                            for (int j = nargs; j < fn->chunk->max_registers; j++)
                                vm->stack[callee->base + j] = make_null();
                            vm->stack_count = need > vm->stack_count ? need : vm->stack_count;
                            caller->ip = (int)(pc - current_chunk->code);
                            vm->frame_count++;
                            SYNC_LOCALS();
                            DECODE;
                            goto *op_labels[OP(instr)];
                        }
                        handled = true;
                    } else if (IS_CLOSURE(fn_val)) {
                        ObjClosure *cl = (ObjClosure*)AS_OBJ(fn_val);
                        ObjFunction *fn = cl->function;
                        if (fn->is_native) {
                            if (fn->cfunc) {
                                result = luna_cfunc_dispatch(vm, fn, scratch, nargs);
                            } else {
                                if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                            }
                        } else if (vm->frame_count < MAX_FRAMES) {
                            /* Leaf fast path: skip full frame push */
                            if (LUNA_LIKELY(fn->is_leaf)) {
                                CallFrame *cf = &FRAME;
                                cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                                cf->leaf_ret_chunk = current_chunk;
                                cf->leaf_ret_base = FRAME.base;
                                cf->leaf_ret_reg = ret_reg;
                                cf->leaf_ret_closure = FRAME.closure;
                                cf->leaf_ret_fn      = FRAME.fn;
                                frame_set_refs(&FRAME, cl, fn);
                                FRAME.base = cf->base + obj_reg + 2;
                                FRAME.nargs = nargs;
                                FRAME.chunk = fn->chunk;
                                FRAME.ip = 0;
                                SYNC_LOCALS();
                                int _needed = FRAME.base + fn->chunk->max_registers;
                                if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                                    vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                                    REG_BASE = vm->stack + FRAME.base;
                                }
                                vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                                for (int _i = nargs; _i < fn->chunk->max_registers; _i++)
                                    REG_BASE[_i] = make_null();
                                DECODE; goto *op_labels[OP(instr)];
                            }
                            CallFrame *caller = &FRAME;
                            CallFrame *callee = &vm->frames[vm->frame_count];
                            callee->chunk = fn->chunk;
                            callee->ip = 0;
                            callee->base = caller->base + obj_reg + 2;
                            callee->ret_reg = ret_reg;
                            callee->nargs = nargs;
                            callee->kw_args = make_null();
                            frame_set_refs(callee, cl, fn);
                            callee->saved_globals = NULL;
                            int need = callee->base + fn->chunk->max_registers;
                            if (need > vm->stack_cap) {
                                vm->stack_cap = need < 64 ? 64 : need * 2;
                                vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                            }
                            for (int j = nargs; j < fn->chunk->max_registers; j++)
                                vm->stack[callee->base + j] = make_null();
                            vm->stack_count = need > vm->stack_count ? need : vm->stack_count;
                            caller->ip = (int)(pc - current_chunk->code);
                            vm->frame_count++;
                            SYNC_LOCALS();
                            DECODE;
                            goto *op_labels[OP(instr)];
                        }
                        handled = true;
                    }
                    if (!handled) {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "AttributeError: class '%s' has no method '%s'", cls->name, mname_obj->chars);
                        
                        _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                        goto op_throw;
                    }
                    break;
                }
case OBJ_MODULE: {
                    ObjModule *mod = (ObjModule*)AS_OBJ(obj);
                    Value fn_val = dict_get(mod->exports, make_obj((Object*)((ObjString*)AS_OBJ(method_val))));
                    if (IS_FUNCTION(fn_val)) {
                        ObjFunction *fn = (ObjFunction*)AS_OBJ(fn_val);
                        if (fn->is_native) {
                            if (fn->cfunc) {
                                result = luna_cfunc_dispatch(vm, fn, scratch, nargs);
                            } else {
                                if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                            }
                        } else if (vm->frame_count < MAX_FRAMES) {
                            /* Leaf fast path: skip full frame push */
                            if (LUNA_LIKELY(fn->is_leaf)) {
                                CallFrame *cf = &FRAME;
                                cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                                cf->leaf_ret_chunk = current_chunk;
                                cf->leaf_ret_base = FRAME.base;
                                cf->leaf_ret_reg = ret_reg;
                                cf->leaf_ret_closure = FRAME.closure;
                                cf->leaf_ret_fn      = FRAME.fn;
                                frame_set_refs(&FRAME, NULL, fn);
                                FRAME.base = cf->base + obj_reg + 2;
                                FRAME.nargs = nargs;
                                FRAME.chunk = fn->chunk;
                                FRAME.ip = 0;
                                SYNC_LOCALS();
                                int _needed = FRAME.base + fn->chunk->max_registers;
                                if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                                    vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                                    REG_BASE = vm->stack + FRAME.base;
                                }
                                vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                                for (int _i = nargs; _i < fn->chunk->max_registers; _i++)
                                    REG_BASE[_i] = make_null();
                                DECODE; goto *op_labels[OP(instr)];
                            }
                            CallFrame *caller = &FRAME;
                            CallFrame *callee = &vm->frames[vm->frame_count];
                            callee->chunk = fn->chunk;
                            callee->ip = 0;
                            callee->base = caller->base + obj_reg + 2;
                            callee->ret_reg = ret_reg;
                            callee->nargs = nargs;
                            callee->kw_args = make_null();
                            frame_set_refs(callee, NULL, fn);
                            callee->saved_globals = NULL;
                            int need = callee->base + fn->chunk->max_registers;
                            if (need > vm->stack_cap) {
                                vm->stack_cap = need < 64 ? 64 : need * 2;
                                vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                            }
                            for (int j = nargs; j < fn->chunk->max_registers; j++)
                                vm->stack[callee->base + j] = make_null();
                            vm->stack_count = need > vm->stack_count ? need : vm->stack_count;
                            caller->ip = (int)(pc - current_chunk->code);
                            vm->frame_count++;
                            SYNC_LOCALS();
                            DECODE;
                            goto *op_labels[OP(instr)];
                        }
                        handled = true;
                    } else if (IS_CLOSURE(fn_val)) {
                        ObjClosure *cl = (ObjClosure*)AS_OBJ(fn_val);
                        ObjFunction *fn = cl->function;
                        if (fn->is_native) {
                            if (fn->cfunc) {
                                result = luna_cfunc_dispatch(vm, fn, scratch, nargs);
                            } else {
                                if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                            }
                        } else if (vm->frame_count < MAX_FRAMES) {
                            /* Leaf fast path: skip full frame push */
                            if (LUNA_LIKELY(fn->is_leaf)) {
                                CallFrame *cf = &FRAME;
                                cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                                cf->leaf_ret_chunk = current_chunk;
                                cf->leaf_ret_base = FRAME.base;
                                cf->leaf_ret_reg = ret_reg;
                                cf->leaf_ret_closure = FRAME.closure;
                                cf->leaf_ret_fn      = FRAME.fn;
                                frame_set_refs(&FRAME, cl, fn);
                                FRAME.base = cf->base + obj_reg + 2;
                                FRAME.nargs = nargs;
                                FRAME.chunk = fn->chunk;
                                FRAME.ip = 0;
                                SYNC_LOCALS();
                                int _needed = FRAME.base + fn->chunk->max_registers;
                                if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                                    vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                                    REG_BASE = vm->stack + FRAME.base;
                                }
                                vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                                for (int _i = nargs; _i < fn->chunk->max_registers; _i++)
                                    REG_BASE[_i] = make_null();
                                DECODE; goto *op_labels[OP(instr)];
                            }
                            CallFrame *caller = &FRAME;
                            CallFrame *callee = &vm->frames[vm->frame_count];
                            callee->chunk = fn->chunk;
                            callee->ip = 0;
                            callee->base = caller->base + obj_reg + 2;
                            callee->ret_reg = ret_reg;
                            callee->nargs = nargs;
                            callee->kw_args = make_null();
                            frame_set_refs(callee, cl, fn);
                            callee->saved_globals = NULL;
                            int need = callee->base + fn->chunk->max_registers;
                            if (need > vm->stack_cap) {
                                vm->stack_cap = need < 64 ? 64 : need * 2;
                                vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                            }
                            for (int j = nargs; j < fn->chunk->max_registers; j++)
                                vm->stack[callee->base + j] = make_null();
                            vm->stack_count = need > vm->stack_count ? need : vm->stack_count;
                            caller->ip = (int)(pc - current_chunk->code);
                            vm->frame_count++;
                            SYNC_LOCALS();
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
                    ObjClass *k = obj_inst->klass;
                    if (k) {
                        ObjFunction *method = NULL;
                        uint32_t ic_idx = ((uint32_t)(uintptr_t)current_chunk ^ (uint32_t)(pc - current_chunk->code)) & (IC_CACHE_SIZE - 1);
                        IC_InvokeEntry *ic = &vm->invoke_ic[ic_idx];
                        if (ic->klass == k && ic->name == mname_obj) {
                            method = ic->method;
                        } else {
                            for (int i = k->method_count - 1; i >= 0; i--) {
                                ObjFunction *m = k->methods[i];
                                if (m && strcmp(k->method_names[i], mname_obj->chars) == 0) { method = m; break; }
                            }
                            if (method) { ic->klass = k; ic->name = mname_obj; ic->method = method; }
                        }
                        if (method) {
                            SET_REG(obj_reg + 1, obj);
                            if (method->is_native) {
                                Value native_scratch[256];
                                native_scratch[0] = obj;
                                for (int i = 0; i < nargs; i++) native_scratch[i + 1] = scratch[i];
                                if (!vm_call_native(vm, method->native_fn, native_scratch, nargs + 1, &result)) goto op_throw;
                            } else {
                                /* Leaf fast path: skip full frame push */
                                if (LUNA_LIKELY(method->is_leaf)) {
                                    CallFrame *cf = &FRAME;
                                    cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                                    cf->leaf_ret_chunk = current_chunk;
                                    cf->leaf_ret_base = FRAME.base;
                                    cf->leaf_ret_reg = ret_reg;
                                    cf->leaf_ret_closure = FRAME.closure;
                                    cf->leaf_ret_fn      = FRAME.fn;
                                    frame_set_refs(&FRAME, NULL, method);
                                    FRAME.base = cf->base + obj_reg + 1;
                                    FRAME.nargs = nargs + 1;
                                    FRAME.chunk = method->chunk;
                                    FRAME.ip = 0;
                                    SYNC_LOCALS();
                                    REG_BASE[0] = obj; /* Set self */
                                    int _needed = FRAME.base + method->chunk->max_registers;
                                    if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                                        vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                                        vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                                        REG_BASE = vm->stack + FRAME.base;
                                    }
                                    vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                                    for (int _i = 1 + nargs; _i < method->chunk->max_registers; _i++)
                                        REG_BASE[_i] = make_null();
                                    DECODE; goto *op_labels[OP(instr)];
                                }
                                if (vm->frame_count < MAX_FRAMES) {
                                    CallFrame *caller = &FRAME;
                                    CallFrame *callee = &vm->frames[vm->frame_count];
                                    callee->chunk = method->chunk;
                                    callee->ip = 0;
                                    callee->base = caller->base + obj_reg + 1;
                                    callee->ret_reg = ret_reg;
                                    callee->nargs = nargs + 1;
                                    callee->kw_args = make_null();
                                    frame_set_refs(callee, NULL, method);
                                    callee->saved_globals = NULL;
                                    int need = callee->base + method->chunk->max_registers;
                                    if (need > vm->stack_cap) {
                                        vm->stack_cap = need < 64 ? 64 : need * 2;
                                        vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                                    }
                                    vm->stack[callee->base] = obj;
                                    for (int j = 1 + nargs; j < method->chunk->max_registers; j++)
                                        vm->stack[callee->base + j] = make_null();
                                    vm->stack_count = need > vm->stack_count ? need : vm->stack_count;
                                    caller->ip = (int)(pc - current_chunk->code);
                                    vm->frame_count++;
                                    SYNC_LOCALS();
                                    DECODE;
                                    goto *op_labels[OP(instr)];
                                }
                            }
                            handled = true;
                        } else if (strcmp(mname, "_init") == 0) {
                            result = make_null();
                            handled = true;
                        }
                    }
                    break;
                }
                /* All other types — use unified get_class + find_method with inline cache */
                default: {
                    ObjClass *klass = get_class(vm, obj);
                    ObjFunction *method = NULL;
                    if (klass) {
                        ObjString *mname_obj = (ObjString*)AS_OBJ(method_val);
                        uint32_t ic_h = ((uint32_t)(uintptr_t)klass ^ mname_obj->hash);
                        int ic_idx = ic_h & (IC_CACHE_SIZE - 1);
                        IC_InvokeEntry *ic = &vm->invoke_ic[ic_idx];
                        if (ic->klass == klass && ic->name == mname_obj) {
                            method = ic->method;
                        } else {
                            method = class_find_method(klass, mname);
                            if (method) { ic->klass = klass; ic->name = mname_obj; ic->method = method; }
                        }
                    }
                    if (method) {
                        if (method->is_native) {
                            native_scratch[0] = obj;
                            for (int i = 0; i < nargs; i++) native_scratch[i + 1] = scratch[i];
                            if (!vm_call_native(vm, method->native_fn, native_scratch, nargs + 1, &result)) goto op_throw;
                        } else {
                            /* Leaf fast path: skip full frame push */
                            if (LUNA_LIKELY(method->is_leaf)) {
                                CallFrame *cf = &FRAME;
                                cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                                cf->leaf_ret_chunk = current_chunk;
                                cf->leaf_ret_base = FRAME.base;
                                cf->leaf_ret_reg = ret_reg;
                                cf->leaf_ret_closure = FRAME.closure;
                                cf->leaf_ret_fn      = FRAME.fn;
                                frame_set_refs(&FRAME, NULL, method);
                                FRAME.base = cf->base + obj_reg + 1;
                                FRAME.nargs = nargs + 1;
                                FRAME.chunk = method->chunk;
                                FRAME.ip = 0;
                                SYNC_LOCALS();
                                REG_BASE[0] = obj; /* self at reg 0 */
                                int _needed = FRAME.base + method->chunk->max_registers;
                                if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                                    vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                                    REG_BASE = vm->stack + FRAME.base;
                                }
                                vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                                for (int _i = 1 + nargs; _i < method->chunk->max_registers; _i++)
                                    REG_BASE[_i] = make_null();
                                DECODE; goto *op_labels[OP(instr)];
                            }
                            if (vm->frame_count < MAX_FRAMES) {
                                CallFrame *caller = &FRAME;
                                CallFrame *callee = &vm->frames[vm->frame_count];
                                callee->chunk = method->chunk;
                                callee->ip = 0;
                                callee->base = caller->base + obj_reg + 1;
                                callee->ret_reg = ret_reg;
                                callee->nargs = nargs + 1;
                                callee->kw_args = make_null();
                                frame_set_refs(callee, NULL, method);
                                callee->saved_globals = NULL;
                                int need = callee->base + method->chunk->max_registers;
                                if (need > vm->stack_cap) {
                                    vm->stack_cap = need < 64 ? 64 : need * 2;
                                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                                }
                                vm->stack[callee->base] = obj; /* self at reg 0 */
                                for (int j = 1 + nargs; j < method->chunk->max_registers; j++)
                                    vm->stack[callee->base + j] = make_null();
                                vm->stack_count = need > vm->stack_count ? need : vm->stack_count;
                                caller->ip = (int)(pc - current_chunk->code);
                                vm->frame_count++;
                                SYNC_LOCALS();
                                DECODE;
                                goto *op_labels[OP(instr)];
                            }
                        }
                        handled = true;
                    } else if (klass && strcmp(mname, "_init") == 0) {
                        /* Constructors are optional: allow new Class() when no _init exists. */
                        result = make_null();
                        handled = true;
                    }
                    break;
                }
            }
        }
        if (handled) SET_REG(ret_reg, result);
        else {
            char buf[256]; snprintf(buf, sizeof(buf), "unknown method '%s'", mname);
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
                            Value native_scratch[256];
                            native_scratch[0] = self;
                            for (int j = 0; j < nargs; j++)
                                native_scratch[j + 1] = REG(ret_reg + 2 + j);
                            if (!vm_call_native(vm, m->native_fn, native_scratch, nargs + 1, &result)) goto op_throw;
                        } else {
                            /* Leaf fast path: skip full frame push */
                            if (LUNA_LIKELY(m->is_leaf)) {
                                CallFrame *cf = &FRAME;
                                cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                                cf->leaf_ret_chunk = current_chunk;
                                cf->leaf_ret_base = FRAME.base;
                                cf->leaf_ret_reg = ret_reg;
                                cf->leaf_ret_closure = FRAME.closure;
                                cf->leaf_ret_fn      = FRAME.fn;
                                frame_set_refs(&FRAME, NULL, m);
                                FRAME.base = cf->base + self_reg + 1;
                                FRAME.nargs = nargs + 1;
                                FRAME.chunk = m->chunk;
                                FRAME.ip = 0;
                                SYNC_LOCALS();
                                REG_BASE[0] = self;
                                int _needed = FRAME.base + m->chunk->max_registers;
                                if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                                    vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                                    REG_BASE = vm->stack + FRAME.base;
                                }
                                vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                                for (int _i = 1 + nargs; _i < m->chunk->max_registers; _i++)
                                    REG_BASE[_i] = make_null();
                                DECODE; goto *op_labels[OP(instr)];
                            }
                        if (vm->frame_count < MAX_FRAMES) {
                            CallFrame *caller = &FRAME;
                            CallFrame *callee = &vm->frames[vm->frame_count];
                            callee->chunk = m->chunk;
                            callee->ip = 0;
                            callee->base = caller->base + self_reg + 1;
                            callee->ret_reg = ret_reg;
                            callee->nargs = nargs + 1;
                            callee->kw_args = make_null();
                            frame_set_refs(callee, NULL, m);
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
                            caller->ip = (int)(pc - current_chunk->code);
                            vm->frame_count++;
                            SYNC_LOCALS();
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
            snprintf(buf, sizeof(buf), "unknown super method '%s'", mname);
            
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Exceptions ---- */
    /* 54. OP_THROW */
    op_throw: {
        if (!IS_OBJ(_exc) || !AS_OBJ(_exc)) {
            if (IS_OBJ(vm->last_exception) && AS_OBJ(vm->last_exception)) {
                _exc = vm->last_exception;
            } else {
                _exc = REG(RA);
            }
        }
        if (!IS_INSTANCE(_exc)) {
            char *msg = value_to_string(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, msg);
            free(msg);
        }
        vm->last_exception = _exc;
        /* unwind to the nearest catch */
        while (vm->try_stack) {
            TryFrame *tf = vm->try_stack;
            if (tf->frame_depth <= vm->frame_count) {
                /* unwind call frames */
                while (vm->frame_count > tf->frame_depth) {
                    CallFrame *unwound = &vm->frames[vm->frame_count - 1];
                    
                    unwound->kw_args = make_null();
                    frame_release_refs(unwound);
                    close_upvalues(vm, vm->frame_count);
                    vm->frame_count--;
                }
                vm->stack_count = tf->stack_count;
                SET_REG(tf->exc_reg, _exc);
                vm->frames[vm->frame_count - 1].ip = tf->catch_ip;
                vm->try_stack = tf->next;
                free(tf);
                _exc = make_null();
                SYNC_LOCALS();
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
        tf->catch_ip   = (int)(pc - current_chunk->code) + sBx;
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
            pc += skip;
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
                Value result;
                if (fn->cfunc) {
                    result = luna_cfunc_dispatch(vm, fn, scratch, nargs);
                } else {
                    if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                }
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            } else {
                /* Leaf fast path for kcall */
                if (LUNA_LIKELY(fn->is_leaf)) {
                    CallFrame *cf = &FRAME;
                    cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                    cf->leaf_ret_chunk = current_chunk;
                    cf->leaf_ret_base = FRAME.base;
                    cf->leaf_ret_reg = ret_reg;
                    cf->leaf_ret_closure = FRAME.closure;
                    cf->leaf_ret_fn      = FRAME.fn;
                    frame_set_refs(&FRAME, NULL, fn);
                    FRAME.base = cf->base + fn_reg + 1;
                    FRAME.nargs = nargs;
                    FRAME.kw_args = kw_args_val;
                    FRAME.chunk = fn->chunk;
                    FRAME.ip = 0;
                    SYNC_LOCALS();
                    int _needed = FRAME.base + fn->chunk->max_registers;
                    if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                        vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                        vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                        REG_BASE = vm->stack + FRAME.base;
                    }
                    vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                    for (int _i = nargs; _i < fn->chunk->max_registers; _i++)
                        REG_BASE[_i] = make_null();
                    DECODE; goto *op_labels[OP(instr)];
                }
                if (vm->frame_count >= MAX_FRAMES) {
                    
                    _exc = make_exception_instance(vm, vm->exception_class, "call stack overflow");
                    goto op_throw;
                }
                CallFrame *caller = &FRAME;
                CallFrame *callee = &vm->frames[vm->frame_count];
                callee->chunk = fn->chunk;
                callee->ip = 0;
                callee->base = caller->base + fn_reg + 1;
                callee->ret_reg = ret_reg;
                callee->nargs = nargs;
                callee->kw_args = kw_args_val;
                
                frame_set_refs(callee, NULL, fn);
                callee->saved_globals = NULL;
                int needed = callee->base + fn->chunk->max_registers;
                if (needed > vm->stack_cap) {
                    vm->stack_cap = needed < 64 ? 64 : needed * 2;
                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                }
                for (int i = nargs; i < fn->chunk->max_registers; i++)
                    vm->stack[callee->base + i] = make_null();
                vm->stack_count = needed > vm->stack_count ? needed : vm->stack_count;
                caller->ip = (int)(pc - current_chunk->code);
                vm->frame_count++;
                SYNC_LOCALS();
                DECODE;
                goto *op_labels[OP(instr)];
            }
        } else if (IS_CLOSURE(fn_val)) {
            ObjClosure *cl = (ObjClosure *)AS_OBJ(fn_val);
            ObjFunction *fn = cl->function;
            if (fn->is_native) {
                Value scratch[256];
                for (int i = 0; i < nargs; i++) scratch[i] = REG(fn_reg + 1 + i);
                Value result;
                if (fn->cfunc) {
                    result = luna_cfunc_dispatch(vm, fn, scratch, nargs);
                } else {
                    if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                }
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            } else {
                /* Leaf fast path for kcall closure */
                if (LUNA_LIKELY(fn->is_leaf)) {
                    CallFrame *cf = &FRAME;
                    cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                    cf->leaf_ret_chunk = current_chunk;
                    cf->leaf_ret_base = FRAME.base;
                    cf->leaf_ret_reg = ret_reg;
                    cf->leaf_ret_closure = FRAME.closure;
                    cf->leaf_ret_fn      = FRAME.fn;
                    frame_set_refs(&FRAME, cl, fn);
                    FRAME.base = cf->base + fn_reg + 1;
                    FRAME.nargs = nargs;
                    FRAME.kw_args = kw_args_val;
                    FRAME.chunk = fn->chunk;
                    FRAME.ip = 0;
                    SYNC_LOCALS();
                    int _needed = FRAME.base + fn->chunk->max_registers;
                    if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                        vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                        vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                        REG_BASE = vm->stack + FRAME.base;
                    }
                    vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                    for (int _i = nargs; _i < fn->chunk->max_registers; _i++)
                        REG_BASE[_i] = make_null();
                    DECODE; goto *op_labels[OP(instr)];
                }
                if (vm->frame_count >= MAX_FRAMES) {
                    
                    _exc = make_exception_instance(vm, vm->exception_class, "call stack overflow");
                    goto op_throw;
                }
                CallFrame *caller = &FRAME;
                CallFrame *callee = &vm->frames[vm->frame_count];
                callee->chunk = fn->chunk;
                callee->ip = 0;
                callee->base = caller->base + fn_reg + 1;
                callee->ret_reg = ret_reg;
                callee->nargs = nargs;
                callee->kw_args = kw_args_val;
                
                frame_set_refs(callee, cl, fn);
                callee->saved_globals = NULL;
                int needed = callee->base + fn->chunk->max_registers;
                if (needed > vm->stack_cap) {
                    vm->stack_cap = needed < 64 ? 64 : needed * 2;
                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                }
                for (int i = nargs; i < fn->chunk->max_registers; i++)
                    vm->stack[callee->base + i] = make_null();
                vm->stack_count = needed > vm->stack_count ? needed : vm->stack_count;
                caller->ip = (int)(pc - current_chunk->code);
                vm->frame_count++;
                SYNC_LOCALS();
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
                Value result;
                if (fn->cfunc) {
                    result = luna_cfunc_dispatch(vm, fn, scratch, nargs + 1);
                } else {
                    if (!vm_call_native(vm, fn->native_fn, scratch, nargs + 1, &result)) goto op_throw;
                }
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            } else {
                /* Leaf fast path for bound-method kcall */
                if (LUNA_LIKELY(fn->is_leaf)) {
                    CallFrame *cf = &FRAME;
                    cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                    cf->leaf_ret_chunk = current_chunk;
                    cf->leaf_ret_base = FRAME.base;
                    cf->leaf_ret_reg = ret_reg;
                    cf->leaf_ret_closure = FRAME.closure;
                    cf->leaf_ret_fn      = FRAME.fn;
                    frame_set_refs(&FRAME, NULL, fn);
                    SET_REG(fn_reg, bm->self);
                    FRAME.base = cf->base + fn_reg;
                    FRAME.nargs = nargs + 1;
                    FRAME.kw_args = kw_args_val;
                    FRAME.chunk = fn->chunk;
                    FRAME.ip = 0;
                    SYNC_LOCALS();
                    int _needed = FRAME.base + fn->chunk->max_registers;
                    if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                        vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                        vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                        REG_BASE = vm->stack + FRAME.base;
                    }
                    vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                    for (int _i = nargs + 1; _i < fn->chunk->max_registers; _i++)
                        REG_BASE[_i] = make_null();
                    DECODE; goto *op_labels[OP(instr)];
                }
                if (vm->frame_count >= MAX_FRAMES) {
                    
                    _exc = make_exception_instance(vm, vm->exception_class, "call stack overflow");
                    goto op_throw;
                }
                CallFrame *caller = &FRAME;
                CallFrame *callee = &vm->frames[vm->frame_count];
                callee->chunk = fn->chunk;
                callee->ip = 0;
                SET_REG(fn_reg, bm->self);
                callee->base = caller->base + fn_reg;
                callee->ret_reg = ret_reg;
                callee->nargs = nargs + 1;
                callee->kw_args = kw_args_val;
                
                frame_set_refs(callee, NULL, fn);
                callee->saved_globals = NULL;
                int needed = callee->base + fn->chunk->max_registers;
                if (needed > vm->stack_cap) {
                    vm->stack_cap = needed < 64 ? 64 : needed * 2;
                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                }
                for (int i = nargs + 1; i < fn->chunk->max_registers; i++)
                    vm->stack[callee->base + i] = make_null();
                vm->stack_count = needed > vm->stack_count ? needed : vm->stack_count;
                caller->ip = (int)(pc - current_chunk->code);
                vm->frame_count++;
                SYNC_LOCALS();
                DECODE;
                goto *op_labels[OP(instr)];
            }
        } else {
            char *s = value_to_string(fn_val);
            char buf[256];
            if (IS_OBJ(fn_val)) {
                snprintf(buf, sizeof(buf), "attempt to call non-function (type=%d, value=%s)", (int)AS_OBJ(fn_val)->type, s);
            } else {
                snprintf(buf, sizeof(buf), "attempt to call non-function (got %s)", s);
            }
            free(s);
            
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 62. OP_COALESCE */
    op_coalesce: {
        if (!IS_NIL(REG(RA))) {
            pc += sBx;
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
            
            _exc = make_exception_instance(vm, vm->exception_class, "import: module name must be a string");
            goto op_throw;
        }
        const char *mod_name = ((ObjString*)AS_OBJ(mod_name_val))->chars;

        Value cached = dict_get(vm->module_cache, mod_name_val);
        if (!IS_NIL(cached) && IS_MODULE(cached)) {
            SET_REG(dst, cached);
            DECODE; goto *op_labels[OP(instr)];
        }

        char from_dir[1024];
        path_dirname(FRAME.chunk && FRAME.chunk->source_path ? FRAME.chunk->source_path : NULL, from_dir, sizeof(from_dir));

        char *filepath = module_resolve_path(mod_name, from_dir[0] ? from_dir : NULL);
        if (!filepath) {
            char buf[256];
            snprintf(buf, sizeof(buf), "module '%s' not found", mod_name);
            
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }

        char *source = module_read_source(filepath);
        if (!source) {
            free(filepath);
            char buf[256];
            snprintf(buf, sizeof(buf), "cannot read module '%s'", mod_name);
            
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }

        Lexer *lexer = lexer_new(source);
        TokenList *tokens = lexer_tokenize(lexer);

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
            free(source);
            char buf[256];
            snprintf(buf, sizeof(buf), "import: lexer errors in '%s'", mod_name);
            
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }

        Parser *parser = parser_new(tokens, source, filepath);
        Program *program = parser_parse(parser);

        if (parser->had_error) {
            parser_free(parser);
            token_list_free(tokens);
            lexer_free(lexer);
            free(source);
            free(filepath);
            char buf[256];
            snprintf(buf, sizeof(buf), "import: parser errors in '%s'", mod_name);
            
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
            
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }

        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        free(source);

        /* Create ObjFunction wrapper for the module chunk */
        ObjFunction *mod_fn = new_function(mod_name);
        mod_fn->chunk = malloc(sizeof(Chunk));
        *mod_fn->chunk = mod_chunk;
        mod_fn->chunk->source_path = filepath;

        /* Push function onto stack and set up call frame */
        SET_REG(dst, make_obj((Object*)mod_fn));

        if (vm->frame_count >= MAX_FRAMES) {
            
            _exc = make_exception_instance(vm, vm->exception_class, "call stack overflow");
            goto op_throw;
        }

        CallFrame *caller = &FRAME;
        CallFrame *callee = &vm->frames[vm->frame_count];
        callee->chunk         = mod_fn->chunk;
        callee->ip            = 0;
        callee->base          = caller->base + dst + 1; /* registers start after the fn slot */
        callee->ret_reg       = dst;
        callee->nargs         = 0;
        callee->kw_args       = make_null();
        frame_set_refs(callee, NULL, mod_fn);
        callee->saved_globals = saved_globals;

        int needed = callee->base + mod_fn->chunk->max_registers;
        if (needed > vm->stack_cap) {
            vm->stack_cap = needed < 64 ? 64 : needed * 2;
            vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
        }
        for (int i = 0; i < mod_fn->chunk->max_registers; i++)
            vm->stack[callee->base + i] = make_null();
        vm->stack_count = needed > vm->stack_count ? needed : vm->stack_count;
        caller->ip = (int)(pc - current_chunk->code);
        vm->frame_count++;
        SYNC_LOCALS();
        DECODE;
        goto *op_labels[OP(instr)];
    }

    /* 67. OP_TRYINIT */
    op_tryinit: {
        uint8_t obj_reg = RA;
        uint8_t nargs   = C;
        Value obj = REG(obj_reg);
        if (!IS_INSTANCE(obj)) {
            
            _exc = make_exception_instance(vm, vm->type_error_class, "TypeError: tryinit on non-instance");
            goto op_throw;
        }
        ObjInstance *inst = (ObjInstance*)AS_OBJ(obj);
        ObjClass *cls = inst->klass;
        int mi = -1;
        if (cls) {
            for (int i = 0; i < cls->method_count; i++) {
                if (cls->method_names[i] && strcmp(cls->method_names[i], "_init") == 0) { mi = i; break; }
            }
        }
        if (mi < 0) {
            if (nargs == 0) {
                DECODE; goto *op_labels[OP(instr)];
            }
            
            char buf[256];
            snprintf(buf, sizeof(buf), "RuntimeError: class '%s' has no _init constructor but %d argument(s) were provided",
                     cls ? cls->name : "?", nargs);
            _exc = make_exception_instance(vm, vm->runtime_error_class, buf);
            goto op_throw;
        }
        ObjFunction *fn = cls->methods[mi];
        if (fn->is_native) {
            Value scratch[256];
            scratch[0] = obj;
            for (int i = 0; i < nargs; i++) scratch[i + 1] = REG(obj_reg + 2 + i);
            Value result;
            if (fn->cfunc) {
                result = luna_cfunc_dispatch(vm, fn, scratch, nargs + 1);
            } else {
                if (!vm_call_native(vm, fn->native_fn, scratch, nargs + 1, &result)) goto op_throw;
            }
        } else if (vm->frame_count < MAX_FRAMES) {
            /* Leaf fast path: skip full frame push */
            if (LUNA_LIKELY(fn->is_leaf)) {
                CallFrame *cf = &FRAME;
                cf->leaf_ret_ip   = (int)(pc - current_chunk->code);
                cf->leaf_ret_chunk = current_chunk;
                cf->leaf_ret_base = FRAME.base;
                cf->leaf_ret_reg = (uint8_t)(obj_reg + nargs + 1);
                frame_set_refs(&FRAME, NULL, fn);
                FRAME.base = cf->base + obj_reg + 1;
                FRAME.nargs = nargs + 1;
                FRAME.kw_args = make_null();
                FRAME.chunk = fn->chunk;
                FRAME.ip = 0;
                SYNC_LOCALS();
                REG_BASE[0] = obj; /* self */
                int _needed = FRAME.base + fn->chunk->max_registers;
                if (LUNA_UNLIKELY(_needed > vm->stack_cap)) {
                    vm->stack_cap = _needed < 64 ? 64 : _needed * 2;
                    vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
                    REG_BASE = vm->stack + FRAME.base;
                }
                vm->stack_count = _needed > vm->stack_count ? _needed : vm->stack_count;
                for (int _i = 1 + nargs; _i < fn->chunk->max_registers; _i++)
                    REG_BASE[_i] = make_null();
                DECODE; goto *op_labels[OP(instr)];
            }
            CallFrame *caller = &FRAME;
            CallFrame *callee = &vm->frames[vm->frame_count];
            callee->chunk = fn->chunk;
            callee->ip = 0;
            callee->base = caller->base + obj_reg + 1;
            /* Use a dummy ret_reg past the args so _init's nil return
             * doesn't overwrite the instance at obj_reg. */
            callee->ret_reg = (uint8_t)(obj_reg + nargs + 1);
            callee->nargs = nargs + 1;
            callee->kw_args = make_null();
            frame_set_refs(callee, NULL, fn);
            callee->saved_globals = NULL;
            int need = callee->base + fn->chunk->max_registers;
            if (need > vm->stack_cap) {
                vm->stack_cap = need < 64 ? 64 : need * 2;
                vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
            }
            vm->stack[callee->base] = obj;
            for (int j = 1 + nargs; j < fn->chunk->max_registers; j++)
                vm->stack[callee->base + j] = make_null();
            vm->stack_count = need > vm->stack_count ? need : vm->stack_count;
            caller->ip = (int)(pc - current_chunk->code);
            vm->frame_count++;
            SYNC_LOCALS();
            DECODE;
            goto *op_labels[OP(instr)];
        } else {
            
            _exc = make_exception_instance(vm, vm->exception_class, "call stack overflow");
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 68. OP_HALT */
    op_halt: {
        CallFrame *frame = &FRAME;
        vm->last_return_value = REG(0);
        close_upvalues(vm, 0);
        
        frame->kw_args = make_null();
        for (int i = 0; i < frame->chunk->max_registers; i++) SET_REG(i, make_null());
        frame_release_refs(frame);
        vm->stack_count = 0;
        vm->frame_count = 0;
        return VM_OK;
    }
