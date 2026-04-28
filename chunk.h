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
} Chunk;

/* ---- Lifecycle ---- */
void chunk_init(Chunk *chunk, const char *name);
void chunk_free(Chunk *chunk);

/* ---- Instruction writing ---- */
/* Append a raw 32-bit instruction; returns its index in chunk->code[]. */
int  chunk_write(Chunk *chunk, uint32_t inst, int line);

/* Convenience emitters using encoding macros from opcode.h */
int  chunk_emit_ABC (Chunk *chunk, int line, OpCode op, uint8_t a, uint8_t b, uint8_t c);
int  chunk_emit_ABx (Chunk *chunk, int line, OpCode op, uint8_t a, uint16_t bx);
int  chunk_emit_AsBx(Chunk *chunk, int line, OpCode op, uint8_t a, int32_t sbx);

/* ---- Constant pool ---- */
int  chunk_add_const(Chunk *chunk, Value value);
int  chunk_add_string(Chunk *chunk, const char *cstr);
int  chunk_add_string_len(Chunk *chunk, const char *chars, int length);

/* ---- Patch helpers (for back-patching jump targets) ---- */
void chunk_patch_sBx(Chunk *chunk, int at, int32_t sbx);

#endif /* LUNA_CHUNK_H */
