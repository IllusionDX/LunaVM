# Luna Implementation Plan

## Pipeline

```
Source → Lexer → Parser → AST → Semantic Analysis → Bytecode → VM → JIT
```

## AST

### Node Types

| Category | Nodes |
|----------|-------|
| **Literals** | `Int`, `Float`, `String`, `Bool`, `Null`, `List`, `Dict` |
| **Declarations** | `Var`, `Let`, `Const`, `Function`, `Class`, `Enum` |
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
| 2 | Int | signed integer |
| 3 | Uint | unsigned integer |
| 4 | Float | 32-bit floating point |
| 5 | Double | 64-bit floating point |
| 6 | String | UTF-8 string |
| 7 | List | dynamic array |
| 8 | Dict | key-value map |
| 9 | Object | class instance |
| 10 | Function | function closure |
| 11 | Native | C/native function |

### Bytecode Instruction Set

```
Format: [opcode: 1 byte][ra: 1 byte][rb: 1 byte][imm: 4 bytes]
```

Register-based: operations read from `ra`, `rb`, write to `ra`.

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
| `CALL` | ra, fn, nargs | Call function with nargs (args in ra...ra+nargs-1) |
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

```
Format: [opcode: 1 byte][ra: 1 byte][rb: 1 byte][imm: 4 bytes]
```

- Single-byte opcode (256 instructions max)
- Two register indices (ra, rb), one immediate/displacement
- 8-byte total for alignment (optional)

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

### V1: Register VM Interpreter
- Register-based bytecode
- Simple dispatch loop

### V2: Optimized VM
- Advanced register allocator
- Inline caches
- Fast path detection

### V3: JIT Compiler
- Trace JIT for hot loops
- Type specialization
- Type hints enable compile-time optimization

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

## Security

- Sandboxed VM for user scripts
- No raw memory access
- Limited file/network access