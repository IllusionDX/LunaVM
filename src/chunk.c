/* chunk.c — Bytecode chunk implementation. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "chunk.h"
#include "value.h"
#include "vm.h"

#define GROW_FACTOR  2
#define INIT_CAP     8

/* ============================================================ */
/* Lifecycle                                                     */
/* ============================================================ */

void chunk_init(Chunk *chunk, const char *name) {
    chunk->code          = NULL;
    chunk->lines         = NULL;
    chunk->count         = 0;
    chunk->capacity      = 0;

    chunk->constants     = NULL;
    chunk->const_count   = 0;
    chunk->const_capacity = 0;
    chunk->global_cache  = NULL;
    chunk->max_registers = 0;

    chunk->name = name ? strdup(name) : strdup("<chunk>");
    chunk->source_path = NULL;
}

void chunk_free(Chunk *chunk) {
    free(chunk->code);
    free(chunk->lines);

    free(chunk->constants);
    free(chunk->global_cache);
    free(chunk->name);
    free(chunk->source_path);

    chunk_init(chunk, NULL);
}

/* ============================================================ */
/* Instruction writing                                           */
/* ============================================================ */

static void ensure_code_capacity(Chunk *chunk) {
    if (chunk->count < chunk->capacity) return;
    int new_cap = chunk->capacity < INIT_CAP
                  ? INIT_CAP
                  : chunk->capacity * GROW_FACTOR;
    chunk->code  = realloc(chunk->code,  sizeof(uint32_t) * new_cap);
    chunk->lines = realloc(chunk->lines, sizeof(int)      * new_cap);
    if (!chunk->code || !chunk->lines) {
        fprintf(stderr, "chunk: out of memory\n");
        exit(1);
    }
    chunk->capacity = new_cap;
}

int chunk_write(Chunk *chunk, uint32_t inst, int line) {
    ensure_code_capacity(chunk);
    int idx = chunk->count++;
    chunk->code[idx]  = inst;
    chunk->lines[idx] = line;
    return idx;
}

int chunk_emit_ABC(Chunk *chunk, int line, OpCode op,
                   uint8_t a, uint8_t b, uint32_t c) {
    return chunk_write(chunk, ENCODE_ABC(op, a, b, c), line);
}

int chunk_emit_ABx(Chunk *chunk, int line, OpCode op,
                   uint8_t a, uint16_t bx) {
    return chunk_write(chunk, ENCODE_ABx(op, a, bx), line);
}

int chunk_emit_AsBx(Chunk *chunk, int line, OpCode op,
                    uint8_t a, int32_t sbx) {
    return chunk_write(chunk, ENCODE_AsBx(op, a, sbx), line);
}

/* ============================================================ */
/* Constant pool                                                  */
/* ============================================================ */

int chunk_add_const(Chunk *chunk, Value value) {
    if (chunk->const_count >= chunk->const_capacity) {
        int new_cap = chunk->const_capacity < INIT_CAP
                      ? INIT_CAP
                      : chunk->const_capacity * GROW_FACTOR;
        chunk->constants = realloc(chunk->constants, sizeof(Value) * new_cap);
        if (!chunk->constants) {
            fprintf(stderr, "chunk: out of memory\n");
            exit(1);
        }
        chunk->const_capacity = new_cap;
    }
    int idx = chunk->const_count++;
    chunk->constants[idx] = value;
    return idx;
}

int chunk_add_string(VM *vm, Chunk *chunk, const char *cstr) {
    return chunk_add_string_len(vm, chunk, cstr, (int)strlen(cstr));
}

int chunk_add_string_len(VM *vm, Chunk *chunk, const char *chars, int length) {
    /* De-duplicate: return existing index if already interned */
    for (int i = 0; i < chunk->const_count; i++) {
        Value v = chunk->constants[i];
        if (IS_OBJ(v) && AS_OBJ(v) && AS_OBJ(v)->type->string_chars) {
            const char *sc = AS_OBJ(v)->type->string_chars(v);
            if (sc && strlen(sc) == (size_t)length && memcmp(sc, chars, (size_t)length) == 0) return i;
        }
    }
    Value obj = vm->frontend->new_string(vm, chars, length);
    int idx = chunk_add_const(chunk, obj);
    return idx;
}

/* ============================================================ */
/* Back-patch helper                                             */
/* ============================================================ */

void chunk_patch_sBx(Chunk *chunk, int at, int32_t sbx) {
    if (at < 0 || at >= chunk->count) {
        fprintf(stderr, "chunk_patch_sBx: index %d out of range\n", at);
        return;
    }
    /* Preserve opcode and A, overwrite bits 15-31 */
    uint32_t inst = chunk->code[at];
    uint32_t preserved = inst & 0x00007FFF;  /* low 15 bits: opcode + A */
    uint32_t new_bx = (uint32_t)(uint32_t)((sbx) + SBIAS);
    chunk->code[at] = preserved | (new_bx << 15);
}

/* Patch the C field (bits 23-31, 9 bits) of a fused compare-and-branch.
   Forward jumps only: offset must fit 0..0x1FF. */
void chunk_patch_jump_c(Chunk *chunk, int at, int32_t offset) {
    if (at < 0 || at >= chunk->count) return;
    assert(offset >= 0 && offset <= 0x1FF && "fused jump offset exceeds 9-bit C field");
    if (offset < 0 || offset > 0x1FF) return;
    uint32_t inst = chunk->code[at];
    chunk->code[at] = (inst & 0x007FFFFF) | ((uint32_t)offset << 23);
}

/* ============================================================ */
/* Disassembly                                                    */
/* ============================================================ */

typedef enum { FMT_ABC, FMT_ABx, FMT_AsBx } OpFormat;

static const char *op_mnemonics[] = {
    "LOADK",   "LOADNULL", "LOADTRUE",  "LOADFALSE",
    "LOADI",   "MOVE",     "SWAP",
    "ADD",     "SUB",      "MUL",       "DIV",
    "MOD",     "IDIV",     "POW",       "NEG",
    "BAND",    "BOR",      "BXOR",      "BNOT",
    "SHL",     "SHR",
    "ADDI",    "SUBI",
    "EQ",      "NE",       "LT",        "LE",
    "GT",      "GE",       "IN",        "NOT",
    "JMP",     "JZ",       "JNZ",       "JNIL",
    "CALL",    "RET",      "LEAVE",
    "CLOSURE", "GETGLOBAL","SETGLOBAL",
    "GETUPVAL","SETUPVAL",
    "NEW",     "NEWDICT",  "NEWLIST",   "LISTAPPEND",
    "GETITER", "FORITER",
    "INDEXGET","INDEXSET", "SLICE",
    "MEMBERGET","MEMBERSET","GETFIELD",  "SETFIELD",
    "INVOKE",    "SUPER",
    "THROW",   "TRY",      "ENDTRY",    "ISINSTANCE",
    "KW_PREFIX",
    "IMPORT",  "HALT",
    "LT_JZ",   "LE_JZ",    "GT_JZ",     "GE_JZ",
    "EQ_JZ",   "NE_JZ",
    "LT_JZ_IMM","LE_JZ_IMM","GT_JZ_IMM","GE_JZ_IMM",
    "EQ_JZ_IMM","NE_JZ_IMM",
    "FORLOOP",
    "FORPREP",
    "RAW_EQ",
    "RAW_NE"
};

static OpFormat op_formats[] = {
    /* LOADK */    FMT_ABx,
    /* LOADNULL */ FMT_ABC,
    /* LOADTRUE */ FMT_ABC,
    /* LOADFALSE*/ FMT_ABC,
    /* LOADI */    FMT_AsBx,
    /* MOVE */     FMT_ABC,
    /* SWAP */     FMT_ABC,
    /* ADD */      FMT_ABC, /* SUB */ FMT_ABC, /* MUL */ FMT_ABC, /* DIV */ FMT_ABC,
    /* MOD */      FMT_ABC, /* IDIV */ FMT_ABC, /* POW */ FMT_ABC, /* NEG */ FMT_ABC,
    /* BAND */     FMT_ABC, /* BOR */ FMT_ABC, /* BXOR */ FMT_ABC,
    /* BNOT */     FMT_ABC, /* SHL */ FMT_ABC, /* SHR */ FMT_ABC,
    /* ADDI */     FMT_ABC, /* SUBI */ FMT_ABC,
    /* EQ */       FMT_ABC, /* NE */ FMT_ABC, /* LT */ FMT_ABC,
    /* LE */       FMT_ABC, /* GT */ FMT_ABC, /* GE */ FMT_ABC,
    /* IN */       FMT_ABC, /* NOT */ FMT_ABC,
    /* JMP */      FMT_AsBx,/* JZ */ FMT_AsBx, /* JNZ */ FMT_AsBx, /* JNIL */ FMT_AsBx,
    /* CALL */     FMT_ABC,
    /* RET */      FMT_ABC,
    /* LEAVE */    FMT_ABC,
    /* CLOSURE */  FMT_ABx,
    /* GETGLOBAL */FMT_ABx,
    /* SETGLOBAL */FMT_ABx,
    /* GETUPVAL */ FMT_ABx,
    /* SETUPVAL */ FMT_ABx,
    /* NEW */      FMT_ABx,
    /* NEWDICT */  FMT_ABC,
    /* NEWLIST */  FMT_ABx,
    /* LISTAPPEND*/FMT_ABC,
    /* GETITER */  FMT_ABC,
    /* FORITER */  FMT_AsBx,
    /* INDEXGET */ FMT_ABC,
    /* INDEXSET */ FMT_ABC,
    /* SLICE */    FMT_ABC,
    /* MEMBERGET */FMT_ABC,
    /* MEMBERSET */FMT_ABC,
    /* GETFIELD */ FMT_ABC,
    /* SETFIELD */ FMT_ABC,
    /* INVOKE */   FMT_ABC,
    /* SUPER */    FMT_ABC,
    /* THROW */    FMT_ABC,
    /* TRY */      FMT_AsBx,
    /* ENDTRY */   FMT_ABC,
    /* ISINSTANCE*/FMT_ABC,
    /* KW_PREFIX */FMT_ABx,
    /* IMPORT */   FMT_ABx,
    /* HALT */     FMT_ABC,
    /* LT_JZ */    FMT_ABC,
    /* LE_JZ */    FMT_ABC,
    /* GT_JZ */    FMT_ABC,
    /* GE_JZ */    FMT_ABC,
    /* EQ_JZ */    FMT_ABC,
    /* NE_JZ */    FMT_ABC,
    /* LT_JZ_IMM */FMT_ABC,
    /* LE_JZ_IMM */FMT_ABC,
    /* GT_JZ_IMM */FMT_ABC,
    /* GE_JZ_IMM */FMT_ABC,
    /* EQ_JZ_IMM */FMT_ABC,
    /* NE_JZ_IMM */FMT_ABC,
    /* FORLOOP */  FMT_AsBx,
    /* FORPREP */  FMT_AsBx,
    /* RAW_EQ */   FMT_ABC,
    /* RAW_NE */   FMT_ABC
};

void chunk_disassemble(Chunk *chunk) {
    static int depth = 0;
    for (int d = 0; d < depth; d++) printf("  ");
    printf("== %s ==\n", chunk->name ? chunk->name : "<chunk>");
    for (int d = 0; d < depth; d++) printf("  ");
    printf("  max_registers: %d  constants: %d  instructions: %d\n",
           chunk->max_registers, chunk->const_count, chunk->count);

    for (int i = 0; i < chunk->count; i++) {
        uint32_t inst = chunk->code[i];
        uint8_t opcode = DECODE_OP(inst);
        uint8_t a      = DECODE_A(inst);
        uint8_t b      = DECODE_B(inst);
        uint32_t c     = DECODE_C(inst);
        uint32_t bx    = DECODE_Bx(inst);
        int32_t sbx    = DECODE_sBx(inst);

        const char *mnem = opcode < OP_COUNT ? op_mnemonics[opcode] : "???";
        OpFormat fmt = opcode < OP_COUNT ? op_formats[opcode] : FMT_ABC;

        for (int d = 0; d < depth; d++) printf("  ");
        printf("  %4d  %-15s", i, mnem);

        /* Special: ADDI / SUBI use C as RK-immediate, B as source register */
        if (opcode == OP_ADDI) {
            printf(" R%d R%d %+d", a, b, (int8_t)c);
        } else if (opcode == OP_SUBI) {
            printf(" R%d R%d %+d", a, b, -(int8_t)c);
        }
        /* Special: LT_JZ / LE_JZ / GT_JZ / GE_JZ use A=left, B=right, C=offset */
        else if (opcode == OP_LT_JZ || opcode == OP_LE_JZ ||
                 opcode == OP_GT_JZ || opcode == OP_GE_JZ) {
            printf(" R%d R%d +%d", a, b, c);
        }
        /* Special: *JZ_IMM use A=left, B=(int8_t)imm, C=offset */
        else if (opcode == OP_LT_JZ_IMM || opcode == OP_LE_JZ_IMM ||
                 opcode == OP_GT_JZ_IMM || opcode == OP_GE_JZ_IMM ||
                 opcode == OP_EQ_JZ_IMM || opcode == OP_NE_JZ_IMM) {
            printf(" R%d %+d +%d", a, (int8_t)b, c);
        }
        else {
            switch (fmt) {
                case FMT_ABC:
                    if (IS_RK_CONST(c)) {
                        Value cv = chunk->constants[RK_INDEX(c)];
                        if (IS_INT(cv))
                            printf(" R%d R%d %lld", a, b, (long long)AS_INT(cv));
                        else if (IS_DOUBLE(cv))
                            printf(" R%d R%d %g", a, b, AS_DOUBLE(cv));
                        else if (IS_OBJ(cv)) {
                            char *s = value_to_string(cv);
                            printf(" R%d R%d %s", a, b, s);
                            free(s);
                        } else
                            printf(" R%d R%d const[%d]", a, b, RK_INDEX(c));
                    } else {
                        printf(" R%d R%d R%d", a, b, c);
                    }
                    break;
                case FMT_ABx:
                    printf(" R%d %d", a, bx);
                    break;
                case FMT_AsBx:
                    printf(" R%d %+d", a, sbx);
                    break;
            }
        }

        printf("\n");
    }

    /* Recurse into function constants (via vtable: native callables have no chunk) */
    for (int i = 0; i < chunk->const_count; i++) {
        Value v = chunk->constants[i];
        if (IS_OBJ(v) && AS_OBJ(v) && AS_OBJ(v)->type->get_chunk) {
            Chunk *c = AS_OBJ(v)->type->get_chunk(v);
            if (c) {
                depth++;
                chunk_disassemble(c);
                depth--;
            }
        }
    }
}
