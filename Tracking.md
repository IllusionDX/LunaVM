# Luna Bytecode Register VM Migration Tracking

## Phase 1: Infrastructure ✅
- [x] Define 32-bit instruction format (ABC / ABx / AsBx) in `IMPLEMENTATION.md`
- [x] Add `Char` type (tag 13) to spec value type table
- [x] Create `opcode.h` — OpCode enum + ENCODE_*/DECODE_* macros for 32-bit words
- [x] Create `chunk.h` / `chunk.c` — instruction array, constant pool, line map, back-patch helpers
- [x] Create `vm.h` — CallFrame (register window), global hash table, VM state struct
- [x] Create `vm_builtins.c` — global table CRUD, all native builtins, list & dict method dispatch
- [x] Create `vm.c` — 32-bit fetch-decode-execute loop, all opcodes from spec
- [x] Rewrite `value.h` — no AST dependency, Dict replaces Map, no Struct, V1 type tags, NativeFn takes VM*
- [x] Rewrite `value.c` — Dict ops, list ops, ARC, value_to_string with UTF-8 char

---

## Phase 1.5: Frontend Alignment (old static → new dynamic) ✅

> The existing lexer, parser, and AST were written for a statically-typed language.
> They have been updated to match the dynamic luna.md spec.

### AST (`ast.h`)
- [x] Remove `Type *inferred_type` field from `Expr` and `Stmt` (static inference, no longer needed)
- [x] Remove `int field_offset`, `bool needs_retain`, `bool needs_release` from `Expr`/`Stmt` (ARC hints, handled by VM)
- [x] Remove `EXPR_STRUCT_LITERAL` and `EXPR_ARRAY_LITERAL` from `ExprKind` (no structs in spec)
- [x] Remove `DECL_STRUCT` from `DeclKind` and its `struct_decl` union member
- [x] Remove the `Type` struct and `TypeKind` enum entirely — type annotations are optional documentation hints only, not semantic nodes
- [x] Remove `FunctionParam.param_type` and `StructField` — replace params with plain `char *name`
- [x] Rename `EXPR_MAP_LITERAL` → `EXPR_DICT_LITERAL` everywhere
- [x] Remove `OBJ_ARRAY`, `OBJ_MAP`, `OBJ_STRUCT`, `OBJ_CLASS` from object kinds (already done in `value.h`)
- [x] Remove `Decl.field_offsets` (static layout, irrelevant in dynamic VM)

### Lexer (`lexer.h` / `lexer.c`)
- [x] Remove static type keyword tokens: `TOK_VOID_TYPE`, `TOK_LONG_TYPE`, `TOK_BYTE_TYPE` (not in V1 spec)
- [x] Remove `TOK_MAP_TYPE` — the spec uses `dict`, not `map`; added `TOK_DICT` keyword
- [x] Remove `TOK_STRUCT` token (no structs in new spec)
- [x] Keep `TOK_INT_TYPE`, `TOK_FLOAT_TYPE`, `TOK_DOUBLE_TYPE`, `TOK_BOOL_TYPE`, `TOK_CHAR_TYPE`, `TOK_STRING_TYPE`, `TOK_LIST_TYPE` — these are valid optional type hint tokens per luna.md

### Parser (`parser.c`)
- [x] Make type annotations (`var x: int = 5`) fully optional everywhere — consumed and discarded via `skip_type_hint()`
- [x] Remove struct declaration parsing (`DECL_STRUCT`)
- [x] Update map literal parsing to produce `EXPR_DICT_LITERAL`
- [x] Remove any parser code that enforces static typing rules (e.g., requiring return types, checking param types)
- [x] Ensure `def func(a, b):` with no type hints parses cleanly — `FunctionParam` now just `char *name`

### Analyzer (`analyzer.h` / `analyzer.c`)
- [x] **Remove entirely** — static type checking is not part of the dynamic VM pipeline
- [x] Remove `analyzer_new()`, `analyze_program()`, `analyzer_free()` calls from `main.c`

### Legacy eval files (`eval_*.c`, `environment.c`)
- [x] Partially updated for compatibility (renamed Map→Dict, removed Struct references) — files will be fully removed in Phase 4
- [x] Build is intentionally broken for eval path until Phase 3 wires in compiler+VM

### Fixes applied during Phase 2
- [x] Fixed `value.h` `DictEntry` → `DictNode` rename to avoid conflict with `ast.h`
- [x] Fixed `value.h` ordering so `Value` struct is defined before `DictNode` (was causing incomplete-type error)
- [x] Fixed `chunk.h` `Chunk` struct tag so forward declarations in `value.h` match
- [x] Fixed `parser.c` `make_stmt` allocating `sizeof(StmtKind)` instead of `sizeof(Stmt)`
- [x] Fixed `parser.c` class body parsing to accept `var` before field names and skip unknown tokens to avoid infinite error loops
- [x] Fixed `vm.c` `OP_INVOKE` to read method name from `RA+1` (compiler convention)
- [x] Fixed `vm.c` `OP_INVOKE` arg collection to start at `RB+2` (skipping method name register)
- [x] Fixed `vm.c` `OP_NEW` to look up class prototype from globals and copy methods/fields
- [x] Fixed `vm.c` local variable name collisions with `inst` → `obj_inst` / `new_inst` (shadowed the `inst` macro used by `RA`)

---

## Phase 2: Compiler (AST → Bytecode) ✅
- [x] Create `compiler.h` / `compiler.c` — walk updated AST, emit register-VM instructions
  - [x] Scope / locals → register mapping (linear register allocator)
  - [x] Literals: LOADK, LOADNULL, LOADTRUE, LOADFALSE, LOADI
  - [x] Binary / Unary expressions: ADD, SUB, MUL, DIV, MOD, NEG, bitwise, comparison
  - [x] Variable get/set: GETGLOBAL / SETGLOBAL (V1 — all vars are global or frame-local)
  - [x] Control flow: JMP, JZ, JNZ with back-patching via `chunk_patch_sBx`
  - [x] If / else: JZ over then-block, JMP over else-block
  - [x] While loop: JZ to exit, JMP back to condition
  - [x] For loop: compile iterable, counter register, INDEXGET per iteration
  - [ ] Switch: chain of EQ + JZ + JMP (V1 stub)
  - [x] Function declarations: compile body into sub-Chunk, store as global ObjFunction
  - [x] Function calls: CALL / RET
  - [x] Method calls: INVOKE with method-name constant in pool
  - [x] Object creation: NEW + INVOKE _init, NEWLIST, NEWDICT
  - [x] Member access: MEMBERGET / MEMBERSET (ABC encoding, field name in constant pool)
  - [x] Index access: INDEXGET / INDEXSET
  - [x] Dict literals: NEWDICT + INDEXSET per entry
  - [x] List literals: NEWLIST + list.add per element
  - [x] Classes: compile each method into a sub-Chunk, register class definition globally
  - [x] Enums: compile as globals with integer values
  - [ ] Try/Catch/Finally: TRY / ENDTRY / THROW (V1 stub)
  - [ ] Import: stub (resolve at runtime from global table)

## Phase 3: Pipeline Wiring ✅
- [x] Update `main.c` — Lexer → Parser → (updated AST) → Compiler → VM; drop Analyzer
- [x] Update `Makefile` — add `chunk.c`, `vm.c`, `vm_builtins.c`, `compiler.c`; remove old `eval_*.c`, `environment.c`, `analyzer.c`
- [x] Verify `test.luna` produces correct output end-to-end

## Phase 4: Cleanup ✅
- [x] Delete legacy files: `eval_expr.c`, `eval_stmt.c`, `eval_call.c`, `eval_decl.c`, `eval.h`, `environment.h`, `environment.c`, `analyzer.h`, `analyzer.c`
- [x] Strip remaining static-typing remnants from `ast.h` that survived Phase 1.5
- [x] Run full regression on `test.luna` and `test2.luna`
