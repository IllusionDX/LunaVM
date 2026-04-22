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

| Tag | Type | Description |
|-----|------|-------------|
| 0 | Null | null value |
| 1 | Bool | true/false |
| 2 | Int | 32-bit signed integer |
| 3 | Float | 64-bit floating point |
| 4 | String | UTF-8 string |
| 5 | List | dynamic array |
| 6 | Dict | key-value map |
| 7 | Object | class instance |
| 8 | Function | function closure |
| 9 | Native | C/native function |

### Instructions

**Stack Operations:**
- `push` - push constant to stack
- `pop` - pop value
- `dup` - duplicate top of stack
- `swap` - swap top two values

**Register Operations:**
- `load` - load from register
- `store` - store to register
- `load_fast` - load local (optimized)

**Control Flow:**
- `jmp` - unconditional jump
- `jz` / `jnz` - conditional jump
- `call` - call function
- `ret` - return from function

**Arithmetic:**
- `add`, `sub`, `mul`, `div`, `mod`

**Comparison:**
- `eq`, `neq`, `lt`, `gt`, `lte`, `gte`

**Logical:**
- `and`, `or`, `not`

**Object Operations:**
- `new` - create object
- `index_get` - list/dict index read
- `index_set` - list/dict index write
- `member_get` - object member read
- `member_set` - object member write
- `invoke` - method call

### Instruction Format

```
[opcode: 1 byte][a: 1 byte][b: 2 bytes]
```

- Single-byte opcode
- Optional operands a, b (register indices or immediate values)
- 4-byte aligned for fast dispatch

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