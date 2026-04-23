/* compiler.h — AST → Bytecode compiler for Luna register VM. */

#ifndef LUNA_COMPILER_H
#define LUNA_COMPILER_H

#include "ast.h"
#include "chunk.h"
#include "vm.h"

/* Compile a Program AST into a top-level Chunk and register globals in vm.
 * Returns true on success, false on failure. */
bool compile_program(Program *program, Chunk *chunk, VM *vm);

#endif /* LUNA_COMPILER_H */
