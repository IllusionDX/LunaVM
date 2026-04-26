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

All values are stored in a single `uint64_t`. Real IEEE-754 doubles use their raw bit pattern. All other values are tagged with a quiet-NaN signature (`0x7FFC` in the top 16 bits) so they never alias valid doubles.

| Pattern | Bits | Description |
|---------|------|-------------|
| Double | raw IEEE-754 | Any valid floating-point number |
| Pointer | `QNAN \| ptr` | Heap object (`Obj*`), low 3 bits = 000 (8-byte aligned) |
| int32 | `QNAN \| TAG_INT \| (i << 3)` | Small signed integer, shifted into payload |
| true | `QNAN \| TAG_TRUE` | Boolean true |
| false | `QNAN \| TAG_FALSE` | Boolean false |
| nil | `QNAN \| TAG_NIL` | Null value |

Object type discrimination (String, List, Dict, Function, etc.) is encoded into unused NaN payload bits (bits 48, 49, 63) so `IS_STRING`, `IS_LIST`, etc. are single bitwise checks without pointer dereference.

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
| `MOVE` | ABC | `A = B` |

#### Register Operations

| Opcode | Format | Description |
|--------|--------|-------------|
| `COPY` | ABC | `A = copy(B)` — bump refcount |
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
| `0.2.1-alpha` | **Current** | Array slicing: `list[1:5]`, `list[::-1]`, `string[1:4:2]`. Supports omitted start/stop/step, negative indices, and negative step for reversal. New `EXPR_SLICE` AST node, `OP_SLICE` opcode, and VM implementation for both lists and strings. |

## Roadmap

| Version | Milestone |
|---------|-----------|
| `0.2.0` | Classes / enums solidified. Proper lambda syntax (`=>`, single-expression bodies). Better error messages. Type-hint keywords (`list`, `dict`, `int`, `string`, etc.) become context-sensitive — valid as identifiers everywhere except after `:` in declarations. Multi-line string comments (`"""..."""`). |
| `0.3.0` | Modules and imports actually work. |
| `0.4.0` | Standard library (strings, math, io, os). |
| `0.5.0` | Debugger / profiler. |
| `0.6.0` | Coroutines / async (`await`, `async def`). |
| `0.7.0` | Generation 2 optimized VM lands. Type specialization from hints (e.g. monomorphic inline caches for `int`-hinted variables). |
| `0.8.0` | Generation 3 simple JIT. |
| `0.9.0-beta` | Spec freeze, release candidate phase. |
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

## Security

- Sandboxed VM for user scripts
- No raw memory access