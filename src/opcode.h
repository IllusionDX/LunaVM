/* Luna Register VM Opcode Definitions.
 *
 * All instructions are a fixed 4 bytes (32-bit uint32_t).
 * 8 instructions fit per 64-byte CPU cache line.
 *
 * Two encodings share the same 32-bit word:
 *
 *   ABC   (3-register ops)
 *     bits  0-6  : opcode  (7 bits, up to 128 opcodes)
 *     bits  7-14 : A       (8 bits, destination register)
 *     bits 15-22 : B       (8 bits, source register 1)
 *     bits 23-31 : C       (9 bits, source register 2 / small immediate)
 *
 *   ABx   (reg + unsigned 16-bit immediate)
 *     bits  0-6  : opcode
 *     bits  7-14 : A
 *     bits 15-31 : Bx      (17 bits unsigned — constant index, count, …)
 *
 *   AsBx  (reg + signed 17-bit offset)
 *     bits  0-6  : opcode
 *     bits  7-14 : A
 *     bits 15-31 : sBx     (17 bits signed, biased — jump offsets)
 *
 * Operand C uses Lua's RK format: C < 256 selects register C,
 * C >= 256 selects constant pool index C-256 (limited to 0..255).
 * Arithmetic and comparison ops read C through this scheme.
 */

#ifndef LUNA_OPCODE_H
#define LUNA_OPCODE_H

#include <stdint.h>

/* ---- Encoding / Decoding macros ---- */

/* ABC format: opcode 7 bits, A 8 bits, B 8 bits, C 9 bits (RK) */
#define ENCODE_ABC(op, a, b, c) \
    ((uint32_t)(op) | ((uint32_t)(a) << 7) | ((uint32_t)(b) << 15) | ((uint32_t)(c) << 23))

/* ABx format (unsigned 17-bit immediate) */
#define ENCODE_ABx(op, a, bx) \
    ((uint32_t)(op) | ((uint32_t)(a) << 7) | ((uint32_t)(uint32_t)(bx) << 15))

/* AsBx format (signed 17-bit immediate, biased by 65535) */
#define SBIAS 65535
#define ENCODE_AsBx(op, a, sbx) \
    ((uint32_t)(op) | ((uint32_t)(a) << 7) | ((uint32_t)(uint32_t)((sbx) + SBIAS) << 15))

/* Field extraction */
#define DECODE_OP(inst)  ((uint8_t) ((inst)       & 0x7F))
#define DECODE_A(inst)   ((uint8_t) (((inst) >> 7)  & 0xFF))
#define DECODE_B(inst)   ((uint8_t) (((inst) >> 15) & 0xFF))
#define DECODE_C(inst)   ((uint16_t)(((inst) >> 23) & 0x1FF))
#define DECODE_Bx(inst)  ((uint32_t)(((inst) >> 15) & 0x1FFFF))
#define DECODE_sBx(inst) ((int32_t)(((inst) >> 15) & 0x1FFFF) - SBIAS)

/* RK operand encoding: 0..255 = register, 256..511 = const index 0..255 */
#define RK_REG_SHIFT   23
#define RK_CONST_FLAG  (1u << 8)
#define RK_REG(i)      ((uint32_t)(i))
#define RK_CONST(i)    (RK_CONST_FLAG | (uint32_t)(i))
#define IS_RK_CONST(rk) ((rk) & RK_CONST_FLAG)
#define RK_INDEX(rk)   ((uint16_t)((rk) & 0xFF))
#define DECODE_RK_C(inst) ((uint16_t)(((inst) >> 23) & 0x1FF))

/* ---- Opcode enum ---- */

typedef enum {
    /* ---- Constants & Loading ---- */
    OP_LOADK,       /* ABx  : A = constants[Bx]                           */
    OP_LOADNULL,    /* ABC  : A = null  (B, C unused)                    */
    OP_LOADTRUE,    /* ABC  : A = true  (B, C unused)                    */
    OP_LOADFALSE,   /* ABC  : A = false (B, C unused)                    */
    OP_LOADI,       /* AsBx : A = (int)sBx                                */
    OP_MOVE,        /* ABC  : A = B                                        */

    /* ---- Register Ops ---- */
    OP_SWAP,        /* ABC  : swap(A, B)                                   */

    /* ---- Arithmetic (ABC: A = B op C) ---- */
    OP_ADD,         /* ABC  : A = B + C                                    */
    OP_SUB,         /* ABC  : A = B - C                                    */
    OP_MUL,         /* ABC  : A = B * C                                    */
    OP_DIV,         /* ABC  : A = B / C                                    */
    OP_MOD,         /* ABC  : A = B % C                                    */
    OP_IDIV,        /* ABC  : A = B // C  (floor division)                 */
    OP_POW,         /* ABC  : A = B ** C  (power)                          */
    OP_NEG,         /* ABC  : A = -B                                       */
    OP_BAND,        /* ABC  : A = B & C                                    */
    OP_BOR,         /* ABC  : A = B | C                                    */
    OP_BXOR,        /* ABC  : A = B ^ C                                    */
    OP_BNOT,        /* ABC  : A = ~B                                       */
    OP_SHL,         /* ABC  : A = B << C                                   */
    OP_SHR,         /* ABC  : A = B >> C                                   */

    /* ---- Integer-immediate arithmetic (ABC: A = B op (int8_t)C) ---- */
    OP_ADDI,        /* ABC  : A = B + (int8_t)C  (arithmetic fast path)  */
    OP_SUBI,        /* ABC  : A = B - (int8_t)C  (arithmetic fast path)  */

    /* ---- Comparison (ABC: A = B cmp C, result is bool) ---- */
    OP_EQ,          /* ABC  : A = (B == C)                                 */
    OP_NE,          /* ABC  : A = (B != C)                                 */
    OP_LT,          /* ABC  : A = (B < C)                                  */
    OP_LE,          /* ABC  : A = (B <= C)                                 */
    OP_GT,          /* ABC  : A = (B > C)                                  */
    OP_GE,          /* ABC  : A = (B >= C)                                 */
    OP_IN,          /* ABC  : A = (B in C)                                 */

    /* ---- Logical ---- */
    OP_NOT,         /* ABC  : A = not B                                    */

    /* ---- Control Flow ---- */
    OP_JMP,         /* AsBx : PC += sBx                                   */
    OP_JZ,          /* AsBx : if !A then PC += sBx                        */
    OP_JNZ,         /* AsBx : if  A then PC += sBx                        */
    OP_JNIL,        /* AsBx : if A == null then PC += sBx                 */

    /* ---- Functions ---- */
    OP_CALL,        /* ABC  : A = call B(args B+1..B+C)                   */
    OP_RET,         /* ABC  : return A (B, C unused)                     */
    OP_LEAVE,       /* ABC  : deallocate locals, restore frame (A,B,C unused) */
    OP_CLOSURE,     /* ABx  : A = closure(constants[Bx])                  */

    /* ---- Globals ---- */
    OP_GETGLOBAL,   /* ABx  : A = globals[constants[Bx]]                  */
    OP_SETGLOBAL,   /* ABx  : globals[constants[Bx]] = A                  */

    /* ---- Upvalues ---- */
    OP_GETUPVAL,    /* ABx  : A = upvalues[Bx]                            */
    OP_SETUPVAL,    /* ABx  : upvalues[Bx] = A                            */

    /* ---- Object Ops ---- */
    OP_NEW,         /* ABx  : A = new Instance(class=constants[Bx])       */
    OP_NEWDICT,     /* ABC  : A = {} (empty dict) (B, C unused)          */
    OP_NEWLIST,     /* ABx  : A = [] pre-sized to Bx elements             */
    OP_LISTAPPEND,  /* ABC  : A.append(B)                                 */
    
    OP_GETITER,     /* ABC  : init iter state at A, from object B         */
    OP_FORITER,     /* AsBx : generic iter: next elem in A+2. If iter(A) has next, PC+=sBx */

    OP_INDEXGET,    /* ABC  : A = B[C]                                     */
    OP_INDEXSET,    /* ABC  : A[B] = C                                     */
    OP_SLICE,       /* ABC  : A = slice(B, B+1..B+C)  C=0..3, null=omit  */
    OP_MEMBERGET,   /* ABC  : A = B.field  (field = constants[C])         */
    OP_MEMBERSET,   /* ABC  : A.field = B  (field = constants[C])         */
    OP_GETFIELD,    /* ABC  : A = B->fields[C] (slot-based field get)     */
    OP_SETFIELD,    /* ABC  : A->fields[C] = B (slot-based field set)     */
    /* Note: MEMBERGET/SET use ABC where A=dest/obj, B=obj/val, C=const-index. 
     * GETFIELD/SETFIELD use C as the direct field slot index (0-255).
     * For larger pools, use LOADK + INDEXGET pattern.             */

    OP_INVOKE,      /* ABC  : A = B.method(nargs=C), method-name in const[A+1 slot] */
    OP_SUPER,       /* ABC  : A = super.method(nargs=C)                   */

    /* ---- Exceptions ---- */
    OP_THROW,       /* ABC  : throw A (B, C unused)                      */
    OP_TRY,         /* AsBx : push try, catch at PC+sBx                   */
    OP_ENDTRY,      /* ABC  : pop try frame (A,B,C unused)               */

    /* ---- Keyword-argument prefix ----
     * OP_KW_PREFIX is emitted immediately before OP_CALL only when a call
     * carries keyword arguments.  It carries the keyword count (A) and a
     * 16-bit constant-pool index (Bx) to a static tuple of argument names.
     * The plain positional OP_CALL never pays for this: it just reads the
     * vm->next_call_kw_* fields (a register compare on the fast path). */
    OP_KW_PREFIX,   /* ABx  : A = kw_count, Bx = kw_names const index       */

    /* ---- Safe access (null on missing, no throw) ---- */

    /* ---- Module import ---- */
    OP_IMPORT,      /* ABx  : A = import(module=constants[Bx])              */

    /* ---- Safe constructor invocation ---- */
    OP_HALT,        /* ABC  : stop VM (A,B,C unused)                     */

    /* ---- Compare-and-branch (ABC: A=left, B=right, C=offset) ---- */
    OP_LT_JZ,       /* ABC  : if !(A < B) then IP += C                  */
    OP_LE_JZ,       /* ABC  : if !(A <= B) then IP += C                 */
    OP_GT_JZ,       /* ABC  : if !(A > B) then IP += C                  */
    OP_GE_JZ,       /* ABC  : if !(A >= B) then IP += C                 */
    OP_EQ_JZ,       /* ABC  : if !(A == B) then IP += C                 */
    OP_NE_JZ,       /* ABC  : if !(A != B) then IP += C                 */

    /* ---- Compare-and-branch with immediate (ABC: A=left, B=(int8_t)imm, C=offset) ---- */
    OP_LT_JZ_IMM,   /* ABC  : if !(A < (int8_t)B) then IP += C          */
    OP_LE_JZ_IMM,   /* ABC  : if !(A <= (int8_t)B) then IP += C         */
    OP_GT_JZ_IMM,   /* ABC  : if !(A > (int8_t)B) then IP += C          */
    OP_GE_JZ_IMM,   /* ABC  : if !(A >= (int8_t)B) then IP += C         */
    OP_EQ_JZ_IMM,   /* ABC  : if !(A == (int8_t)B) then IP += C         */
    OP_NE_JZ_IMM,   /* ABC  : if !(A != (int8_t)B) then IP += C         */

    OP_FORLOOP,     /* AsBx : numeric range loop. A=index, A+1=limit, A+2=step.
                       A += step; if (step>=0 ? A<limit : A>limit) PC += sBx */
    OP_FORPREP,     /* AsBx : numeric range prepare. A=index, A+1=limit, A+2=step.
                       If the range is already exhausted (or step==0) PC += sBx
                       to skip the loop; otherwise fall through into the body. */

    /* ---- Identity (Python `is` / `is not`): raw bit/pointer equality, no
     * deep comparison (ABC: A = (B is C), result is bool) ---- */
    OP_RAW_EQ,      /* ABC  : A = (B is C)                                */
    OP_RAW_NE,      /* ABC  : A = (B is not C)                            */

    OP_COUNT        /* sentinel — total opcode count                       */
} OpCode;

#endif /* LUNA_OPCODE_H */
