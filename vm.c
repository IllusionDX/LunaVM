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
    switch(v.type){
        case VAL_INT:    return (double)v.as.integer;
        case VAL_UINT:   return (double)v.as.uint_val;
        case VAL_FLOAT:  return (double)v.as.float_val;
        case VAL_DOUBLE: return v.as.double_val;
        default:         return 0.0;
    }
}
static inline int64_t to_i64(Value v) {
    switch(v.type){
        case VAL_INT:    return v.as.integer;
        case VAL_UINT:   return (int64_t)v.as.uint_val;
        case VAL_FLOAT:  return (int64_t)v.as.float_val;
        case VAL_DOUBLE: return (int64_t)v.as.double_val;
        default:         return 0;
    }
}
static inline bool is_num(Value v) {
    return v.type==VAL_INT||v.type==VAL_UINT||v.type==VAL_FLOAT||v.type==VAL_DOUBLE;
}
static inline bool is_int_type(Value v) { return v.type==VAL_INT||v.type==VAL_UINT; }

static Value do_arith(Value L, Value R, OpCode op) {
    /* String concat for ADD */
    if (op==OP_ADD && L.type==VAL_OBJ && L.as.obj && L.as.obj->type==OBJ_STRING) {
        ObjString *ls = (ObjString*)L.as.obj;
        const char *rs;
        int rs_len;
        char *rs_tmp = NULL;
        if (R.type==VAL_OBJ && R.as.obj && R.as.obj->type==OBJ_STRING) {
            ObjString *rs_str = (ObjString*)R.as.obj;
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
    if (!is_num(L)||!is_num(R)) return make_null();
    bool fp = L.type==VAL_DOUBLE||R.type==VAL_DOUBLE||L.type==VAL_FLOAT||R.type==VAL_FLOAT;
    switch(op){
        case OP_ADD: if(fp){double r=to_f64(L)+to_f64(R); return (L.type==VAL_DOUBLE||R.type==VAL_DOUBLE)?make_double(r):make_float((float)r);} return make_int(to_i64(L)+to_i64(R));
        case OP_SUB: if(fp){double r=to_f64(L)-to_f64(R); return (L.type==VAL_DOUBLE||R.type==VAL_DOUBLE)?make_double(r):make_float((float)r);} return make_int(to_i64(L)-to_i64(R));
        case OP_MUL: if(fp){double r=to_f64(L)*to_f64(R); return (L.type==VAL_DOUBLE||R.type==VAL_DOUBLE)?make_double(r):make_float((float)r);} return make_int(to_i64(L)*to_i64(R));
        case OP_DIV: { double d=to_f64(R); if(d==0.0){fprintf(stderr,"vm: div/0\n");return make_null();} if(fp){double r=to_f64(L)/d;return(L.type==VAL_DOUBLE||R.type==VAL_DOUBLE)?make_double(r):make_float((float)r);}return make_int(to_i64(L)/to_i64(R)); }
        case OP_MOD: { int64_t ri=to_i64(R); if(!ri){fprintf(stderr,"vm: mod/0\n");return make_null();} return make_int(to_i64(L)%ri); }
        default: return make_null();
    }
}
static Value do_cmp(Value L, Value R, OpCode op) {
    if (is_num(L)&&is_num(R)){
        double a=to_f64(L),b=to_f64(R);
        switch(op){case OP_LT:return make_bool(a<b);case OP_LE:return make_bool(a<=b);case OP_GT:return make_bool(a>b);case OP_GE:return make_bool(a>=b);default:break;}
    }
    /* Fast path: interned string equality is pointer comparison */
    if (L.type == VAL_OBJ && R.type == VAL_OBJ && L.as.obj && R.as.obj &&
        L.as.obj->type == OBJ_STRING && R.as.obj->type == OBJ_STRING) {
        bool same = L.as.obj == R.as.obj;
        switch(op){
            case OP_EQ: return make_bool(same);
            case OP_NE: return make_bool(!same);
            default: break;
        }
    }
    switch(op){
        case OP_EQ: return make_bool(values_equal(L,R));
        case OP_NE: return make_bool(!values_equal(L,R));
        default: return make_null();
    }
}

/* ============================================================ */
/* VM init / free                                                */
/* ============================================================ */

void vm_init(VM *vm) { memset(vm,0,sizeof(VM)); }

static void close_upvalues(VM *vm, int frame_depth);

static void vm_pop_try_frames(VM *vm, int min_depth) {
    while (vm->try_stack && vm->try_stack->frame_depth > min_depth) {
        TryFrame *tf = vm->try_stack;
        vm->try_stack = tf->next;
        free(tf);
    }
}

void vm_free(VM *vm) {
    for (int i=0;i<VM_GLOBAL_BUCKETS;i++) {
        GlobalEntry *e=vm->globals[i];
        while(e){ GlobalEntry *nx=e->next; release_obj(e->value.type==VAL_OBJ?e->value.as.obj:NULL); free(e->name);free(e);e=nx; }
        vm->globals[i]=NULL;
    }
    Object *o=vm->objects;
    while(o){ Object *nx=o->next; free_object(o); o=nx; }
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
#define RA          DECODE_A(inst)
#define RB          DECODE_B(inst)
#define RC          DECODE_C(inst)
#define BX          DECODE_Bx(inst)
#define SBX         DECODE_sBx(inst)
#define REG(n)      (FRAME.regs[(uint8_t)(n)])
#define CONST(n)    (CHUNK->constants[(int)(n)])
#define KSTR(n)     (((ObjString*)CONST(n).as.obj)->chars)

static ObjUpvalue *capture_upvalue(VM *vm, Value *slot) {
    ObjUpvalue *uv = new_upvalue(slot);
    uv->frame_depth = vm->frame_count;
    uv->next = vm->open_upvalues;
    vm->open_upvalues = uv;
    return uv;
}

static void close_upvalues(VM *vm, int frame_depth) {
    ObjUpvalue **current = &vm->open_upvalues;
    while (*current) {
        if ((*current)->frame_depth >= frame_depth) {
            ObjUpvalue *uv = *current;
            uv->closed = *uv->location;
            uv->location = &uv->closed;
            *current = uv->next;
        } else {
            current = &(*current)->next;
        }
    }
}

static VMResult push_frame(VM *vm, Chunk *chunk, int ret_reg) {
    if (vm->frame_count >= VM_MAX_FRAMES) { fprintf(stderr,"vm: stack overflow\n"); return VM_ERROR; }
    CallFrame *f = &vm->frames[vm->frame_count++];
    f->chunk   = chunk;
    f->ip      = 0;
    f->ret_reg = ret_reg;
    f->has_self= false;
    f->self_val= make_null();
    f->upvalue_count = 0;
    for (int i=0;i<VM_MAX_REGISTERS;i++) f->regs[i]=make_null();
    for (int i=0;i<VM_MAX_REGISTERS;i++) f->upvalues[i]=NULL;
    return VM_OK;
}

VMResult vm_run_chunk(VM *vm, Chunk *top) {
    static bool init = false;
    if (!init) { vm_register_builtins(vm); init = true; }

    if (push_frame(vm, top, -1) != VM_OK) return VM_ERROR;

dispatch:
    while (vm->frame_count > 0) {
        if (IP >= CHUNK->count) {
            /* implicit return null from top frame */
            if (vm->frame_count == 1) {
                while (vm->try_stack) {
                    TryFrame *tf = vm->try_stack;
                    vm->try_stack = tf->next;
                    free(tf);
                }
                close_upvalues(vm, 1);
                vm->frame_count=0; return VM_OK;
            }
            int rr = FRAME.ret_reg;
            int closing_depth = vm->frame_count;
            vm->frame_count--;
            close_upvalues(vm, closing_depth);
            vm_pop_try_frames(vm, vm->frame_count);
            if (rr >= 0) REG(rr) = make_null();
            goto dispatch;
        }

        uint32_t inst = CHUNK->code[IP++];
        OpCode op = (OpCode)DECODE_OP(inst);

        static void *op_labels[OP_COUNT] = {
            &&op_loadk, &&op_loadnull, &&op_loadtrue, &&op_loadfalse, &&op_loadi,
            &&op_move, &&op_copy, &&op_swap,
            &&op_add, &&op_sub, &&op_mul, &&op_div, &&op_mod, &&op_neg,
            &&op_band, &&op_bor, &&op_bxor, &&op_bnot, &&op_shl, &&op_shr,
            &&op_eq, &&op_ne, &&op_lt, &&op_le, &&op_gt, &&op_ge,
            &&op_not,
            &&op_jmp, &&op_jz, &&op_jnz,
            &&op_call, &&op_ret, &&op_enter, &&op_leave, &&op_closure,
            &&op_getglobal, &&op_setglobal,
            &&op_getupval, &&op_setupval,
            &&op_new, &&op_newdict, &&op_newlist,
            &&op_indexget, &&op_indexset, &&op_memberget, &&op_memberset,
            &&op_invoke, &&op_super,
            &&op_throw, &&op_try, &&op_endtry,
            &&op_halt
        };

        goto *op_labels[op];

    /* ---- Load / Move ---- */
    op_loadk:     REG(RA) = CONST(BX);        goto dispatch;
    op_loadnull:  REG(RA) = make_null();       goto dispatch;
    op_loadtrue:  REG(RA) = make_bool(true);   goto dispatch;
    op_loadfalse: REG(RA) = make_bool(false);  goto dispatch;
    op_loadi:     REG(RA) = make_int(SBX);     goto dispatch;
    op_move:      REG(RA) = REG(RB);           goto dispatch;
    op_copy:      {
        Value v=REG(RB);
        if(v.type==VAL_OBJ&&v.as.obj) retain_obj(v.as.obj);
        REG(RA)=v; goto dispatch;
    }
    op_swap:      { Value t=REG(RA); REG(RA)=REG(RB); REG(RB)=t; goto dispatch; }

    /* ---- Globals ---- */
    op_getglobal: {
        if(CONST(BX).type!=VAL_OBJ||!CONST(BX).as.obj){REG(RA)=make_null();goto dispatch;}
        const char *nm=KSTR(BX);

        /* Inline cache: check cached GlobalEntry at this instruction */
        int inst_idx = IP - 1;
        GlobalEntry *cached = CHUNK->global_cache ? CHUNK->global_cache[inst_idx] : NULL;
        if (cached && cached->name && strcmp(cached->name, nm) == 0) {
            REG(RA) = cached->value;
            goto dispatch;
        }

        GlobalEntry *e = vm_resolve_global(vm, nm);
        if (!e) {
            fprintf(stderr,"vm: undefined '%s'\n",nm);
            REG(RA)=make_null();
        } else {
            REG(RA)=e->value;
            if (!CHUNK->global_cache) {
                CHUNK->global_cache = calloc((size_t)CHUNK->capacity, sizeof(GlobalEntry*));
            }
            CHUNK->global_cache[inst_idx] = e;
        }
        goto dispatch;
    }
    op_setglobal: {
        if(CONST(BX).type!=VAL_OBJ||!CONST(BX).as.obj) goto dispatch;
        vm_set_global(vm, KSTR(BX), REG(RA), false);
        goto dispatch;
    }

    /* ---- Arithmetic ---- */
    op_add:
    op_sub:
    op_mul:
    op_div:
    op_mod:
        REG(RA) = do_arith(REG(RB), REG(RC), op); goto dispatch;
    op_neg: {
        Value v=REG(RB);
        if(v.type==VAL_INT)    { REG(RA)=make_int(-v.as.integer);    goto dispatch; }
        if(v.type==VAL_FLOAT)  { REG(RA)=make_float(-v.as.float_val); goto dispatch; }
        if(v.type==VAL_DOUBLE) { REG(RA)=make_double(-v.as.double_val);goto dispatch; }
        REG(RA)=make_null(); goto dispatch;
    }
    op_band: REG(RA)=make_int(to_i64(REG(RB))&to_i64(REG(RC)));  goto dispatch;
    op_bor:  REG(RA)=make_int(to_i64(REG(RB))|to_i64(REG(RC)));  goto dispatch;
    op_bxor: REG(RA)=make_int(to_i64(REG(RB))^to_i64(REG(RC)));  goto dispatch;
    op_shl:  REG(RA)=make_int(to_i64(REG(RB))<<to_i64(REG(RC))); goto dispatch;
    op_shr:  REG(RA)=make_int(to_i64(REG(RB))>>to_i64(REG(RC))); goto dispatch;
    op_bnot: REG(RA)=make_int(~to_i64(REG(RB))); goto dispatch;

    /* ---- Comparison / Logical ---- */
    op_eq:
    op_ne:
    op_lt:
    op_le:
    op_gt:
    op_ge:
        REG(RA) = do_cmp(REG(RB), REG(RC), op); goto dispatch;
    op_not: REG(RA) = make_bool(!is_truthy(REG(RB))); goto dispatch;

    /* ---- Control Flow ---- */
    op_jmp:  IP += SBX; goto dispatch;
    op_jz:   if(!is_truthy(REG(RA))) IP += SBX; goto dispatch;
    op_jnz:  if( is_truthy(REG(RA))) IP += SBX; goto dispatch;

    /* ---- CALL ---- */
    op_call: {
        /* A=dest, B=fn-reg, C=nargs; args in B+1..B+C */
        Value fn_val = REG(RB);
        int   nargs  = (int)RC;
        if (fn_val.type!=VAL_OBJ||!fn_val.as.obj) {
            fprintf(stderr,"vm: call non-function\n"); REG(RA)=make_null(); goto dispatch;
        }
        ObjFunction *fn = NULL;
        ObjClosure *cl = NULL;
        if (fn_val.as.obj->type == OBJ_FUNCTION) {
            fn = (ObjFunction *)fn_val.as.obj;
        } else if (fn_val.as.obj->type == OBJ_CLOSURE) {
            cl = (ObjClosure *)fn_val.as.obj;
            fn = cl->function;
        } else {
            fprintf(stderr,"vm: not callable\n"); REG(RA)=make_null(); goto dispatch;
        }
        if (fn->is_native) {
            /* collect args into temporary buffer (stack scratch, fallback to heap) */
            Value scratch[256];
            Value *argv = (nargs > 0 && nargs <= 256) ? scratch : (nargs > 0 ? malloc(sizeof(Value)*nargs) : NULL);
            for (int i = 0; i < nargs; i++) argv[i] = REG(RB+1+i);
            Value res = fn->native_fn(vm, argv, nargs);
            if (argv != scratch) free(argv);
            REG(RA) = res;
            goto dispatch;
        }
        /* Luna function — push new frame */
        if (!fn->chunk) { fprintf(stderr,"vm: fn '%s' no bytecode\n", fn->name); REG(RA)=make_null(); goto dispatch; }
        int ret_dest = (int)RA;
        /* save args from current frame before push */
        Value saved[256]; int sn = nargs < 256 ? nargs : 255;
        for (int i=0;i<sn;i++) saved[i]=REG(RB+1+i);
        if (push_frame(vm, fn->chunk, ret_dest) != VM_OK) return VM_ERROR;
        for (int i=0;i<sn;i++) FRAME.regs[i]=saved[i];
        /* Copy closure upvalues into the new frame */
        if (cl) {
            for (int i=0;i<cl->upvalue_count && i<VM_MAX_REGISTERS;i++) {
                FRAME.upvalues[i] = cl->upvalues[i];
                if (cl->upvalues[i]) retain_obj((Object*)cl->upvalues[i]);
            }
            FRAME.upvalue_count = cl->upvalue_count;
        }
        goto dispatch;
    }

    /* ---- RET ---- */
    op_ret: {
        Value ret_val = REG(RA);
        if (vm->frame_count <= 1) {
            while (vm->try_stack) {
                TryFrame *tf = vm->try_stack;
                vm->try_stack = tf->next;
                free(tf);
            }
            close_upvalues(vm, 1);
            vm->frame_count=0; return VM_OK;
        }
        int rr = FRAME.ret_reg;
        int closing_depth = vm->frame_count;
        vm->frame_count--;
        close_upvalues(vm, closing_depth);
        vm_pop_try_frames(vm, vm->frame_count);
        if (rr >= 0) REG(rr) = ret_val;
        goto dispatch;
    }

    op_enter:
    op_leave:
        goto dispatch; /* hints, no-op in register VM */

    /* ---- Collections ---- */
    op_newlist: REG(RA) = make_obj((Object*)new_list()); goto dispatch;
    op_newdict: REG(RA) = make_obj((Object*)new_dict()); goto dispatch;

    /* ---- NEW instance ---- */
    op_new: {
        if(CONST(BX).type!=VAL_OBJ||!CONST(BX).as.obj){REG(RA)=make_null();goto dispatch;}
        const char *cls_name = KSTR(BX);
        ObjInstance *new_inst = new_instance(cls_name, NULL, 4);
        /* Copy methods from class prototype if it exists as a global */
        Value proto_val;
        if (vm_get_global(vm, cls_name, &proto_val) && proto_val.type==VAL_OBJ && proto_val.as.obj && proto_val.as.obj->type==OBJ_INSTANCE) {
            ObjInstance *proto = (ObjInstance*)proto_val.as.obj;
            if (proto->method_count > 0) {
                new_inst->methods = malloc(sizeof(ObjFunction*) * proto->method_count);
                new_inst->method_count = proto->method_count;
                new_inst->method_capacity = proto->method_count;
                for (int mi=0; mi<proto->method_count; mi++) {
                    new_inst->methods[mi] = proto->methods[mi];
                }
            }
            /* Copy fields from prototype */
            for (int fi=0; fi<proto->field_count; fi++) {
                instance_set_field(new_inst, proto->field_names[fi], proto->fields[fi]);
            }
        }
        REG(RA) = make_obj((Object*)new_inst);
        goto dispatch;
    }

    /* ---- Index access ---- */
    op_indexget: {
        Value obj=REG(RB), key=REG(RC);
        if(obj.type==VAL_OBJ&&obj.as.obj){
            switch(obj.as.obj->type){
                case OBJ_LIST:
                    REG(RA)= key.type==VAL_INT ? list_get((ObjList*)obj.as.obj,(int)key.as.integer) : make_null(); break;
                case OBJ_DICT:
                    REG(RA)= dict_get((ObjDict*)obj.as.obj, key); break;
                case OBJ_STRING: {
                    ObjString *s=(ObjString*)obj.as.obj;
                    if(key.type==VAL_INT&&key.as.integer>=0&&key.as.integer<s->length)
                        REG(RA)=make_obj((Object*)new_string(&s->chars[(int)key.as.integer],1));
                    else REG(RA)=make_null();
                    break;
                }
                default: REG(RA)=make_null(); break;
            }
        } else REG(RA)=make_null();
        goto dispatch;
    }
    op_indexset: {
        /* A=obj, B=key, C=value */
        Value obj=REG(RA), key=REG(RB), val=REG(RC);
        if(obj.type==VAL_OBJ&&obj.as.obj){
            if(obj.as.obj->type==OBJ_LIST&&key.type==VAL_INT) list_set((ObjList*)obj.as.obj,(int)key.as.integer,val);
            else if(obj.as.obj->type==OBJ_DICT) dict_set((ObjDict*)obj.as.obj,key,val);
        }
        goto dispatch;
    }

    /* ---- Member access: A=dest, B=obj, Bx has field const idx in high bits
     * We encode MEMBERGET as ABC where C=const-pool-index of field name ---- */
    op_memberget: {
        Value obj=REG(RB);
        /* field name const index is in C (8-bit, up to 255 constants inline) */
        uint8_t ci = RC;
        if(CONST(ci).type!=VAL_OBJ||!CONST(ci).as.obj){REG(RA)=make_null();goto dispatch;}
        const char *field=((ObjString*)CONST(ci).as.obj)->chars;
        if(obj.type!=VAL_OBJ||!obj.as.obj){REG(RA)=make_null();goto dispatch;}
        switch(obj.as.obj->type){
            case OBJ_INSTANCE: REG(RA)=instance_get_field((ObjInstance*)obj.as.obj,field); break;
            case OBJ_LIST:     REG(RA)=!strcmp(field,"length")?make_int(((ObjList*)obj.as.obj)->count):make_null(); break;
            case OBJ_DICT:     REG(RA)=!strcmp(field,"length")?make_int(((ObjDict*)obj.as.obj)->entry_count):make_null(); break;
            case OBJ_STRING:   REG(RA)=!strcmp(field,"length")?make_int(((ObjString*)obj.as.obj)->length):make_null(); break;
            default: REG(RA)=make_null(); break;
        }
        goto dispatch;
    }
    op_memberset: {
        /* A=obj, B=value, C=const-idx of field name */
        Value obj=REG(RA);
        uint8_t ci = RC;
        if(CONST(ci).type!=VAL_OBJ||!CONST(ci).as.obj) goto dispatch;
        const char *field=((ObjString*)CONST(ci).as.obj)->chars;
        if(obj.type==VAL_OBJ&&obj.as.obj&&obj.as.obj->type==OBJ_INSTANCE)
            instance_set_field((ObjInstance*)obj.as.obj, field, REG(RB));
        goto dispatch;
    }

    /* ---- INVOKE: A=dest, B=obj, C=nargs; method-name const in A's slot+1
     * Encoding: method const index packed into imm (we store it as BX of the next word —
     * simple convention: compiler emits LOADK A+1, methodname before INVOKE) ---- */
    op_invoke: {
        /* Convention: A=dest, B=obj-reg, C=nargs
         * Method name string is pre-loaded into register RA+1 by the compiler */
        Value obj = REG(RB);
        int nargs = (int)RC;
        Value method_val = REG(RA + 1);
        if(method_val.type!=VAL_OBJ||!method_val.as.obj){REG(RA)=make_null();goto dispatch;}
        const char *mname=((ObjString*)method_val.as.obj)->chars;
        /* collect args into temporary buffer (stack scratch, fallback to heap) */
        Value scratch[256];
        Value *argv = (nargs > 0 && nargs <= 256) ? scratch : (nargs > 0 ? malloc(sizeof(Value)*nargs) : NULL);
        /* Args start at RB+2; RB+1 holds the method name (loaded by compiler) */
        for(int i=0;i<nargs;i++) argv[i]=REG(RB+2+i);
        Value result=make_null(); bool handled=false;
        if(obj.type==VAL_OBJ&&obj.as.obj){
            switch(obj.as.obj->type){
                case OBJ_LIST: handled=vm_invoke_list(vm,(ObjList*)obj.as.obj,mname,argv,nargs,&result); break;
                case OBJ_DICT: handled=vm_invoke_dict(vm,(ObjDict*)obj.as.obj,mname,argv,nargs,&result); break;
                case OBJ_INSTANCE: {
                    ObjInstance *obj_inst=(ObjInstance*)obj.as.obj;
                    for(int mi=0;mi<obj_inst->method_count&&!handled;mi++){
                        ObjFunction *mf=obj_inst->methods[mi];
                        if(strcmp(mf->name,mname)==0){
                            if(mf->is_native){ result=mf->native_fn(vm,argv,nargs); handled=true; }
                            else if(mf->chunk){
                                int rr=(int)RA;
                                Value saved2[256]; int sn2=nargs<255?nargs:255;
                                for(int i=0;i<sn2;i++) saved2[i]=argv[i];
                                if(push_frame(vm,mf->chunk,rr)!=VM_OK){if(argv!=scratch)free(argv);return VM_ERROR;}
                                FRAME.regs[0]=obj; /* self */
                                for(int i=0;i<sn2;i++) FRAME.regs[i+1]=saved2[i];
                                FRAME.has_self=true; FRAME.self_val=obj;
                                handled=true; goto dispatch;
                            }
                        }
                    }
                    break;
                }
                default: break;
            }
        }
        if (argv != scratch) free(argv);
        if(!handled) fprintf(stderr,"vm: unknown method '%s'\n",mname);
        REG(RA)=result;
        goto dispatch;
    }

    /* ---- Exceptions ---- */
    op_throw: {
        Value exc = REG(RA);
        vm->last_exception = exc;
        while (vm->try_stack) {
            TryFrame *tf = vm->try_stack;
            vm->try_stack = tf->next;
            while (vm->frame_count > tf->frame_depth) {
                vm->frame_count--;
            }
            if (vm->frame_count > 0) {
                FRAME.regs[tf->exc_reg] = exc;
                IP = tf->catch_ip;
                free(tf);
                goto dispatch;
            }
            free(tf);
        }
        vm->frame_count = 0;
        return VM_EXCEPTION;
    }

    op_try: {
        TryFrame *tf = malloc(sizeof(TryFrame));
        tf->catch_ip = IP + SBX;
        tf->exc_reg = RA;
        tf->frame_depth = vm->frame_count;
        tf->next = vm->try_stack;
        vm->try_stack = tf;
        goto dispatch;
    }

    op_endtry: {
        if (vm->try_stack) {
            TryFrame *tf = vm->try_stack;
            vm->try_stack = tf->next;
            free(tf);
        }
        goto dispatch;
    }

    op_closure: {
        Value fn_val = CONST(BX);
        if (fn_val.type != VAL_OBJ || fn_val.as.obj->type != OBJ_FUNCTION) {
            REG(RA) = make_null(); goto dispatch;
        }
        ObjFunction *fn = (ObjFunction*)fn_val.as.obj;
        ObjClosure *cl = new_closure(fn);
        for (int i = 0; i < fn->upvalue_count; i++) {
            uint8_t idx = fn->upvalue_descriptors[i].index;
            bool is_local = fn->upvalue_descriptors[i].is_local;
            if (is_local) {
                cl->upvalues[i] = capture_upvalue(vm, &FRAME.regs[idx]);
            } else {
                cl->upvalues[i] = FRAME.upvalues[idx];
                if (cl->upvalues[i]) retain_obj((Object*)cl->upvalues[i]);
            }
        }
        REG(RA) = make_obj((Object*)cl);
        goto dispatch;
    }

    op_getupval: {
        ObjUpvalue *uv = FRAME.upvalues[BX];
        if (uv) REG(RA) = *uv->location;
        else REG(RA) = make_null();
        goto dispatch;
    }

    op_setupval: {
        ObjUpvalue *uv = FRAME.upvalues[BX];
        if (uv) *uv->location = REG(RA);
        goto dispatch;
    }

    op_super:
        /* stub — V2 */ goto dispatch;

    op_halt: vm->frame_count=0; return VM_OK;

    }
    return VM_OK;

#undef FRAME
#undef CHUNK
#undef IP
#undef RA
#undef RB
#undef RC
#undef BX
#undef SBX
#undef REG
#undef CONST
#undef KSTR
}
