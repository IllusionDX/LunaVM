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
        char *ls=value_to_string(L), *rs=value_to_string(R);
        int len=(int)strlen(ls)+(int)strlen(rs);
        char *buf=malloc(len+1); strcpy(buf,ls); strcat(buf,rs);
        ObjString *s=new_string(buf,len); free(ls);free(rs);free(buf);
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

void vm_free(VM *vm) {
    for (int i=0;i<VM_GLOBAL_BUCKETS;i++) {
        GlobalEntry *e=vm->globals[i];
        while(e){ GlobalEntry *nx=e->next; release_obj(e->value.type==VAL_OBJ?e->value.as.obj:NULL); free(e->name);free(e);e=nx; }
        vm->globals[i]=NULL;
    }
    Object *o=vm->objects;
    while(o){ Object *nx=o->next; free_object(o); o=nx; }
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

static VMResult push_frame(VM *vm, Chunk *chunk, int ret_reg) {
    if (vm->frame_count >= VM_MAX_FRAMES) { fprintf(stderr,"vm: stack overflow\n"); return VM_ERROR; }
    CallFrame *f = &vm->frames[vm->frame_count++];
    f->chunk   = chunk;
    f->ip      = 0;
    f->ret_reg = ret_reg;
    f->has_self= false;
    f->self_val= make_null();
    for (int i=0;i<VM_MAX_REGISTERS;i++) f->regs[i]=make_null();
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
            if (vm->frame_count == 1) { vm->frame_count=0; return VM_OK; }
            int rr = FRAME.ret_reg;
            vm->frame_count--;
            if (rr >= 0) REG(rr) = make_null();
            goto dispatch;
        }

        uint32_t inst = CHUNK->code[IP++];
        OpCode op = (OpCode)DECODE_OP(inst);

        switch (op) {

        /* ---- Load / Move ---- */
        case OP_LOADK:     REG(RA) = CONST(BX);        break;
        case OP_LOADNULL:  REG(RA) = make_null();       break;
        case OP_LOADTRUE:  REG(RA) = make_bool(true);   break;
        case OP_LOADFALSE: REG(RA) = make_bool(false);  break;
        case OP_LOADI:     REG(RA) = make_int(SBX);     break;
        case OP_MOVE:      REG(RA) = REG(RB);           break;
        case OP_COPY:      {
            Value v=REG(RB);
            if(v.type==VAL_OBJ&&v.as.obj) retain_obj(v.as.obj);
            REG(RA)=v; break;
        }
        case OP_SWAP:      { Value t=REG(RA); REG(RA)=REG(RB); REG(RB)=t; break; }

        /* ---- Globals ---- */
        case OP_GETGLOBAL: {
            if(CONST(BX).type!=VAL_OBJ||!CONST(BX).as.obj){REG(RA)=make_null();break;}
            const char *nm=KSTR(BX);
            Value out;
            if(!vm_get_global(vm,nm,&out)){fprintf(stderr,"vm: undefined '%s'\n",nm);REG(RA)=make_null();}
            else REG(RA)=out;
            break;
        }
        case OP_SETGLOBAL: {
            if(CONST(BX).type!=VAL_OBJ||!CONST(BX).as.obj) break;
            vm_set_global(vm, KSTR(BX), REG(RA), false);
            break;
        }

        /* ---- Arithmetic ---- */
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            REG(RA) = do_arith(REG(RB), REG(RC), op); break;
        case OP_NEG: {
            Value v=REG(RB);
            if(v.type==VAL_INT)    { REG(RA)=make_int(-v.as.integer);    break; }
            if(v.type==VAL_FLOAT)  { REG(RA)=make_float(-v.as.float_val); break; }
            if(v.type==VAL_DOUBLE) { REG(RA)=make_double(-v.as.double_val);break; }
            REG(RA)=make_null(); break;
        }
        case OP_BAND: REG(RA)=make_int(to_i64(REG(RB))&to_i64(REG(RC)));  break;
        case OP_BOR:  REG(RA)=make_int(to_i64(REG(RB))|to_i64(REG(RC)));  break;
        case OP_BXOR: REG(RA)=make_int(to_i64(REG(RB))^to_i64(REG(RC)));  break;
        case OP_SHL:  REG(RA)=make_int(to_i64(REG(RB))<<to_i64(REG(RC))); break;
        case OP_SHR:  REG(RA)=make_int(to_i64(REG(RB))>>to_i64(REG(RC))); break;
        case OP_BNOT: REG(RA)=make_int(~to_i64(REG(RB))); break;

        /* ---- Comparison / Logical ---- */
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
            REG(RA) = do_cmp(REG(RB), REG(RC), op); break;
        case OP_NOT: REG(RA) = make_bool(!is_truthy(REG(RB))); break;

        /* ---- Control Flow ---- */
        case OP_JMP:  IP += SBX; break;
        case OP_JZ:   if(!is_truthy(REG(RA))) IP += SBX; break;
        case OP_JNZ:  if( is_truthy(REG(RA))) IP += SBX; break;

        /* ---- CALL ---- */
        case OP_CALL: {
            /* A=dest, B=fn-reg, C=nargs; args in B+1..B+C */
            Value fn_val = REG(RB);
            int   nargs  = (int)RC;
            if (fn_val.type!=VAL_OBJ||!fn_val.as.obj) {
                fprintf(stderr,"vm: call non-function\n"); REG(RA)=make_null(); break;
            }
            ObjFunction *fn = (ObjFunction *)fn_val.as.obj;
            if (fn->obj.type != OBJ_FUNCTION) {
                fprintf(stderr,"vm: not callable\n"); REG(RA)=make_null(); break;
            }
            if (fn->is_native) {
                /* collect args into temporary buffer */
                Value *argv = nargs > 0 ? malloc(sizeof(Value)*nargs) : NULL;
                for (int i=0;i<nargs;i++) argv[i]=REG(RB+1+i);
                Value res = fn->native_fn(vm, argv, nargs);
                free(argv);
                REG(RA) = res;
                break;
            }
            /* Luna function — push new frame */
            if (!fn->chunk) { fprintf(stderr,"vm: fn '%s' no bytecode\n", fn->name); REG(RA)=make_null(); break; }
            int ret_dest = (int)RA;
            /* save args from current frame before push */
            Value saved[256]; int sn = nargs < 256 ? nargs : 255;
            for (int i=0;i<sn;i++) saved[i]=REG(RB+1+i);
            if (push_frame(vm, fn->chunk, ret_dest) != VM_OK) return VM_ERROR;
            for (int i=0;i<sn;i++) FRAME.regs[i]=saved[i];
            goto dispatch;
        }

        /* ---- RET ---- */
        case OP_RET: {
            Value ret_val = REG(RA);
            if (vm->frame_count <= 1) { vm->frame_count=0; return VM_OK; }
            int rr = FRAME.ret_reg;
            vm->frame_count--;
            if (rr >= 0) REG(rr) = ret_val;
            goto dispatch;
        }

        case OP_ENTER: case OP_LEAVE: break; /* hints, no-op in register VM */

        /* ---- Collections ---- */
        case OP_NEWLIST: REG(RA) = make_obj((Object*)new_list()); break;
        case OP_NEWDICT: REG(RA) = make_obj((Object*)new_dict()); break;

        /* ---- NEW instance ---- */
        case OP_NEW: {
            if(CONST(BX).type!=VAL_OBJ||!CONST(BX).as.obj){REG(RA)=make_null();break;}
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
            break;
        }

        /* ---- Index access ---- */
        case OP_INDEXGET: {
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
            break;
        }
        case OP_INDEXSET: {
            /* A=obj, B=key, C=value */
            Value obj=REG(RA), key=REG(RB), val=REG(RC);
            if(obj.type==VAL_OBJ&&obj.as.obj){
                if(obj.as.obj->type==OBJ_LIST&&key.type==VAL_INT) list_set((ObjList*)obj.as.obj,(int)key.as.integer,val);
                else if(obj.as.obj->type==OBJ_DICT) dict_set((ObjDict*)obj.as.obj,key,val);
            }
            break;
        }

        /* ---- Member access: A=dest, B=obj, Bx has field const idx in high bits
         * We encode MEMBERGET as ABC where C=const-pool-index of field name ---- */
        case OP_MEMBERGET: {
            Value obj=REG(RB);
            /* field name const index is in C (8-bit, up to 255 constants inline) */
            uint8_t ci = RC;
            if(CONST(ci).type!=VAL_OBJ||!CONST(ci).as.obj){REG(RA)=make_null();break;}
            const char *field=((ObjString*)CONST(ci).as.obj)->chars;
            if(obj.type!=VAL_OBJ||!obj.as.obj){REG(RA)=make_null();break;}
            switch(obj.as.obj->type){
                case OBJ_INSTANCE: REG(RA)=instance_get_field((ObjInstance*)obj.as.obj,field); break;
                case OBJ_LIST:     REG(RA)=!strcmp(field,"length")?make_int(((ObjList*)obj.as.obj)->count):make_null(); break;
                case OBJ_DICT:     REG(RA)=!strcmp(field,"length")?make_int(((ObjDict*)obj.as.obj)->entry_count):make_null(); break;
                case OBJ_STRING:   REG(RA)=!strcmp(field,"length")?make_int(((ObjString*)obj.as.obj)->length):make_null(); break;
                default: REG(RA)=make_null(); break;
            }
            break;
        }
        case OP_MEMBERSET: {
            /* A=obj, B=value, C=const-idx of field name */
            Value obj=REG(RA);
            uint8_t ci = RC;
            if(CONST(ci).type!=VAL_OBJ||!CONST(ci).as.obj) break;
            const char *field=((ObjString*)CONST(ci).as.obj)->chars;
            if(obj.type==VAL_OBJ&&obj.as.obj&&obj.as.obj->type==OBJ_INSTANCE)
                instance_set_field((ObjInstance*)obj.as.obj, field, REG(RB));
            break;
        }

        /* ---- INVOKE: A=dest, B=obj, C=nargs; method-name const in A's slot+1
         * Encoding: method const index packed into imm (we store it as BX of the next word —
         * simple convention: compiler emits LOADK A+1, methodname before INVOKE) ---- */
        case OP_INVOKE: {
            /* Convention: A=dest, B=obj-reg, C=nargs
             * Method name string is pre-loaded into register RA+1 by the compiler */
            Value obj = REG(RB);
            int nargs = (int)RC;
            Value method_val = REG(RA + 1);
            if(method_val.type!=VAL_OBJ||!method_val.as.obj){REG(RA)=make_null();break;}
            const char *mname=((ObjString*)method_val.as.obj)->chars;
            Value *argv = nargs>0 ? malloc(sizeof(Value)*nargs) : NULL;
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
                            if(strcmp(obj_inst->methods[mi]->name,mname)==0){
                                ObjFunction *mf=obj_inst->methods[mi];
                                if(mf->is_native){ result=mf->native_fn(vm,argv,nargs); handled=true; }
                                else if(mf->chunk){
                                    int rr=(int)RA;
                                    Value saved2[256]; int sn2=nargs<255?nargs:255;
                                    for(int i=0;i<sn2;i++) saved2[i]=argv[i];
                                    free(argv); argv=NULL;
                                    if(push_frame(vm,mf->chunk,rr)!=VM_OK){free(argv);return VM_ERROR;}
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
            free(argv);
            if(!handled) fprintf(stderr,"vm: unknown method '%s'\n",mname);
            REG(RA)=result;
            break;
        }

        /* ---- Exceptions ---- */
        case OP_THROW:
            vm->last_exception=REG(RA);
            vm->frame_count=0;
            return VM_EXCEPTION;

        case OP_TRY: case OP_ENDTRY:
        case OP_CLOSURE: case OP_GETUPVAL: case OP_SETUPVAL: case OP_SUPER:
            /* stubs — V2 */ break;

        case OP_HALT: vm->frame_count=0; return VM_OK;

        default:
            fprintf(stderr,"vm: unknown opcode %d\n", op);
            break;
        }
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
