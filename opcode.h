/* Luna Register VM Opcode Definitions.
 *
 * All instructions are a fixed 4 bytes (32-bit uint32_t).
 * 8 instructions fit per 64-byte CPU cache line.
 *
 * Three encodings share the same 32-bit word:
 *
 *   ABC   (3-register ops)
 *     bits  0-7  : opcode  (8 bits, up to 256 opcodes)
 *     bits  8-15 : A       (8 bits, destination register)
 *     bits 16-23 : B       (8 bits, source register 1)
 *     bits 24-31 : C       (8 bits, source register 2 / small immediate)
 *
 *   ABx   (reg + unsigned 16-bit immediate)
 *     bits  0-7  : opcode
 *     bits  8-15 : A
 *     bits 16-31 : Bx      (16 bits unsigned — constant index, count, …)
 *
 *   AsBx  (reg + signed 16-bit offset)
 *     bits  0-7  : opcode
 *     bits  8-15 : A
 *     bits 16-31 : sBx     (16 bits signed — jump offsets)
 */

#ifndef LUNA_OPCODE_H
#define LUNA_OPCODE_H

#include <stdint.h>

/* ---- Encoding / Decoding macros ---- */

/* ABC format */
#define ENCODE_ABC(op, a, b, c) \
    ((uint32_t)(op) | ((uint32_t)(a) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 24))

/* ABx format (unsigned 16-bit immediate) */
#define ENCODE_ABx(op, a, bx) \
    ((uint32_t)(op) | ((uint32_t)(a) << 8) | ((uint32_t)(uint16_t)(bx) << 16))

/* AsBx format (signed 16-bit immediate, biased by 32767) */
#define SBIAS 32767
#define ENCODE_AsBx(op, a, sbx) \
    ((uint32_t)(op) | ((uint32_t)(a) << 8) | ((uint32_t)(uint16_t)((sbx) + SBIAS) << 16))

/* Field extraction */
#define DECODE_OP(inst)  ((uint8_t) ((inst)       & 0xFF))
#define DECODE_A(inst)   ((uint8_t) (((inst) >> 8)  & 0xFF))
#define DECODE_B(inst)   ((uint8_t) (((inst) >> 16) & 0xFF))
#define DECODE_C(inst)   ((uint8_t) (((inst) >> 24) & 0xFF))
#define DECODE_Bx(inst)  ((uint16_t)(((inst) >> 16) & 0xFFFF))
#define DECODE_sBx(inst) ((int32_t)((uint16_t)(((inst) >> 16) & 0xFFFF)) - SBIAS)

/* ---- Opcode enum ---- */

typedef enum {
    /* ---- Constants & Loading ---- */
    OP_LOADK,       /* ABx  : A = constants[Bx]                           */
    OP_LOADNULL,    /* A    : A = null                                     */
    OP_LOADTRUE,    /* A    : A = true                                     */
    OP_LOADFALSE,   /* A    : A = false                                    */
    OP_LOADI,       /* AsBx : A = (int)sBx                                */
    OP_MOVE,        /* ABC  : A = B                                        */

    /* ---- Register Ops ---- */
    OP_COPY,        /* ABC  : A = copy(B)  (bump refcount)                */
    OP_SWAP,        /* ABC  : swap(A, B)                                   */

    /* ---- Arithmetic (ABC: A = B op C) ---- */
    OP_ADD,         /* ABC  : A = B + C                                    */
    OP_SUB,         /* ABC  : A = B - C                                    */
    OP_MUL,         /* ABC  : A = B * C                                    */
    OP_DIV,         /* ABC  : A = B / C                                    */
    OP_MOD,         /* ABC  : A = B % C                                    */
    OP_NEG,         /* ABC  : A = -B                                       */
    OP_BAND,        /* ABC  : A = B & C                                    */
    OP_BOR,         /* ABC  : A = B | C                                    */
    OP_BXOR,        /* ABC  : A = B ^ C                                    */
    OP_BNOT,        /* ABC  : A = ~B                                       */
    OP_SHL,         /* ABC  : A = B << C                                   */
    OP_SHR,         /* ABC  : A = B >> C                                   */

    /* ---- Comparison (ABC: A = B cmp C, result is bool) ---- */
    OP_EQ,          /* ABC  : A = (B == C)                                 */
    OP_NE,          /* ABC  : A = (B != C)                                 */
    OP_LT,          /* ABC  : A = (B < C)                                  */
    OP_LE,          /* ABC  : A = (B <= C)                                 */
    OP_GT,          /* ABC  : A = (B > C)                                  */
    OP_GE,          /* ABC  : A = (B >= C)                                 */

    /* ---- Logical ---- */
    OP_NOT,         /* ABC  : A = not B                                    */

    /* ---- Control Flow ---- */
    OP_JMP,         /* AsBx : PC += sBx                                   */
    OP_JZ,          /* AsBx : if !A then PC += sBx                        */
    OP_JNZ,         /* AsBx : if  A then PC += sBx                        */

    /* ---- Functions ---- */
    OP_CALL,        /* ABC  : A = call B(args B+1..B+C)                   */
    OP_RET,         /* A    : return A                                     */
    OP_ENTER,       /* ABx  : allocate Bx local slots (hint)              */
    OP_LEAVE,       /* ---  : deallocate locals, restore frame             */
    OP_CLOSURE,     /* ABx  : A = closure(constants[Bx])                  */

    /* ---- Globals ---- */
    OP_GETGLOBAL,   /* ABx  : A = globals[constants[Bx]]                  */
    OP_SETGLOBAL,   /* ABx  : globals[constants[Bx]] = A                  */

    /* ---- Upvalues ---- */
    OP_GETUPVAL,    /* ABx  : A = upvalues[Bx]                            */
    OP_SETUPVAL,    /* ABx  : upvalues[Bx] = A                            */

    /* ---- Object Ops ---- */
    OP_NEW,         /* ABx  : A = new Instance(class=constants[Bx])       */
    OP_NEWDICT,     /* A    : A = {} (empty dict)                          */
    OP_NEWLIST,     /* ABx  : A = [] pre-sized to Bx elements             */
    OP_LISTAPPEND,  /* ABC  : A.append(B)                                 */
    
    OP_GETITER,     /* ABC  : init iter state at A, from object B         */
    OP_FORLOOP,     /* AsBx : next elem in A+2. If iter(A) has next, PC+=sBx */

    OP_INDEXGET,    /* ABC  : A = B[C]                                     */
    OP_INDEXSET,    /* ABC  : A[B] = C                                     */
    OP_MEMBERGET,   /* ABC  : A = B.field  (field = constants[C])         */
    OP_MEMBERSET,   /* ABC  : A.field = B  (field = constants[C])         */
    /* Note: MEMBERGET/SET use ABC where A=dest/obj, B=obj/val, C=const-index. 
     * For larger pools, use LOADK + INDEXGET pattern.             */

    OP_INVOKE,      /* ABC  : A = B.method(nargs=C), method-name in const[A+1 slot] */
    OP_SUPER,       /* ABC  : A = super.method(nargs=C)                   */

    /* ---- Exceptions ---- */
    OP_THROW,       /* A    : throw A                                      */
    OP_TRY,         /* AsBx : push try, catch at PC+sBx                   */
    OP_ENDTRY,      /* ---  : pop try frame                                */

    OP_HALT,        /* ---  : stop VM                                      */

    OP_COUNT        /* sentinel — total opcode count                       */
} OpCode;

#endif /* LUNA_OPCODE_H */
