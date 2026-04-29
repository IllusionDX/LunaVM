
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
        if (is_int_type(_L) && is_int_type(_R)) {
            SET_REG_PRIM(RA, make_int_result(to_i64(_L) + to_i64(_R)));
        } else {
            SET_REG(RA, do_arith(_L, _R, OP_ADD));
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 8.  OP_SUB */
    op_sub: { Value _L=RKB,_R=RKC; if(is_int_type(_L)&&is_int_type(_R)){SET_REG_PRIM(RA,make_int_result(to_i64(_L)-to_i64(_R)));}else{SET_REG(RA,do_arith(_L,_R,OP_SUB));} DECODE; goto *op_labels[OP(instr)]; }
    /* 9.  OP_MUL */
    op_mul: { Value _L=RKB,_R=RKC; if(is_int_type(_L)&&is_int_type(_R)){SET_REG_PRIM(RA,make_int_result(to_i64(_L)*to_i64(_R)));}else{SET_REG(RA,do_arith(_L,_R,OP_MUL));} DECODE; goto *op_labels[OP(instr)]; }

    /* 10. OP_DIV */
    op_div: {
        Value _L = RKB, _R = RKC;
        if (is_int_type(_L) && is_int_type(_R)) {
            int64_t ri = to_i64(_R);
            if (!ri) { release_value(_exc); _exc = make_exception_instance(vm, vm->runtime_error_class, "vm: division by zero"); goto op_throw; }
            SET_REG_PRIM(RA, make_int_result(to_i64(_L) / ri));
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
            if (!ri) { release_value(_exc); _exc = make_exception_instance(vm, vm->exception_class, "vm: mod/0"); goto op_throw; }
            SET_REG_PRIM(RA, make_int_result(to_i64(_L) % ri));
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
        if (is_int_type(v)) { SET_REG_PRIM(RA, make_int_result(-to_i64(v))); DECODE; goto *op_labels[OP(instr)]; }
        if (IS_DOUBLE(v)) { SET_REG_PRIM(RA, make_double(-AS_DOUBLE(v))); DECODE; goto *op_labels[OP(instr)]; }
        SET_REG_PRIM(RA, make_null());
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 13. OP_BAND */
    op_band: SET_REG_PRIM(RA, make_int_result(to_i64(REG(RB)) & to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    /* 14. OP_BOR */
    op_bor:  SET_REG_PRIM(RA, make_int_result(to_i64(REG(RB)) | to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    /* 15. OP_BXOR */
    op_bxor: SET_REG_PRIM(RA, make_int_result(to_i64(REG(RB)) ^ to_i64(REG(RC))));  DECODE; goto *op_labels[OP(instr)];
    /* 16. OP_BNOT */
    op_bnot: SET_REG_PRIM(RA, make_int_result(~to_i64(REG(RB)))); DECODE; goto *op_labels[OP(instr)];
    /* 17. OP_SHL */
    op_shl:  SET_REG_PRIM(RA, make_int_result(to_i64(REG(RB)) << to_i64(REG(RC)))); DECODE; goto *op_labels[OP(instr)];
    /* 18. OP_SHR */
    op_shr:  SET_REG_PRIM(RA, make_int_result(to_i64(REG(RB)) >> to_i64(REG(RC)))); DECODE; goto *op_labels[OP(instr)];

    /* ---- Integer-immediate arithmetic (ABC: A = A op (int8_t)B) ---- */
    /* 19. OP_ADDI */
    op_addi: {
        Value v = REG(RA);
        if (is_int_type(v))    REG(RA) = make_int_result(to_i64(v) + (int8_t)B);
        else if (IS_DOUBLE(v)) REG(RA) = make_double(AS_DOUBLE(v) + (double)(int8_t)B);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 20. OP_SUBI */
    op_subi: {
        Value v = REG(RA);
        if (is_int_type(v))    REG(RA) = make_int_result(to_i64(v) - (int8_t)B);
        else if (IS_DOUBLE(v)) REG(RA) = make_double(AS_DOUBLE(v) - (double)(int8_t)B);
        DECODE; goto *op_labels[OP(instr)];
    }

    /* ---- Comparison (ABC: A = B cmp C, result is bool) ---- */
    /* 21. OP_EQ */
    op_eq: SET_REG_PRIM(RA, do_cmp(RKB, RKC, OP_EQ)); DECODE; goto *op_labels[OP(instr)];
    /* 22. OP_NE */
    op_ne: SET_REG_PRIM(RA, do_cmp(RKB, RKC, OP_NE)); DECODE; goto *op_labels[OP(instr)];
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

    /* ---- Compare-and-branch (ABC: A=left, B=right, C=offset) ---- */
    op_lt_jz: {
        Value _L = REG(RA), _R = REG(RB);
        bool _c;
        if (is_int_type(_L) && is_int_type(_R)) {
            _c = to_i64(_L) < to_i64(_R);
        } else if (is_num(_L) && is_num(_R)) {
            _c = to_f64(_L) < to_f64(_R);
        } else {
            _c = is_truthy(do_cmp(_L, _R, OP_LT));
        }
        if (!_c) IP += C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_le_jz: {
        Value _L = REG(RA), _R = REG(RB);
        bool _c;
        if (is_int_type(_L) && is_int_type(_R)) {
            _c = to_i64(_L) <= to_i64(_R);
        } else if (is_num(_L) && is_num(_R)) {
            _c = to_f64(_L) <= to_f64(_R);
        } else {
            _c = is_truthy(do_cmp(_L, _R, OP_LE));
        }
        if (!_c) IP += C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_gt_jz: {
        Value _L = REG(RA), _R = REG(RB);
        bool _c;
        if (is_int_type(_L) && is_int_type(_R)) {
            _c = to_i64(_L) > to_i64(_R);
        } else if (is_num(_L) && is_num(_R)) {
            _c = to_f64(_L) > to_f64(_R);
        } else {
            _c = is_truthy(do_cmp(_L, _R, OP_GT));
        }
        if (!_c) IP += C;
        DECODE; goto *op_labels[OP(instr)];
    }
    op_ge_jz: {
        Value _L = REG(RA), _R = REG(RB);
        bool _c;
        if (is_int_type(_L) && is_int_type(_R)) {
            _c = to_i64(_L) >= to_i64(_R);
        } else if (is_num(_L) && is_num(_R)) {
            _c = to_f64(_L) >= to_f64(_R);
        } else {
            _c = is_truthy(do_cmp(_L, _R, OP_GE));
        }
        if (!_c) IP += C;
        DECODE; goto *op_labels[OP(instr)];
    }

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
                Value result;
                if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                SET_REG(ret_reg, result);
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
                if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                SET_REG(ret_reg, result);
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
                if (!vm_call_native(vm, fn->native_fn, scratch, nargs + 1, &result)) goto op_throw;
                SET_REG(ret_reg, result);
                DECODE; goto *op_labels[OP(instr)];
            }
            SET_REG(fn_reg, bm->self);
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
                    if (!vm_call_native(vm, fn->native_fn, scratch, nargs + 1, &result)) goto op_throw;
                    SET_REG(ret_reg, result);
                    DECODE; goto *op_labels[OP(instr)];
                }
                PUSH_FRAME(fn, NULL, FRAME.base + fn_reg, ret_reg, nargs, 1);
                DECODE;
                goto *op_labels[OP(instr)];
            } else {
                char buf[256];
                snprintf(buf, sizeof(buf), "vm: attempt to call instance of '%s' (no _call method)", inst->klass ? inst->klass->name : "?");
                release_value(_exc);
                _exc = make_exception_instance(vm, vm->exception_class, buf);
                goto op_throw;
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
            if (LUNA_UNLIKELY(vm->open_upvalues)) close_upvalues(vm, 0);
            for (int i = 0; i < FRAME.chunk->max_registers; i++) {
                Value _old = vm->stack[FRAME.base + i];
                vm->stack[FRAME.base + i] = make_null();
                release_value(_old);
            }
            vm->stack_count = 0;
            vm->frame_count = 0;
            if (IS_OBJ(retval) && AS_OBJ(retval)) release_obj(AS_OBJ(retval));
            return VM_OK;
        }
        CallFrame *callee = &FRAME;
        int old_base = callee->base;
        release_value(callee->kw_args);
        callee->kw_args = make_null();
        if (LUNA_UNLIKELY(vm->open_upvalues)) close_upvalues(vm, vm->frame_count);

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

        for (int i = 0; i < callee->chunk->max_registers; i++) {
            Value _old = vm->stack[callee->base + i];
            vm->stack[callee->base + i] = make_null();
            release_value(_old);
        }
        
        vm_pop_try_frames(vm, vm->frame_count - 1);
        vm->frame_count--;
        CallFrame *caller = &FRAME;
        CHUNK = caller->chunk;
        IP = caller->ip;
        SET_REG(callee->ret_reg, retval);
        if (IS_OBJ(retval) && AS_OBJ(retval)) release_obj(AS_OBJ(retval));
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
                if (IS_OBJ(val) && AS_OBJ(val)) retain_obj(AS_OBJ(val));
                Value old = uv->closed;
                uv->closed = val;
                if (IS_OBJ(old) && AS_OBJ(old)) release_obj(AS_OBJ(old));
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
                release_value(_exc);
                char buf[256];
                snprintf(buf, sizeof(buf), "vm: module '%s' not found for new", mod_name);
                _exc = make_exception_instance(vm, vm->exception_class, buf);
                goto op_throw;
            }
            ObjModule *mod = (ObjModule*)AS_OBJ(mod_val);
            ObjString *field = new_string(dot + 1, (int)strlen(dot + 1));
            cls_val = dict_get(mod->exports, make_obj((Object*)field));
            if (!IS_CLASS(cls_val)) {
                release_value(_exc);
                char buf[256];
                snprintf(buf, sizeof(buf), "vm: class '%s' not found in module '%s'", dot + 1, mod_name);
                _exc = make_exception_instance(vm, vm->exception_class, buf);
                goto op_throw;
            }
        } else {
            if (!vm_get_global(vm, cls_name, &cls_val) || !IS_CLASS(cls_val)) {
                release_value(_exc);
                char buf[256];
                snprintf(buf, sizeof(buf), "vm: class '%s' not found for new", cls_name);
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
                ObjDict *d = (ObjDict*)AS_OBJ(obj);
                if (!dict_has(d, key)) {
                    char *ks = value_to_string(key);
                    char buf[256];
                    snprintf(buf, sizeof(buf), "KeyError: %s not found in dict", ks);
                    free(ks);
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->key_error_class, buf);
                    goto op_throw;
                }
                SET_REG(RA, dict_get(d, key));
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
                if (field->length == 6 && !memcmp(field->chars, "length", 6)) {
                    SET_REG_PRIM(RA, make_int(((ObjString*)AS_OBJ(obj))->length));
                } else {
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
                else if (cls->fields) {
                    Value val = dict_get(cls->fields, make_obj((Object*)field));
                    if (!IS_NIL(val)) {
                        SET_REG(RA, val);
                    } else {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "AttributeError: class '%s' has no field '%s'", cls->name, field->chars);
                        release_value(_exc);
                        _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                        goto op_throw;
                    }
                } else {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "AttributeError: class '%s' has no field '%s'", cls->name, field->chars);
                    release_value(_exc);
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
                        release_value(_exc);
                        _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
                        goto op_throw;
                    }
                    SET_REG_PRIM(RA, make_double((double)v));
                } else {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "AttributeError: vector has no field '%s'", field->chars);
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->attribute_error_class, buf);
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
                SET_REG(RA, dict_get(mod->exports, make_obj((Object*)field)));
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
                    release_value(_exc);
                    _exc = make_exception_instance(vm, vm->attribute_error_class, "AttributeError: vector has no field");
                    goto op_throw;
                }
            } else {
                release_value(_exc);
                _exc = make_exception_instance(vm, vm->attribute_error_class, "AttributeError: vector has no field");
                goto op_throw;
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
                case OBJ_BUFFER: {
                    ObjBuffer *buf = (ObjBuffer*)AS_OBJ(obj);
                    if (!strcmp(mname, "read_byte")) {
                        if (nargs != 0) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->argument_error_class, "buffer.read_byte() expects no arguments");
                            goto op_throw;
                        }
                        result = buffer_read_byte(buf, buf->cursor);
                        if (!IS_NIL(result)) buf->cursor += 1;
                        handled = true;
                    } else if (!strcmp(mname, "read_short")) {
                        if (nargs != 0) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->argument_error_class, "buffer.read_short() expects no arguments");
                            goto op_throw;
                        }
                        result = buffer_read_short(buf, buf->cursor);
                        if (!IS_NIL(result)) buf->cursor += 2;
                        handled = true;
                    } else if (!strcmp(mname, "read_int")) {
                        if (nargs != 0) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->argument_error_class, "buffer.read_int() expects no arguments");
                            goto op_throw;
                        }
                        result = buffer_read_int(buf, buf->cursor);
                        if (!IS_NIL(result)) buf->cursor += 4;
                        handled = true;
                    } else if (!strcmp(mname, "read_long")) {
                        if (nargs != 0) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->argument_error_class, "buffer.read_long() expects no arguments");
                            goto op_throw;
                        }
                        result = buffer_read_long(buf, buf->cursor);
                        if (!IS_NIL(result)) buf->cursor += 8;
                        handled = true;
                    }
                    break;
                }
                case OBJ_CLASS: {
                    ObjClass *cls = (ObjClass*)AS_OBJ(obj);
                    ObjString *mname_obj = (ObjString*)AS_OBJ(method_val);
                    Value fn_val = cls->fields ? dict_get(cls->fields, make_obj((Object*)mname_obj)) : make_null();
                    if (IS_FUNCTION(fn_val)) {
                        ObjFunction *fn = (ObjFunction*)AS_OBJ(fn_val);
                        if (fn->is_native) {
                            if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                        } else if (vm->frame_count < MAX_FRAMES) {
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
                            if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                        } else if (vm->frame_count < MAX_FRAMES) {
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
                            vm->frame_count++;
                            CHUNK = callee->chunk;
                            IP = 0;
                            DECODE;
                            goto *op_labels[OP(instr)];
                        }
                        handled = true;
                    }
                    if (!handled) {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "AttributeError: class '%s' has no method '%s'", cls->name, mname_obj->chars);
                        release_value(_exc);
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
                            if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                        } else if (vm->frame_count < MAX_FRAMES) {
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
                            if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
                        } else if (vm->frame_count < MAX_FRAMES) {
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
                                Value native_scratch[256];
                                native_scratch[0] = obj;
                                for (int i = 0; i < nargs; i++) native_scratch[i + 1] = scratch[i];
                                if (!vm_call_native(vm, m->native_fn, native_scratch, nargs + 1, &result)) goto op_throw;
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
                                    frame_set_refs(callee, NULL, m);
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
                        } else if (strcmp(mname, "_init") == 0) {
                            /* Constructors are optional: allow `new Class()` when no _init exists. */
                            result = make_null();
                            handled = true;
                        }
                    }
                    break;
                }
                case OBJ_STRING: {
                    if (!strcmp(mname, "to_buffer")) {
                        if (nargs != 0) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->argument_error_class, "string.to_buffer() takes no arguments");
                            goto op_throw;
                        }
                        ObjString *str = (ObjString*)AS_OBJ(obj);
                        ObjBuffer *buf = new_buffer((size_t)str->length);
                        buffer_append_data(buf, (const uint8_t*)str->chars, (size_t)str->length);
                        result = make_obj((Object*)buf);
                        handled = true;
                    } else if (strcmp(mname, "byte_at") == 0) {
                        if (nargs < 1 || (!IS_INT(scratch[0]) && !IS_INT64(scratch[0]))) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->type_error_class, "string.byte_at() expects an integer index");
                            goto op_throw;
                        }
                        ObjString *str = (ObjString*)AS_OBJ(obj);
                        int64_t idx64 = as_int64(scratch[0]);
                        if (idx64 < 0 || idx64 >= str->length) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->index_error_class, "string.byte_at() index out of range");
                            goto op_throw;
                        }
                        result = make_int((uint8_t)str->chars[idx64]);
                        handled = true;
                    }
                    break;
                }
                case OBJ_VECTOR: {
                    ObjVector *vec = (ObjVector*)AS_OBJ(obj);
                    if (!strcmp(mname, "add")) {
                        if (nargs < 1 || !IS_VECTOR(scratch[0])) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->type_error_class, "vector.add() expects a vector argument");
                            goto op_throw;
                        }
                        ObjVector *other = (ObjVector*)AS_OBJ(scratch[0]);
                        float s = 1.0f;
                        if (nargs >= 2) {
                            if (!IS_NUMBER(scratch[1])) {
                                release_value(_exc);
                                _exc = make_exception_instance(vm, vm->type_error_class, "vector.add() scalar must be numeric");
                                goto op_throw;
                            }
                            s = (float)value_to_double(scratch[1]);
                        }
                        vec->data[0] += other->data[0] * s;
                        vec->data[1] += other->data[1] * s;
                        vec->data[2] += other->data[2] * s;
                        vec->data[3] += other->data[3] * s;
                        result = obj;
                        handled = true;
                    } else if (!strcmp(mname, "sub")) {
                        if (nargs < 1 || !IS_VECTOR(scratch[0])) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->type_error_class, "vector.sub() expects a vector argument");
                            goto op_throw;
                        }
                        ObjVector *other = (ObjVector*)AS_OBJ(scratch[0]);
                        vec->data[0] -= other->data[0];
                        vec->data[1] -= other->data[1];
                        vec->data[2] -= other->data[2];
                        vec->data[3] -= other->data[3];
                        result = obj;
                        handled = true;
                    } else if (!strcmp(mname, "mul")) {
                        if (nargs < 1 || !IS_NUMBER(scratch[0])) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->type_error_class, "vector.mul() expects a numeric argument");
                            goto op_throw;
                        }
                        float s = (float)value_to_double(scratch[0]);
                        vec->data[0] *= s;
                        vec->data[1] *= s;
                        vec->data[2] *= s;
                        vec->data[3] *= s;
                        result = obj;
                        handled = true;
                    } else if (!strcmp(mname, "copy")) {
                        if (nargs != 0) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->argument_error_class, "vector.copy() takes no arguments");
                            goto op_throw;
                        }
                        result = make_obj((Object*)new_vector(vec->data[0], vec->data[1], vec->data[2], vec->data[3]));
                        handled = true;
                    }
                    break;
                }
                case OBJ_MATRIX: {
                    ObjMatrix *mat = (ObjMatrix*)AS_OBJ(obj);
                    if (!strcmp(mname, "translate")) {
                        if (nargs < 1 || !IS_VECTOR(scratch[0])) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->type_error_class, "mat4.translate() expects a vector argument");
                            goto op_throw;
                        }
                        ObjVector *v = (ObjVector*)AS_OBJ(scratch[0]);
                        mat4_mul_translate(mat->m, v->data[0], v->data[1], v->data[2]);
                        result = obj;
                        handled = true;
                    } else if (!strcmp(mname, "rotate_x")) {
                        if (nargs < 1 || !IS_NUMBER(scratch[0])) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->type_error_class, "mat4.rotate_x() expects a numeric angle (degrees)");
                            goto op_throw;
                        }
                        mat4_mul_rotate_x(mat->m, (float)(value_to_double(scratch[0]) * 3.141592653589793 / 180.0));
                        result = obj;
                        handled = true;
                    } else if (!strcmp(mname, "rotate_y")) {
                        if (nargs < 1 || !IS_NUMBER(scratch[0])) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->type_error_class, "mat4.rotate_y() expects a numeric angle (degrees)");
                            goto op_throw;
                        }
                        mat4_mul_rotate_y(mat->m, (float)(value_to_double(scratch[0]) * 3.141592653589793 / 180.0));
                        result = obj;
                        handled = true;
                    } else if (!strcmp(mname, "rotate_z")) {
                        if (nargs < 1 || !IS_NUMBER(scratch[0])) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->type_error_class, "mat4.rotate_z() expects a numeric angle (degrees)");
                            goto op_throw;
                        }
                        mat4_mul_rotate_z(mat->m, (float)(value_to_double(scratch[0]) * 3.141592653589793 / 180.0));
                        result = obj;
                        handled = true;
                    } else if (!strcmp(mname, "scale")) {
                        if (nargs < 1 || !IS_VECTOR(scratch[0])) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->type_error_class, "mat4.scale() expects a vector argument");
                            goto op_throw;
                        }
                        ObjVector *v = (ObjVector*)AS_OBJ(scratch[0]);
                        mat4_mul_scale(mat->m, v->data[0], v->data[1], v->data[2]);
                        result = obj;
                        handled = true;
                    } else if (!strcmp(mname, "transpose")) {
                        mat4_transpose(mat->m, mat->m);
                        result = obj;
                        handled = true;
                    } else if (!strcmp(mname, "transposed")) {
                        if (nargs != 0) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->argument_error_class, "mat4.transposed() takes no arguments");
                            goto op_throw;
                        }
                        ObjMatrix *out = new_matrix();
                        mat4_transpose(mat->m, out->m);
                        result = make_obj((Object*)out);
                        handled = true;
                    } else if (!strcmp(mname, "invert")) {
                        ObjMatrix *tmp = new_matrix();
                        if (!mat4_invert(mat->m, tmp->m)) {
                            release_obj((Object*)tmp);
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->value_error_class, "mat4.invert(): matrix is singular");
                            goto op_throw;
                        }
                        for (int i = 0; i < 16; i++) mat->m[i] = tmp->m[i];
                        release_obj((Object*)tmp);
                        result = obj;
                        handled = true;
                    } else if (!strcmp(mname, "inverted")) {
                        if (nargs != 0) {
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->argument_error_class, "mat4.inverted() takes no arguments");
                            goto op_throw;
                        }
                        ObjMatrix *out = new_matrix();
                        if (!mat4_invert(mat->m, out->m)) {
                            release_obj((Object*)out);
                            release_value(_exc);
                            _exc = make_exception_instance(vm, vm->value_error_class, "mat4.inverted(): matrix is singular");
                            goto op_throw;
                        }
                        result = make_obj((Object*)out);
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
                            if (!vm_call_native(vm, m->native_fn, scratch, nargs, &result)) goto op_throw;
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
            if (IS_OBJ(vm->last_exception) && AS_OBJ(vm->last_exception)) {
                _exc = vm->last_exception;
            } else {
                _exc = REG(RA);
            }
        }
        vm->last_exception = _exc;
        /* unwind to the nearest catch */
        while (vm->try_stack) {
            TryFrame *tf = vm->try_stack;
            if (tf->frame_depth <= vm->frame_count) {
                /* unwind call frames */
                while (vm->frame_count > tf->frame_depth) {
                    CallFrame *unwound = &vm->frames[vm->frame_count - 1];
                    release_value(unwound->kw_args);
                    unwound->kw_args = make_null();
                    frame_release_refs(unwound);
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
                Value result;
                if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
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
                callee->ret_reg = ret_reg;
                callee->nargs = nargs;
                callee->kw_args = kw_args_val;
                retain_value(kw_args_val);
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
                Value result;
                if (!vm_call_native(vm, fn->native_fn, scratch, nargs, &result)) goto op_throw;
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
                callee->ret_reg = ret_reg;
                callee->nargs = nargs;
                callee->kw_args = kw_args_val;
                retain_value(kw_args_val);
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
                Value result;
                if (!vm_call_native(vm, fn->native_fn, scratch, nargs + 1, &result)) goto op_throw;
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
                callee->ret_reg = ret_reg;
                callee->nargs = nargs;
                callee->kw_args = kw_args_val;
                retain_value(kw_args_val);
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

        char from_dir[1024];
        path_dirname(FRAME.chunk && FRAME.chunk->source_path ? FRAME.chunk->source_path : NULL, from_dir, sizeof(from_dir));

        char *filepath = module_resolve_path(mod_name, from_dir[0] ? from_dir : NULL);
        if (!filepath) {
            char buf[256];
            snprintf(buf, sizeof(buf), "import: module '%s' not found", mod_name);
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, buf);
            goto op_throw;
        }

        char *source = module_read_source(filepath);
        if (!source) {
            free(filepath);
            char buf[256];
            snprintf(buf, sizeof(buf), "import: cannot read module '%s'", mod_name);
            release_value(_exc);
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
            release_value(_exc);
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
        free(source);

        /* Create ObjFunction wrapper for the module chunk */
        ObjFunction *mod_fn = new_function(mod_name);
        mod_fn->chunk = malloc(sizeof(Chunk));
        *mod_fn->chunk = mod_chunk;
        mod_fn->chunk->source_path = filepath;

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
        vm->frame_count++;
        CHUNK = callee->chunk;
        IP = 0;
        DECODE;
        goto *op_labels[OP(instr)];
    }

    /* 67. OP_TRYINIT */
    op_tryinit: {
        uint8_t obj_reg = RA;
        uint8_t nargs   = C;
        Value obj = REG(obj_reg);
        if (!IS_INSTANCE(obj)) {
            release_value(_exc);
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
            release_value(_exc);
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
            if (!vm_call_native(vm, fn->native_fn, scratch, nargs + 1, &result)) goto op_throw;
        } else if (vm->frame_count < MAX_FRAMES) {
            CallFrame *caller = &FRAME;
            CallFrame *callee = &vm->frames[vm->frame_count];
            callee->chunk = fn->chunk;
            callee->ip = 0;
            callee->base = caller->base + obj_reg + 1;
            /* Use a dummy ret_reg past the args so _init's nil return
             * doesn't overwrite the instance at obj_reg. */
            callee->ret_reg = (uint8_t)(obj_reg + nargs + 1);
            callee->nargs = nargs;
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
            vm->frame_count++;
            CHUNK = callee->chunk;
            IP = 0;
            DECODE;
            goto *op_labels[OP(instr)];
        } else {
            release_value(_exc);
            _exc = make_exception_instance(vm, vm->exception_class, "vm: call stack overflow");
            goto op_throw;
        }
        DECODE; goto *op_labels[OP(instr)];
    }

    /* 68. OP_HALT */
    op_halt: {
        CallFrame *frame = &FRAME;
        close_upvalues(vm, 0);
        release_value(frame->kw_args);
        frame->kw_args = make_null();
        for (int i = 0; i < frame->chunk->max_registers; i++) SET_REG(i, make_null());
        frame_release_refs(frame);
        vm->stack_count = 0;
        vm->frame_count = 0;
        return VM_OK;
    }
