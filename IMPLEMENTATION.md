# LunaScript Implementation Plan

## Pipeline

```
Source → Lexer → Parser → AST → Semantic Analysis → Bytecode → VM → JIT
```

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

### Value Types

> V1: Simple types. Granular types (int8...int64, uint8...uint64, float32) in V2+.

| Tag | Type | Description |
|-----|------|-------------|
| 0 | Null | null value |
| 1 | Bool | true/false |
| 2 | Int | signed integer (int64) |
| 3 | Uint | unsigned integer (uint64) |
| 4 | Float | 32-bit floating point |
| 5 | Double | 64-bit floating point |
| 6 | NaN | Not-a-Number (IEEE 754) |
| 7 | String | UTF-8 string |
| 8 | List | dynamic array |
| 9 | Dict | key-value map |
| 10 | Object | class instance |
| 11 | Function | function closure |
| 12 | Native | C/native function |
| 13 | Char | Unicode code-point (uint32) |

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

| Opcode | Args | Description |
|--------|------|-------------|
| `LOADK` | rd, #const | Load constant from constant pool to rd |
| `LOADNULL` | rd | Load null to rd |
| `LOADTRUE` | rd | Load true to rd |
| `LOADFALSE` | rd | Load false to rd |
| `LOADI` | rd, imm | Load immediate int to rd |
| `LOADF` | rd, imm | Load immediate float to rd |
| `MOVE` | rd, rs | Move rs to rd |

#### Register Operations

| Opcode | Args | Description |
|--------|------|-------------|
| `COPY` | rd, rs | Copy rs to rd (full ref) |
| `SWAP` | rs, rt | Swap values in rs and rt |

#### Arithmetic

| Opcode | Args | Description |
|--------|------|-------------|
| `ADD` | rd, rs, rt | rd = rs + rt |
| `SUB` | rd, rs, rt | rd = rs - rt |
| `MUL` | rd, rs, rt | rd = rs * rt |
| `DIV` | rd, rs, rt | rd = rs / rt |
| `MOD` | rd, rs, rt | rd = rs % rt |
| `NEG` | rd, rs | rd = -rs |
| `BAND` | rd, rs, rt | rd = rs & rt |
| `BOR` | rd, rs, rt | rd = rs \| rt |
| `BXOR` | rd, rs, rt | rd = rs ^ rt |
| `BNOT` | rd, rs | rd = ~rs |
| `SHL` | rd, rs, rt | rd = rs << rt |
| `SHR` | rd, rs, rt | rd = rs >> rt |

#### Comparison

| Opcode | Args | Description |
|--------|------|-------------|
| `EQ` | rd, rs, rt | rd = (rs == rt) |
| `NE` | rd, rs, rt | rd = (rs != rt) |
| `LT` | rd, rs, rt | rd = (rs < rt) |
| `LE` | rd, rs, rt | rd = (rs <= rt) |
| `GT` | rd, rs, rt | rd = (rs > rt) |
| `GE` | rd, rs, rt | rd = (rs >= rt) |

#### Logical

| Opcode | Args | Description |
|--------|------|-------------|
| `NOT` | rd, rs | rd = not rs |
| `JMP` | pc | Unconditional jump |
| `JZ` | rs, pc | Jump if rs is falsy |
| `JNZ` | rs, pc | Jump if rs is truthy |

#### Functions

| Opcode | Args | Description |
|--------|------|-------------|
| `CALL` | ra, fn, nargs | Call function with nargs |
| `RET` | rd | Return value from function |
| `ENTER` | nlocals, nstack | Enter function, allocate locals |
| `LEAVE` | | Exit function |

#### Object Operations

| Opcode | Args | Description |
|--------|------|-------------|
| `NEW` | rd, class | Create new instance |
| `NEWDICT` | rd | Create new dict |
| `NEWLIST` | rd, size | Create new list |
| `INDEXGET` | rd, rs, rt | rd = rs[rt] |
| `INDEXSET` | rs, rt, val | rs[rt] = val |
| `MEMBERGET` | rd, rs, #field | rd = rs.field |
| `MEMBERSET` | rs, #field, val | rs.field = val |
| `INVOKE` | rd, rs, #method, nargs | rd = rs.method(...args) |
| `SUPER` | rd, base, #method, nargs | Call parent method |

#### Closures & Scope

| Opcode | Args | Description |
|--------|------|-------------|
| `CLOSURE` | rd, fn | Create closure |
| `GETGLOBAL` | rd, #name | Get global variable |
| `SETGLOBAL` | #name, rs | Set global variable |
| `GETUPVAL` | rd, upindex | Get upvalue |
| `SETUPVAL` | upindex, rs | Set upvalue |

#### Exceptions

| Opcode | Args | Description |
|--------|------|-------------|
| `THROW` | rs | Throw exception |
| `TRY` | pc | Setup try catch |
| `ENDTRY` | | End try block |

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
- Tagged union (type tag + payload)
- Immediate optimization for small ints

**Frame:**
- Program counter (pc)
- Register array
- Stack base pointer
- Return address

**Call Stack:**
- Array of frames
- Grows upward (stack-allocated frames possible)

**Heap:**
- Managed allocation
- Mark-and-sweep GC (V1)
- Generational GC (V2+)

## JIT Roadmap

> **Note:** The V1-V4 roadmap tracks the *VM engine* evolution (interpreter → optimized VM → JIT). It is orthogonal to the public SemVer release cycle, which tracks *language stability* and *stdlib maturity*.

### V1: Register VM Interpreter
- Register-based bytecode
- Simple dispatch loop
- Baseline performance

### V2: Optimized VM
- Advanced register allocator
- Inline caches
- Fast path detection

### V3: Simple JIT
- Trace JIT for hot loops
- Compile frequently-executed code to machine code
- No DynASM needed, custom code emitter

### V4: Advanced JIT (if needed)
- Type specialization
- Type hints enable compile-time optimization
- Full JIT with inline caches (consider DynASM or LLVM backend)

## Release Roadmap (SemVer)

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
| `0.1.9-alpha` | **Current** | Closure return-value routing fix. INVOKE convention fix. Method invocation fix. Class instantiation fix. Compound assignment via upvalues fix. `DEBUG` preprocessor flag for conditional debug output. |
| `0.2.0` | Planned | Classes / enums solidified. Proper lambda syntax (`=>`, single-expression bodies). Better error messages. |
| `0.3.0` | Planned | Modules and imports actually work. |
| `0.4.0` | Planned | Standard library (strings, math, io, os). |
| `0.5.0` | Planned | Package manager (`luna install`). |
| `0.6.0` | Planned | Debugger / profiler. |
| `0.7.0` | Planned | V2 optimized VM lands. |
| `0.8.0` | Planned | V3 simple JIT. |
| `0.9.0-beta` | Planned | Spec freeze, release candidate phase. |
| `1.0.0` | Planned | Language spec is stable. Stdlib is complete. Backward compatibility promised. |

**Rule of thumb:** bump the minor version when a user-facing feature ships (e.g. package manager, stdlib module); bump the engine version (V1→V2) when the *runtime* gets a major architectural upgrade. They do not have to stay in lockstep.

## Memory Management

### V1
- Simple mark-and-sweep GC
- Incremental collection

### V2+
- Generational GC
- Object finalizers
- Weak references

## Error Handling

- Stack traces on exceptions
- Debug info in bytecode (optional)
-panic/recover for native code

## Optimization Roadmap

| Priority | Optimization | Impact | Complexity |
|----------|--------------|--------|------------|
| High | **NaN Boxing** | *Done in 0.1.8-alpha* — unified all Value types into a single `uint64_t`. Faster copying, less memory, better cache usage. |
| High | **Small Object Optimization** | Inline small lists/dicts (e.g., <= 4 elements) directly into the object struct to avoid separate heap allocations. | Moderate — changes object layout and collection helpers. |
| Medium | **Hidden Classes / Shapes** | Give instances a fixed field layout (array indexing) instead of open hash maps. Makes property access O(1) instead of O(n). | High — requires shape objects, transition trees, compiler changes for shape-aware ops. |
| Low | **String Interning** | *Done in 0.1.5-alpha* | — |
| Low | **Computed GOTOs** | *Done in 0.1.4-alpha* | — |

## Security

- Sandboxed VM for user scripts
- No raw memory access
- Limited file/network access