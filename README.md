# Luna Tree-Walking Interpreter (C Implementation)

A complete tree-walking interpreter for the Luna programming language, written in C.

## Architecture

```
Luna Source File
       |
       v
  Python Lexer (lexer.py)
       |
       v
  Python Parser (parser.py)
       |
       v
  AST to JSON (run_luna.py)
       |
       v
  C Interpreter (luna)
       |
       v
  Program Execution
```

## Files

### Core Interpreter

- **ast.h** - Abstract Syntax Tree node definitions
- **value.h/c** - Value types and memory management (ARC)
- **environment.h/c** - Variable scoping and storage
- **eval.h** - Expression evaluator and statement executor declarations
- **eval_expr.c** - Expression evaluation (binary, unary, literals)
- **eval_call.c** - Function calls, field access, method calls
- **eval_stmt.c** - Statement execution (if, while, for, switch, try)
- **eval_decl.c** - Declaration execution (functions, classes, structs, enums)
- **ast_free.c** - Memory cleanup for AST nodes
- **main.c** - Entry point

### Build System

- **Makefile** - Build configuration for Linux/macOS/Windows

### Driver

- **run_luna.py** - Python driver that lexes, parses, and runs programs

## Building

### Requirements

- GCC or Clang C compiler
- Python 3.x (for the driver)
- Make (optional, for convenience)

### Build Commands

```bash
# Build the interpreter
cd interpreter
make

# Or manually:
gcc -c -Wall -Wextra -std=c99 -O2 -g *.c
gcc *.o -o luna -lm

# Windows:
gcc -c -Wall -Wextra -std=c99 -O2 -g *.c
gcc *.o -o luna.exe -lm
```

## Usage

### Running a Luna Program

```bash
# Using the Python driver (recommended)
python run_luna.py program.luna

# Using the C interpreter directly with JSON
python -c "from run_luna import ast_to_dict; import json; ..." > ast.json
./luna --json ast.json
```

### Example Luna Program

```luna
def greet(string name) -> void:
    print("Hello, " + name + "!")

def add(int a, int b) -> int:
    return a + b

def main() -> int:
    int x = 5
    int y = 10
    int result = add(x, y)
    
    greet("Luna")
    
    if result > 10:
        print("Result is greater than 10")
    
    for i in range(0, 3):
        print(i)
    
    return 0

main()
```

## Features Implemented

### Data Types
- `int` (32-bit signed integer)
- `long` (64-bit signed integer)
- `float` (32-bit floating point)
- `double` (64-bit floating point)
- `bool` (boolean)
- `char` (single character)
- `byte` (8-bit unsigned)
- `string` (UTF-8 strings)
- `null` (null reference)

### Collections
- Fixed arrays: `int[5] arr = [1, 2, 3, 4, 5]`
- Dynamic lists: `list<int> lst = [1, 2, 3]`
- Hash maps: `map<string, int> m = {"a": 1}`

### Control Flow
- `if`/`else` statements
- `while` loops
- `for` loops (with range)
- `switch`/`case` statements
- `break` and `continue`
- `return` statements

### Functions
- Function declarations with typed parameters
- Return type annotations
- Recursive functions
- Native functions (print, input, range, len, type, etc.)

### Classes and Structs
- Struct declarations (value types)
- Class declarations (reference types)
- Single inheritance
- Methods with `self`
- Constructors (`_init`)
- Field access with dot notation

### Enums
- Enum declarations with variants
- Auto-incrementing values
- Custom values: `RUNNING = 10`

### Exception Handling
- `try`/`catch`/`finally`
- `throw` statements
- Exception propagation

### Operators
- Arithmetic: `+`, `-`, `*`, `/`, `%`
- Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
- Logical: `and`, `or`, `not`
- Bitwise: `&`, `|`, `^`, `<<`, `>>`, `~`
- Assignment: `=`, `+=`, `-=`, `*=`, `/=`

## Memory Management

The interpreter uses Automatic Reference Counting (ARC):
- Objects track their reference count
- `retain_obj()` increments count
- `release_obj()` decrements count, frees at zero
- No garbage collector needed

## Limitations

1. No separate C lexer/parser yet (requires Python for lexing/parsing)
2. Limited optimization (tree-walking is inherently slower)
3. No module system implementation
4. Type checking is minimal
5. No debugger or profiler

## Future Enhancements

1. Native C lexer/parser
2. Bytecode VM for better performance
3. JIT compilation
4. Better error messages with line numbers
5. Debugger support
6. Standard library
7. Module imports from files
