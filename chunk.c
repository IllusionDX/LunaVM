/* chunk.c — Bytecode chunk implementation. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chunk.h"
#include "value.h"

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
}

void chunk_free(Chunk *chunk) {
    free(chunk->code);
    free(chunk->lines);

    /* Release object-type constants */
    for (int i = 0; i < chunk->const_count; i++) {
        if (IS_OBJ(chunk->constants[i]) && AS_OBJ(chunk->constants[i])) {
            release_obj(AS_OBJ(chunk->constants[i]));
        }
    }
    free(chunk->constants);
    free(chunk->global_cache);
    free(chunk->name);

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
                   uint8_t a, uint8_t b, uint8_t c) {
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
    /* Retain heap objects entering the constant pool */
    if (IS_OBJ(value) && AS_OBJ(value)) {
        retain_obj(AS_OBJ(value));
    }
    int idx = chunk->const_count++;
    chunk->constants[idx] = value;
    return idx;
}

int chunk_add_string(Chunk *chunk, const char *cstr) {
    /* De-duplicate: return existing index if already interned */
    for (int i = 0; i < chunk->const_count; i++) {
        Value v = chunk->constants[i];
        if (IS_OBJ(v) && AS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING) {
            ObjString *s = (ObjString *)AS_OBJ(v);
            if (strcmp(s->chars, cstr) == 0) return i;
        }
    }
    ObjString *obj = new_string(cstr, (int)strlen(cstr));
    int idx = chunk_add_const(chunk, make_obj((Object *)obj));
    release_obj((Object *)obj);  /* chunk_add_const retained it; balance new_string ref */
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
    /* Preserve opcode and A, overwrite bits 16-31 */
    uint32_t inst = chunk->code[at];
    uint32_t preserved = inst & 0x0000FFFF;  /* low 16 bits: opcode + A */
    uint32_t new_bx = (uint32_t)(uint16_t)((sbx) + SBIAS);
    chunk->code[at] = preserved | (new_bx << 16);
}
