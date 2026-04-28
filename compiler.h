/* compiler.h — AST → Bytecode compiler for Luna register VM. */

#ifndef LUNA_COMPILER_H
#define LUNA_COMPILER_H

#include "ast.h"
#include "chunk.h"
#include "vm.h"

/* Compile a Program AST into a top-level Chunk and register globals in vm.
 * Returns true on success, false on failure.
 * is_repl: last expression auto-printed and assigned to global '_'.
 * is_module: emits OP_RET instead of OP_HALT so the chunk returns to caller. */
bool compile_program(Program *program, Chunk *chunk, VM *vm, bool is_repl, bool is_module);

#endif /* LUNA_COMPILER_H */
