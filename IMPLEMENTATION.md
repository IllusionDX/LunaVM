# LunaScript Implementation Plan

## Pipeline

```
Source → Lexer → Parser → AST → Compiler → Bytecode → VM
```

> JIT compilation is planned for a future engine generation (see Engine Evolution Roadmap below).

## AST

### Node Types

| Category | Nodes |
|----------|-------|
| **Literals** | `Int`, `Float`, `String`, `Bool`, `Null`, `List`, `Dict` |
| **Declarations** | `Var`, `Const`, `Function`, `Class`, `Enum` |
| **Expressions** | `Binary`, `Unary`, `Call`, `Index`, `Member`, `New`, `Super` |
| **Statements** | `If`, `While`, `For`, `Switch`, `Return`, `Break`, `Continue`, `Block` |

### Optimization Passes

- Constant folding
- Dead code elimination
- Type inference from hints

## Bytecode VM

### Architecture

- Register-based for performance
- Fixed instruction width for fast dispatch
- Local register window per frame
- Tagged value representation

### Value Representation (NaN Boxing)

All values are stored in a single `uint64_t`. Real IEEE-754 doubles use their raw bit pattern. All other values are tagged with a quiet-NaN signature so they never alias valid doubles.

| Pattern | Bits | Description |
|---------|------|-------------|
| Double | raw IEEE-754 | Any valid floating-point number |
| Pointer | `QNAN \| type_tag \| ptr` | Heap object (`Obj*`), 4-bit type tag in bits 47-50 |
| int32 | `QNAN \| TAG_INT \| (i << 3)` | Small signed integer, shifted into payload |
| true | `QNAN \| TAG_TRUE` | Boolean true |
| false | `QNAN \| TAG_FALSE` | Boolean false |
| nil | `QNAN \| TAG_NIL` | Null value |

#### Bit Layout (64-bit)

```
63 62-52 51 50 49 48 47 46 ... 3  2 1 0
│   │    │  │  │  │  │  │      │  │ │ │
│   │    │  │  │  │  │  └── Pointer payload (47 bits = 128 TB addressable)
│   │    │  │  │  │  └────── Type tag bit 0 (OBJ_STRING=0, OBJ_LIST=1, ...)
│   │    │  │  │  └───────── Type tag bit 1
│   │    │  │  └──────────── Type tag bit 2
│   │    │  └─────────────── Type tag bit 3 (OBJ_ENUM=8)
│   │    └────────────────── Quiet-NaN bit (must be 1)
│   └─────────────────────── Exponent (all 1s = NaN)
└─────────────────────────── Sign bit (0, arbitrary for NaN)
```

- **Bits 0-2**: Sub-tag (`nil`=1, `true`=2, `false`=3, `int`=4, `empty`=5). Object pointers have `000` because `malloc` is 8-byte aligned.
- **Bits 3-46**: Payload (integer value shifted left by 3, or pointer address).
- **Bits 47-50**: 4-bit **object type tag** (`OBJ_STRING`..`OBJ_ENUM`).
- **Bit 51**: Quiet-NaN bit (must be 1, otherwise it's a signaling NaN and the FPU may trap).
- **Bits 52-62**: Exponent (all 1s, required by IEEE-754 for NaN).
- **Bit 63**: Sign (0, arbitrary for NaN).

#### Pointer Constraint

The 4-bit type tag consumes bit 47, which means Luna **requires pointers to live in the low 47-bit address space** (128 TB). This is identical to LuaJIT GC64, which uses the same layout in production on x86_64 and ARM64.

On systems with 57-bit addressing (Intel LA57 / ARM64 LVA), Luna's allocator must ensure GC-managed objects stay below `0x00007FFFFFFFFFFF`. This is achieved by:
- Using `mmap` with explicit address hints or `MAP_32BIT`
- Rejecting high addresses and retrying until the OS provides a low one
- Documenting that C-API handles and raw pointers must be in the low 47 bits

> **Future expansion**: If a 5th type-tag bit is ever needed, bit 63 is currently free (it only carries the NaN sign, which is don't-care). This would give 32 object types using bits 47-50 + 63.

> Granular numeric types (int8...int64, uint8...uint64, float32) are not implemented and may be added in a future release.

### Bytecode Instruction Set

#### Instruction Format

All instructions are a fixed **4 bytes (32 bits)** — a single `uint32_t`. Eight instructions fit in one 64-byte CPU cache line, matching Lua 5.x's approach for fast dispatch.

Three instruction encodings share the same 32-bit word:

```
ABC  format  (3-register ops)
  bits 0-7   : opcode  (8 bits, up to 256 opcodes)
  bits 8-15  : A       (8 bits, destination register)
  bits 16-23 : B       (8 bits, source register 1)
  bits 24-31 : C       (8 bits, source register 2)

ABx  format  (reg + unsigned 16-bit immediate)
  bits 0-7   : opcode
  bits 8-15  : A
  bits 16-31 : Bx      (16 bits unsigned — constant pool index, list size, …)

AsBx format  (reg + signed 16-bit offset)
  bits 0-7   : opcode
  bits 8-15  : A
  bits 16-31 : sBx     (16 bits signed — jump offsets, relative PCs)
```

Register-based: operations read from `A`, `B`, `C`; write result to `A`.

#### Constants & Loading

| Opcode | Format | Description |
|--------|--------|-------------|
| `LOADK` | ABx | `A = constants[Bx]` |
| `LOADNULL` | ABC | `A = null` |
| `LOADTRUE` | ABC | `A = true` |
| `LOADFALSE` | ABC | `A = false` |
| `LOADI` | AsBx | `A = (int)sBx` |
| `MOVE` | ABC | `A = B` — copies value with refcount retain/release |

#### Register Operations

| Opcode | Format | Description |
|--------|--------|-------------|
| `SWAP` | ABC | `swap(A, B)` |

#### Arithmetic

| Opcode | Format | Description |
|--------|--------|-------------|
| `ADD` | ABC | `A = B + C` |
| `SUB` | ABC | `A = B - C` |
| `MUL` | ABC | `A = B * C` |
| `DIV` | ABC | `A = B / C` |
| `MOD` | ABC | `A = B % C` |
| `NEG` | ABC | `A = -B` |
| `BAND` | ABC | `A = B & C` |
| `BOR` | ABC | `A = B | C` |
| `BXOR` | ABC | `A = B ^ C` |
| `BNOT` | ABC | `A = ~B` |
| `SHL` | ABC | `A = B << C` |
| `SHR` | ABC | `A = B >> C` |
| `ADDI` | ABC | `A = A + (int8_t)B` — integer fast path |
| `SUBI` | ABC | `A = A - (int8_t)B` — integer fast path |

#### Comparison

| Opcode | Format | Description |
|--------|--------|-------------|
| `EQ` | ABC | `A = (B == C)` |
| `NE` | ABC | `A = (B != C)` |
| `LT` | ABC | `A = (B < C)` |
| `LE` | ABC | `A = (B <= C)` |
| `GT` | ABC | `A = (B > C)` |
| `GE` | ABC | `A = (B >= C)` |

#### Logical

| Opcode | Format | Description |
|--------|--------|-------------|
| `NOT` | ABC | `A = not B` |

#### Control Flow

| Opcode | Format | Description |
|--------|--------|-------------|
| `JMP` | AsBx | `PC += sBx` |
| `JZ` | AsBx | `if !A then PC += sBx` |
| `JNZ` | AsBx | `if A then PC += sBx` |

#### Functions

| Opcode | Format | Description |
|--------|--------|-------------|
| `CALL` | ABC | `A = call B(args B+1..B+C)` |
| `RET` | ABC | `return A` |
| `ENTER` | ABx | allocate `Bx` local slots |
| `LEAVE` | ABC | deallocate locals, restore frame |
| `CLOSURE` | ABx | `A = closure(constants[Bx])` |

#### Globals & Upvalues

| Opcode | Format | Description |
|--------|--------|-------------|
| `GETGLOBAL` | ABx | `A = globals[constants[Bx]]` |
| `SETGLOBAL` | ABx | `globals[constants[Bx]] = A` |
| `GETUPVAL` | ABx | `A = upvalues[Bx]` |
| `SETUPVAL` | ABx | `upvalues[Bx] = A` |

#### Object Operations

| Opcode | Format | Description |
|--------|--------|-------------|
| `NEW` | ABx | `A = new Instance(class=constants[Bx])` |
| `NEWDICT` | ABC | `A = {}` |
| `NEWLIST` | ABx | `A = []` pre-sized to `Bx` elements |
| `LISTAPPEND` | ABC | `A.append(B)` |
| `GETITER` | ABC | init iterator state at `A`, from object `B` |
| `FORLOOP` | AsBx | next elem in `A+2`. If iter has next, `PC += sBx` |
| `INDEXGET` | ABC | `A = B[C]` |
| `INDEXSET` | ABC | `A[B] = C` |
| `SLICE` | ABC | `A = slice(B, B+1..B+C)` — C=0..3, null=omit |
| `MEMBERGET` | ABC | `A = B.field` (field = constants[C]) |
| `MEMBERSET` | ABC | `A.field = B` (field = constants[C]) |
| `INVOKE` | ABC | `A = B.method(nargs=C)` (method-name in const slot) |
| `SUPER` | ABC | `A = super.method(nargs=C)` |

#### Exceptions

| Opcode | Format | Description |
|--------|--------|-------------|
| `THROW` | ABC | `throw A` |
| `TRY` | AsBx | push try frame, catch at `PC + sBx` |
| `ENDTRY` | ABC | pop try frame |
| `HALT` | ABC | stop VM |

### Instruction Format

All instructions are a fixed **4 bytes (32 bits)** encoded as a `uint32_t`.

| Encoding | Bits 0-7 | Bits 8-15 | Bits 16-23 | Bits 24-31 |
|----------|----------|-----------|------------|------------|
| **ABC**  | opcode   | A (dest)  | B (src1)   | C (src2)   |
| **ABx**  | opcode   | A         | Bx (unsigned 16-bit) ←———→       |
| **AsBx** | opcode   | A         | sBx (signed 16-bit)  ←———→       |

- 8 instructions fit in a single 64-byte CPU cache line
- 256 opcode budget (8-bit opcode)
- 256 register budget per frame (8-bit A/B/C)
- 16-bit Bx allows constant pools up to 65535 entries
- 16-bit sBx allows jump offsets of ±32767 instructions

### VM Components

**Value:**
- NaN-boxed `uint64_t` — doubles use raw IEEE-754 bits; all other values use quiet-NaN tagging
- Pointers to heap objects are 8-byte aligned (low 3 bits = 000)
- Immediate `int32_t` stored in payload (shifted left by 3 bits)

**Frame:**
- Program counter (pc) into chunk->code[]
- Register window (pointer into VM stack array)
- Return address (previous frame's pc)
- Chunk reference (for constant pool access)

**Call Stack:**
- Array of `CallFrame` structs
- Grows upward; max depth is a compile-time constant

**Heap:**
- Managed allocation via `malloc`/`free`
- Hybrid ARC + Mark-and-Sweep GC
  - ARC handles acyclic objects (strings, lists, dicts, functions)
  - GC detects and collects reference cycles
- Small Object Optimization (SOO): small lists/dicts store first 4 elements inline

## Engine Evolution Roadmap

> **Note:** The engine roadmap tracks the *VM runtime* evolution (interpreter → optimized VM → JIT). It is orthogonal to the public SemVer release cycle, which tracks *language stability* and *stdlib maturity*.

### Generation 1: Register VM Interpreter (current)
- Register-based bytecode
- Computed-GOTO dispatch loop
- Baseline performance
- Inline caches for global variable access
- NaN-boxed values

### Generation 2: Optimized VM
- Advanced register allocator
- Monomorphic inline caches for property/method access
- Fast path detection (type-specialized integer arithmetic)
- Hidden classes / shapes for instance fields

### Generation 3: Simple JIT
- Trace JIT for hot loops
- Compile frequently-executed code to machine code
- Custom code emitter (no external dependency)

### Generation 4: Advanced JIT (if needed)
- Full method JIT with inline caches
- Type specialization from hints
- Consider DynASM or LLVM backend

## Changelog

| Version | Status | Milestone |
|---------|--------|-----------|
| `0.1.0-alpha` | Done | Core language works. Basic VM, lexer, parser, compiler. REPL. |
| `0.1.1-alpha` | Done | String concat optimization. Inline cache for global variable access. |
| `0.1.2-alpha` | Done | F-strings (`f"Hello {name}"`). |
| `0.1.3-alpha` | Done | Scratch buffer optimization. Test suite reorganization. |
| `0.1.4-alpha` | Done | Computed GOTO dispatch loop. |
| `0.1.5-alpha` | Done | String interning. `clock()` builtin. |
| `0.1.6-alpha` | Done | Anonymous function expressions (`def(): ...`). |
| `0.1.7-alpha` | Done | List comprehensions (`[x for x in list]`). |
| `0.1.8-alpha` | Done | NaN boxing — unified `Value` into single `uint64_t`. |
| `0.1.9-alpha` | Done | Closure return-value routing fix. INVOKE convention fix. Method invocation fix. Class instantiation fix. Compound assignment via upvalues fix. `DEBUG` preprocessor flag for conditional debug output. |
| `0.1.10-alpha` | Done | Short-circuit boolean operators (`and`/`or`). Iterator protocol (`OP_GETITER`/`OP_FORLOOP`) for dicts, lists, strings. Ordered compact dict (Python 3.7+ style). `OP_LISTAPPEND` for fast list construction. Compound assignment double-evaluation fix. Refcount fixes for `dict_remove`/`list_remove`/`list_pop`. |
| `0.1.11-alpha` | Done | Fixed VM register auto-release: `SET_REG_PRIM` skips retain for primitives, `SET_REG` retains/releases objects. Integer fast-path in `OP_ADD`. All constructors use refcount=0 (floating ARC). |
| `0.1.12-alpha` | Done | CRLF parsing support in lexer (`\r\n` handled in comments, blank lines, and newline tokens). Centralized VM exception handling: all runtime errors route through `op_throw` via `_exc` local, preventing infinite error spam. `OP_DIV`/`OP_MOD` throw on division by zero instead of silently returning null. |
| `0.1.13-alpha` | Done | Hybrid ARC + Mark-and-Sweep garbage collector. ARC handles acyclic objects; GC detects and collects reference cycles. Global object tracking via `all_objects` intrusive linked list. Two-phase sweep with refcount pinning to prevent re-entrant ARC cascade. `gc()` and `gc_info()` builtins for debugging. |
| `0.1.14-alpha` | Done | GC sweep refactor: replaced the 1,000,000-refcount "pinning hack" with a clean `gc_collecting` flag and a `free_object_container()` helper. `release_obj()` skips calling `free_object()` while `gc_collecting == true`, preventing re-entrant ARC during sweep. Eliminates the double-decrement bug on live children of garbage objects. |
| `0.1.15-alpha` | Done | GC hardening: re-entrant GC guard (`if (gc_collecting) return;`), inline cache invalidation on every collection to prevent dangling pointers, and proper `vm_free` shutdown path that releases stack values and frees all remaining objects via `free_object_container()`. |
| `0.1.16-alpha` | Done | Memory audit follow-up: fixed `dict_transition_to_heap` inline entry leak (SOO→heap transition now releases old inline references). Fixed `vm_free` use-after-free (stack freed before `close_upvalues` — reordered to close upvalues first, then free stack). Verified 8 of 12 audit findings as false positives. |
| `0.1.17-alpha` | Done | Fixed one-liner function parsing (`def add(a, b): return a + b`). Added regression test. |
| `0.1.18-alpha` | Done | List concatenation with `+` (`[1, 2] + [3, 4]`). List repetition with `*` (`[1, 2] * 3`). String repetition with `*` (`"abc" * 3`). All support commutative int operands and return empty results for zero/negative multipliers. |
| `0.2.0-alpha` | Done | NaN-boxed object type tags: encode `OBJ_STRING`, `OBJ_LIST`, `OBJ_DICT`, `OBJ_INSTANCE`, `OBJ_FUNCTION`, `OBJ_EXCEPTION`, `OBJ_CLOSURE` into unused NaN payload bits. `IS_STRING`/`IS_LIST`/etc. are now single bitwise checks without pointer dereference. Replaced verbose type-check patterns across `vm.c`, `value.c`, and `vm_builtins.c`. Fixed `OP_CALL` error-reporting path to safely handle non-object callee values. Corrected opcode dispatch section comment numbering to match true `OpCode` enum values. |
| `0.2.1-alpha` | Done | Array slicing: `list[1:5]`, `list[::-1]`, `string[1:4:2]`. Supports omitted start/stop/step, negative indices, and negative step for reversal. New `EXPR_SLICE` AST node, `OP_SLICE` opcode, and VM implementation for both lists and strings. |
| `0.2.2-alpha` | Done | Destructuring assignment (Phase 1): `var [a, b] = [1, 2]` and `var {"name": n, "hp": h} = entity`. Supports `_` placeholder for skipped positions. Compiled to existing `OP_INDEXGET` sequences. New `pattern` field in `VarDecl` AST node. |
| `0.2.3-alpha` | Done | Multiple assignment / swap: `a, b = b, a`. The compiler detects the two-local-variable swap pattern and emits a single `OP_SWAP` instruction (1 cycle instead of 3 `MOVE`s). General-case multi-assignment (`a, b = 10, 20`) evaluates all RHS values into temps first, then assigns each to its target. New `EXPR_MULTI_ASSIGN` AST node. Parser improvements: backtracking (`save_state`/`restore_state`), expression-level destructuring assignments, variadic `parser_error()`. |
| `0.2.4-alpha` | Done | Classes / enums solidified. Expanded NaN-boxing from 3-bit to 4-bit type tags (bits 47-50) to support `OBJ_ENUM`. Compile-time class inheritance: parent fields and methods flattened into child prototype. Method override with backward search (`OP_INVOKE` searches child→parent). `OP_SUPER` implemented for `super.method()` calls. Auto-call `_init` on `new`. First-class enum objects (`ObjEnum`) with `.count()`, `.keys()`, `.values()` runtime API. |
| `0.2.5-alpha` | Done | Arrow functions / lambdas: `(a, b) => a + b`, `() => 42`, `(x) => x * x`. Higher-order functions supported. Parser gains `peek_ahead(n)` for non-destructive lookahead. Fixed multi-assignment bug where `a, b, c = 1, 2, 3` was incorrectly rejected. Refactored `is_type_hint_start` to use `peek_ahead` instead of raw pointer chasing. UTF-16 BOM detection in lexer (rejects UTF-16 with clear error; supports UTF-8 BOM skip). Added `.should_fail.` test convention for regression tests that expect failure. |
| `0.2.6-alpha` | Done | Multiline comments: `"""..."""` for block comments spanning multiple lines. Lexer consumes everything between triple-quotes without generating tokens. |
| `0.2.7-alpha` | Done | `in` operator for membership testing: `item in list`, `key in dict`, `substring in string`. Type-hint keywords (`list`, `dict`, `int`, `float`, `string`, `bool`, `char`) are now context-sensitive — valid as identifiers everywhere except after `:` in declarations. Parser checks token text instead of hardcoded token types for type hints. |
| `0.2.8-alpha` | Done | Bound methods (`ObjBoundMethod`): `obj.method` returns a callable bound to `obj`. `OP_CALL` handles bound method invocation (self as reg 0). `_init` auto-call fix for zero-argument constructors. |
| `0.2.9-alpha` | Done | Default arguments (`def f(a, b=10):`) and keyword arguments (`f(a=5, b=3)`). Pre-interned param name keys for O(1) kwargs lookup. Eliminated string-interning overhead on every function call. |
| `0.2.10-alpha` | **Current** | Keyword arguments for method calls: `obj.method(a=5, b=10)`. Compiler emits `OP_MEMBERGET` + `OP_KCALL` instead of `OP_INVOKE` when kwargs are present. |

## Roadmap

| Version | Milestone |
|---------|-----------|
| `0.2.x` | Default arguments with immutability guard: `def connect(host = "localhost", port = 8080)`. Better error messages with line/column context and suggestions. |
| `0.3.x` | Modules, imports, and standard library (strings, math, io, os). Optional chaining (`?.`) for null-safe member access. |
| `0.4.x` | Embedding / C API (`LunaState`, `luna_dofile`, `luna_push_xxx`, etc.). |
| `0.5.x` | Exception handling (`try` / `catch` / `finally`, custom exceptions, stack traces). |
| `0.6.x` | Debugger / profiler. |
| `0.7.x` | Coroutines / async (`await`, `async def`). |
| `0.8.x` | Generation 2 optimized VM lands. Type specialization from hints. |
| `0.9.x` | Generation 3 simple JIT. |
| `1.0.0-beta` | Spec freeze, release candidate phase. |
| `1.0.0` | Language spec is stable. Stdlib is complete. Backward compatibility promised. |

> **Note:** A package manager (`luna install`) is not guaranteed and may ship after 1.0 depending on community demand and maintenance bandwidth.

**Rule of thumb:** bump the minor version when a user-facing feature ships (e.g. package manager, stdlib module); bump the engine generation when the *runtime* gets a major architectural upgrade. They do not have to stay in lockstep.

## Memory Management

### Current
- Hybrid ARC + Mark-and-Sweep GC. ARC handles acyclic objects; GC detects and collects reference cycles.
- Small Object Optimization (SOO): inline small lists/dicts (<= 4 elements) directly into the object struct.
- String interning via global hash table.

### Future
- Generational GC
- Object finalizers

## Error Handling

- Stack traces on exceptions
- Debug info in bytecode (optional)
- Panic/Recover for native code

## Optimization Roadmap

| Priority | Optimization | Impact | Complexity |
|----------|--------------|--------|------------|
| High | **NaN Boxing** | *Done in 0.1.8-alpha* — unified all Value types into a single `uint64_t`. Faster copying, less memory, better cache usage. |
| High | **Small Object Optimization** | *Done in 0.1.13-alpha* — inline small lists/dicts (<= 4 elements) directly into the object struct to avoid separate heap allocations. Dict SOO→heap transition fixed in 0.1.16-alpha. |
| Medium | **Hidden Classes / Shapes** | Give instances a fixed field layout (array indexing) instead of open hash maps. Makes property access O(1) instead of O(n). | High — requires shape objects, transition trees, compiler changes for shape-aware ops. |
| Low | **String Interning** | *Done in 0.1.5-alpha* | — |
| Low | **Computed GOTOs** | *Done in 0.1.4-alpha* | — |

## Future Ideas & Evolution

Features below are not committed roadmap items unless explicitly noted. They are collected here for design reference.

### Planned Evolution

**Destructuring Assignment (Unpacking)**

Phase 1 (targeted): `var [a, b] = [1, 2]` and `var {"name": n, "hp": h} = entity` — list and dict patterns inside `var`/`const` declarations, compiled to existing `OP_INDEXGET` sequences.

Phase 2 (future): `[a, b] = [3, 4]` and `{"name": n} = entity` — assignment-level destructuring without `var`. Optional syntactic sugar `a, b = [3, 4]` may follow as shorthand for `[a, b] = [3, 4]`.

Phase 3 (future): Dedicated opcodes for performance:
- `OP_UNPACK_LIST n` — decompose top-of-stack into n consecutive registers.
- `OP_GETK A, B, Bx` — `A = B.field(constants[Bx])` in a single instruction (ABx variant of MEMBERGET, bypasses the 255-constant limit of ABC).
- `OP_SETK A, Bx, C` — `A.field(constants[Bx]) = C` (symmetric write).

**Compilation strategy for destructuring:**
| Case | Generated opcodes |
|------|------------------|
| Fixed list `var [a, b] = list` | `OP_UNPACK_LIST` (1 instruction, n targets) |
| Fixed dict `var {"hp": h, "mp": m} = dict` | `OP_GETK` per field (1 instruction per binding) |
| List with rest `var [a, ...b] = list` | `OP_INDEXGET` for `a` + `OP_SLICE` for `b` |
| Dict with rest `var {"hp": h, ...rest} = dict` | `OP_GETK` for `h` + iteration loop for rest |

These would make Luna's destructuring faster than languages that rely on generic iteration.

### Ideas Under Consideration

**From JavaScript**
- Nullish coalescing (`??`) — fallback only on `null`, preserving `false` and `0`.
- Spread / rest operators (`...`) for list literals and function parameter lists.
- Object shorthand and computed keys in dict literals: `{x, "key": value, [dyn]: 42}`.
- Generators (`yield`) extending the existing iterator protocol.

**Unique / Differentiating**
- Pipe operator (`|>`) for expression-oriented data flow.
- Pattern matching (`match` expression) with list/dict/range patterns.
- Named arguments (call-site): `connect(timeout=60, host="localhost")` — pass arguments by name at call site.
- First-class regex literals: `/[a-z]+/g`.
- Traits / mixins for composable behavior without multiple inheritance.
- Method cascading (`..`) for chaining mutating calls.
- Multi-value returns via `OP_RET A, n` — use the `B` field to indicate how many consecutive registers to return. Would pair with destructuring to avoid list wrappers: `def coords(): return 10, 20` → `var [x, y] = coords()`. Requires calling-convention changes in `OP_CALL`.

**VM Architecture (Planned)**

*Type System & NaN-Boxing*

Luna uses a unified 64-bit `Value` type (NaN-boxing) where all non-double values live inside quiet-NaN payloads. The current layout:

| Bits | Purpose |
|------|---------|
| 0-2 | Sub-tags: `NIL`(1), `TRUE`(2), `FALSE`(3), `INT`(4), `EMPTY`(5) |
| 3-46 | Pointer payload (44 bits = 16 TB addressable) |
| 47-50 | **4-bit object type tag** (16 slots: ~10 used, ~6 free) |
| 51 | Quiet-NaN bit (must stay 1) |
| 52-62 | Exponent (all 1s for NaN) |
| 63 | Sign bit (NOT free — IEEE 754 NaNs can have bit 63 = 0 or 1; using it for tagging requires normalizing all hardware NaNs)

*Current types:*
- `int32` — immediate via sub-tag `INT` (bits 0-2). Fastest: no allocation, single bitwise check.
- `double` — raw IEEE-754. Hardware NaN (`0.0/0.0`) collides with `QNAN_TAG` (`0x7FF8...`). Known issue; future fix via payload sentinel or bit-63 negative space (V8 style).
- `null`, `bool` — immediate via sub-tags.
- Object types — 4-bit tag in bits 47-50.

*Why not expand NaN tags for granular types?*
The 4-bit type tag gives 16 slots. Already used: ~10 object types, ~6 free. Expanding to 32 slots would require using bit 63, which is NOT free — IEEE 754 NaNs can have bit 63 = 0 or 1, so using it for tagging requires normalizing every hardware NaN (a sweeping change). We are not doing this until we genuinely exhaust the ~6 remaining object tags. The sub-tag space (bits 0-2) has 2 free slots (6, 7), sufficient for future immediate types like `CHAR` or `INT64`.

*Solution: `OBJ_INTEGER` / `OBJ_NUMBER` with internal kind fields.*
Instead of burning NaN tags for every numeric granularity, Luna will expose a single `OBJ_INTEGER` type tag (boxed object) with an internal `IntegerKind` subfield (`I8`, `I16`, `I32`, `I64`, `U8`, `U16`, `U32`, `U64`, `BIG`). Similarly, `OBJ_NUMBER` will carry a `NumberKind` (`F32`, `F64`). The VM's fast path stays on `int32` immediate and `double` IEEE-754. Granular types only materialize when the user explicitly requests them (`var hp: int16`, `var pos: float32`) or when the stdlib needs them (pixel buffers, ML matrices). The JIT tracks granular types in its SSA IR (`IR_I32`, `IR_F64`, `IR_I64`, `IR_F32`) and emits native instructions directly — no NaN-tag involvement at the JIT level. Boxing/unboxing only happens at JIT/VM boundaries.

*Comparison with Lua/LuaJIT:*
Lua and LuaJIT expose only one numeric type to the user: `number` (double). Internally, LuaJIT may track whether a value fits in `int64` for optimization, but the programmer never sees `int32` vs `int64` vs `float32`. Luna differs: we expose granular numeric types to the programmer (like Rust or C) while keeping the VM fast-path simple (like LuaJIT).

*Future NaN-boxing redesign options (not viable without massive changes):*
1. *Payload sentinel:* Change base tag to a quiet-NaN pattern the hardware is unlikely to emit (e.g. `0x7FF8...0001`). Requires updating `IS_DOUBLE` and `IS_OBJ` to distinguish hardware NaN (treat as double) from tagged objects. This fixes the NaN collision but does NOT expand tag space.
2. *Bit-63 negative space (V8/SpiderMonkey style):* Move Luna's tagged space to `0xFFF8...` (bit 63 = 1). This makes `IS_DOUBLE(v)` a single `CMP v < 0xFFF8...` — the fastest possible type check. **Requires normalizing all hardware NaNs in `make_double()`** (every `0.0/0.0`, `sqrt(-1)`, `log(-1)` must be rewritten to a canonical NaN with bit 63 = 0). This is a sweeping change across `value.h`, the VM dispatch loop, and all floating-point builtins. Only worth doing if we genuinely run out of the ~6 free object tags.

## Language Design Decisions

### Default Arguments

Default arguments use **compile-time constants only**. The expression after `=` must be a literal of an immutable type:

| Allowed | Example |
|---------|---------|
| Integer literal | `def foo(a = 10)` |
| Float literal | `def foo(a = 3.14)` |
| String literal | `def foo(a = "hello")` |
| Boolean literal | `def foo(a = true)` |
| Null | `def foo(a = null)` |

| Rejected | Rationale |
|----------|-----------|
| List literal | `def foo(a = [])` — mutable, would be shared across calls |
| Dict literal | `def foo(a = {})` — mutable, would be shared across calls |
| Function call | `def foo(a = time.now())` — not a constant |
| Variable reference | `def foo(a = SOME_CONST)` — not a constant |

**Why:** Python's mutable default argument footgun (`def foo(a=[]): a.append(1)`) is one of the most common bugs in the language. Luna prevents it at the parser level. If you need a mutable default, use `null` and initialize in the body:

```luna
def add_item(item, items = null):
    if items == null:
        items = []
    items.append(item)
    return items
```

**Type hints with defaults:** The parser accepts `def connect(host: string = "localhost", port: int = 8080):`. The type hint is parsed first, then the `=` and default value.

### Type System — Three Operators (Planned)

**Status: Not yet implemented.** This section documents the design for three type-checking operators that will land in a future release (target: `0.3.x` after modules/imports).

| Operator | Question | Use case |
|----------|----------|----------|
| `type(x)` | "What **is** this object?" | Introspection, debugging, metaprogramming |
| `x is T` | "Is this **exactly** T?" | Fast guard clauses, null checks, primitive dispatch |
| `isinstance(x, T)` | "Can I **treat** this as T?" | Polymorphism, inheritance checks |

#### `type(x)` — Exact runtime type (current behavior, no changes planned)

Will continue to return the canonical type representation. Primitives and builtin objects return **strings**; class instances return their **class object**.

| Value | Will return |
|-------|-------------|
| `null` | `"null"` |
| `true`/`false` | `"bool"` |
| `5` | `"int"` |
| `3.14` | `"float"` |
| `"hello"` | `"string"` |
| `[1,2]` | `"list"` |
| `def(): ...` | `"function"` / `"closure"` / `"bound_method"` |
| `new Player()` | `Player` class object |

#### `x is T` / `x is not T` — Exact type assertion (planned)

Will be a fast exact-match. No inheritance, no polymorphism. Direct tag/bit/class comparison.

| Expression | Planned VM check |
|------------|------------------|
| `x is null` | `IS_NIL(x)` |
| `x is bool` | `IS_BOOL(x)` |
| `x is int` | `IS_INT(x)` |
| `x is float` | `IS_DOUBLE(x)` |
| `x is number` | `IS_INT(x) \|\| IS_DOUBLE(x)` |
| `x is string` | `IS_OBJ_TYPE(x, OBJ_STRING)` |
| `x is list` | `IS_OBJ_TYPE(x, OBJ_LIST)` |
| `x is dict` | `IS_OBJ_TYPE(x, OBJ_DICT)` |
| `x is function` | function \| closure \| bound_method |
| `x is class` | `IS_OBJ_TYPE(x, OBJ_CLASS)` |
| `x is enum` | `IS_OBJ_TYPE(x, OBJ_ENUM)` |
| `x is instance` | `IS_OBJ_TYPE(x, OBJ_INSTANCE)` |
| `x is object` | `IS_OBJ(x)` |
| `x is Player` | `IS_INSTANCE(x) && klass == Player_class` |

**`is`** will be fast because it is exact match. For class names, it will be a pointer comparison on `klass`. No hash lookups, no chain walks.

#### `isinstance(x, T)` — Compatibility check (planned)

Will walk the class hierarchy for instances. For primitives, will work the same as `is` (no hierarchy yet).

| Expression | Planned VM check |
|------------|------------------|
| `isinstance(x, int)` | `IS_INT(x)` |
| `isinstance(x, number)` | `IS_INT(x) \|\| IS_DOUBLE(x)` |
| `isinstance(x, Player)` | `IS_INSTANCE(x) && klass_chain_contains(klass, Player_class)` |
| `isinstance(x, object)` | `true` for any value |

For objects, will walk `klass → base → base → ...` until match or null.

#### Why all three are needed

- **`is`** for speed: `x is null`, `x is int` will be single bitwise checks. Intended for hot loops and guard clauses.
- **`isinstance`** for polymorphism: `isinstance(hero, Entity)` will return `true` when `hero` is a `Warrior` extending `Entity`. Respects inheritance.
- **`type`** for introspection: `type(x)` lets you compare types (`type(x) == type(y)`) and do metaprogramming.

#### Implementation status

**Heavy refactor.** Will require:
1. Lexer: `TOK_IS`, `TOK_NOT` (already exist but need `is not` sequence handling)
2. Parser: new precedence level between equality and `and`
3. AST: `EXPR_IS_TYPE` node
4. Compiler: `OP_ISTYPE` (ABC, builtin category) and `OP_INSTANCEOF` (ABx, class name lookup)
5. VM: dispatch loop handlers for both opcodes
6. Builtins: `isinstance()` function in `vm_builtins.c`

Target: `0.3.x` after modules/imports land, since it touches the same compiler pipeline.

## Security

- Sandboxed VM for user scripts
- No raw memory access