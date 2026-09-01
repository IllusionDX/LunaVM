/* chunk.h — Bytecode chunk (uint32_t instruction array + constant pool).
 *
 * Each instruction is a 32-bit word (see opcode.h for encoding).
 * The constant pool holds Values referenced by LOADK and other ABx ops.
 * A parallel lines[] array maps each instruction to its source line.
 */

#ifndef LUNA_CHUNK_H
#define LUNA_CHUNK_H

#include <stdint.h>
#include "opcode.h"
#include "value.h"

/* Forward declaration for inline cache */
struct GlobalEntry;

/* Static exception range attached to a chunk.  The compiler emits one of these
 * per try-block, and OP_THROW consults the table of the currently-executing
 * chunk to find the nearest handler.  This makes the happy path of try/catch
 * zero-cost (no runtime state, no opcodes, no heap allocations). */
typedef struct {
    int start_ip; /* inclusive: first instruction of the protected region   */
    int end_ip;   /* exclusive: first instruction past the protected region  */
    int catch_ip; /* absolute IP of the catch dispatcher                     */
    int exc_reg;  /* register to receive the exception value                  */
} ExceptionEntry;

typedef struct Chunk {
    /* Instruction array (each entry is a 32-bit encoded instruction) */
    uint32_t   *code;
    int         count;
    int         capacity;

    /* Source line map (parallel to code[]) */
    int        *lines;

    /* Constant pool */
    Value      *constants;
    int         const_count;
    int         const_capacity;

    /* Inline cache: GlobalEntry* per instruction, used by OP_GETGLOBAL */
    struct GlobalEntry **global_cache;

    /* Max registers needed by this chunk */
    int         max_registers;

    /* Debug name (function name or "<module>") */
    char       *name;

    /* Source file path (for relative import resolution) */
    char       *source_path;

    /* Static exception table (sorted by start_ip, searched linearly at throw) */
    ExceptionEntry *exceptions;
    int             exception_count;
    int             exception_capacity;
} Chunk;

/* ---- Lifecycle ---- */
void chunk_init(Chunk *chunk, const char *name);
void chunk_free(Chunk *chunk);

/* ---- Instruction writing ---- */
/* Append a raw 32-bit instruction; returns its index in chunk->code[]. */
int  chunk_write(Chunk *chunk, uint32_t inst, int line);

/* Convenience emitters using encoding macros from opcode.h */
int  chunk_emit_ABC (Chunk *chunk, int line, OpCode op, uint8_t a, uint8_t b, uint32_t c);
int  chunk_emit_ABx (Chunk *chunk, int line, OpCode op, uint8_t a, uint16_t bx);
int  chunk_emit_AsBx(Chunk *chunk, int line, OpCode op, uint8_t a, int32_t sbx);

/* ---- Constant pool ---- */
int  chunk_add_const(Chunk *chunk, Value value);
int  chunk_add_string(struct VM *vm, Chunk *chunk, const char *cstr);
int  chunk_add_string_len(struct VM *vm, Chunk *chunk, const char *chars, int length);

/* ---- Patch helpers (for back-patching jump targets) ---- */
void chunk_patch_sBx(Chunk *chunk, int at, int32_t sbx);
/* Patch the 9-bit C field of a fused compare-and-branch (forward 0..511) */
void chunk_patch_jump_c(Chunk *chunk, int at, int32_t offset);

/* ---- Exception table (zero-cost try/catch) ---- */
/* Append a static exception range covering [start_ip, end_ip) whose handler
 * sits at catch_ip and writes the raised value into exc_reg.  start_ip must
 * be less than end_ip and both must reference valid instruction indices. */
int  chunk_add_exception(Chunk *chunk, int start_ip, int end_ip,
                         int catch_ip, uint8_t exc_reg);

/* ---- Disassembly ---- */
void chunk_disassemble(Chunk *chunk);

#endif /* LUNA_CHUNK_H */
