# Luna Programming Language

## Overview
Luna is a statically-typed systems programming language with indentation-based syntax and C-style type declarations.

## Design Principles

- **Strong static typing** - Types are enforced at compile time, not optional hints
- **Indentation-based blocks** - No braces; structure is determined by whitespace
- **C-style familiarity** - Prefix types, familiar keywords, readable syntax
- **Minimal ceremony** - Semicolons optional, type inference where obvious
- **V1: Python-style interpreter** - Like GDScript; v2 will move to VM+JIT

---

## Syntax Specifications

### Collections

Luna provides two collection types: fixed-size arrays and dynamic lists.

**Fixed arrays** - size is part of the type, allocated on stack or contiguous memory:

```luna
int[5] numbers = [1, 2, 3, 4, 5]     # size known at compile time
int[] inferred = [10, 20, 30]       # size inferred from literal
string[3] names = ["Luna", "Sol", "Terra"]
```

Array size is immutable after creation. Arrays are accessed with zero-based indexing:

```luna
int first = numbers[0]              # get element
numbers[2] = 100                  # set element
int len = numbers.length            # get size (read-only)
```

**Dynamic lists** - growable, heap-allocated, like Java's ArrayList:

```luna
list<int> scores = [100, 200, 300]    # can grow and shrink
scores.add(400)                       # append to end
scores.insert(1, 150)                 # insert at index
int removed = scores.remove(2)       # remove and return element at index
scores.pop()                          # remove and return last element
scores.clear()                        # remove all elements
int len = scores.length               # get current size
```

Both collections are type-safe. You cannot add a `float` to a `list<int>` or `float[]`.

**Choosing between them:**
- Use **arrays** when size is known and fixed (performance, memory efficiency)
- Use **lists** when you need to add/remove elements dynamically

### Base Types

Luna provides the following primitive types:

| Type | Description | Example |
|------|-------------|---------|
| `int` | Signed 32-bit integer | `int x = 42` |
| `long` | Signed 64-bit integer | `long count = 1000000` |
| `float` | 32-bit floating point | `float pi = 3.14` |
| `double` | 64-bit floating point | `double precise = 3.14159265359` |
| `bool` | Boolean true/false | `bool active = true` |
| `byte` | Unsigned 8-bit integer (0-255) | `byte flags = 0xFF` |
| `char` | Single Unicode character | `char c = 'A'` |
| `string` | UTF-8 string | `string name = "Luna"` |
| `void` | No value (return type only) | `def f() -> void:` |
| `null` | Null reference | `Player p = null` |

**Null rules (v1):** Classes and reference types can be assigned `null`. Value types (`int`, `float`, `bool`, structs) cannot be null.

```luna
Player p = null      # OK - class is reference type
int x = null        # Error - value type cannot be null
Vector2 v = null    # Error - struct is value type
```

### Escape Sequences

Strings support standard escape sequences:

| Escape | Meaning |
|--------|---------|
| `\\` | Backslash |
| `\"` | Double quote |
| `\'` | Single quote |
| `\n` | Newline |
| `\t` | Tab |
| `\r` | Carriage return |

```luna
string path = "C:\\Users\\Luna"
string msg = "Hello\nWorld"
```

### Types

Types are written prefix-style, before the identifier:

```luna
int x = 5
float y = 3.14
string name = "Luna"
bool active = true
```

Complex types follow the same pattern:

```luna
map<string, int> data
Pair<int, float> coordinates
```

### Variable Declarations

Explicit types (C-style):

```luna
int count = 0
float total = 0.0
```

Type inference with `var`:

```luna
var count = 0           # inferred as int
var name = "Alice"      # inferred as string
var items = [1, 2, 3]  # inferred as list<int>
```

Use `const` for immutable values:

```luna
const float PI = 3.14159
const string GREETING = "Hello, Luna"
```

`const` and `var` variables must be initialized at declaration time.

### Functions

Functions are declared with `def`, return type follows the `->` arrow:

```luna
def calculate_damage(int base, float crit) -> float:
    float total = base * crit
    return total
```

**Colon requirement:** The colon `:` immediately follows the return type and introduces the indented block body.

**Same-line bodies:** Simple one-liners may follow the colon (style-permitted but discouraged for complex logic):

```luna
def add(int a, int b) -> int: return a + b
```

**Multi-statement bodies:** Must be on new lines with consistent indentation:

```luna
def compute(int x, int y) -> int:
    int a = x * 2
    int b = y + 3
    return a + b
```

### Structs

Structs are value types (copied on assignment):

```luna
struct Point:
    int x
    int y

struct Config:
    string name
    int timeout
    bool enabled
```

**Empty blocks:** Use the `pass` keyword:

```luna
struct Placeholder:
    pass
```

### Classes

Classes are reference types (shared on assignment). Support single inheritance:

```luna
class Entity:
    string name
    int health

    def _init(string name, int health):
        self.name = name
        self.health = health

    def take_damage(int amount) -> void:
        self.health = self.health - amount

    def is_alive() -> bool:
        return self.health > 0

class Player extends Entity:
    int score

    def _init(string name):
        super(name, 100)
        self.score = 0

    def add_score(int points) -> void:
        self.score = self.score + points
```

**Constructor:** `def _init()` - called automatically by `new`
**Parent constructor:** Call with `super(args)` as first statement in child constructor
**Instantiation:** Use `new` keyword: `Player player = new Player("Hero")`
**Self-reference:** Use `self` keyword for explicit field/method access

**Structs vs Classes:**
- **Structs:** Value types, no inheritance, no methods (v1), stack-allocated
- **Classes:** Reference types, single inheritance, methods, heap-allocated with ARC

**Visibility (v1):** All methods and fields are public. Use underscore prefix (e.g., `_internal_method`) as a convention for internal APIs.

### Enums

Enums define a set of named constants. Values auto-increment from 0 unless specified:

```luna
enum Status:
    PENDING         # value = 0
    RUNNING = 10    # value = 10
    COMPLETED       # value = 11 (auto)

enum Color:
    RED = 0
    GREEN = 1
    BLUE = 2
```

Access enum values with dot notation: `Status.PENDING`, `Color.RED`

### Indentation Rules

- **Tabs or spaces** - Choose one per file, never mix
- **Consistency required** - Mixed tabs and spaces raise a compile-time error
- **Four spaces recommended** - Standard, but any consistent indentation works
- **Significant indentation** - Block level is determined by the indentation of the first statement in the block

### Semicolons

Semicolons are optional. Use them to separate multiple statements on one line:

```luna
int x = 5
int y = 10

# Or with semicolons:
int x = 5; int y = 10
```

**Auto-insertion:** Newlines after expression tokens are treated as implicit semicolons (Go-style).

### Multi-line Expressions

Expressions that span multiple lines must be wrapped in parentheses. Inside parentheses, newlines are ignored and expressions can span multiple lines:

```luna
def long_calc() -> int:
    int result = some_function(
        arg1,
        arg2,
        arg3
    )
    return result

def another() -> int:
    int total = (
        base +
        bonus +
        multiplier
    )
    return total
```

Parentheses make the continuation explicit and unambiguous with indentation-based blocks.

### Operators

**Arithmetic:** `+`, `-`, `*`, `/`, `%` (modulo)

**Comparison:** `==`, `!=`, `<`, `>`, `<=`, `>=`

**Logical:** `and`, `or`, `not` (word-based for readability)

**Assignment:** `=`, `+=`, `-=`, `*=`, `/=`

**Bitwise:** `&`, `|`, `^`, `<<`, `>>`, `~`

### Control Flow

If statements:

```luna
if damage > threshold:
    trigger_alert()
    reduce_health()
```

While loops:

```luna
while counter > 0:
    process(counter)
    counter = counter - 1
```

For loops:

```luna
for i in range(0, 10):
    print(i)
```

**Loop control:** `break` to exit early, `continue` to skip to next iteration

```luna
for i in range(0, 100):
    if i % 2 == 0:
        continue
    if i > 50:
        break
    print(i)
```

Switch statements:

```luna
switch status:
    case Status.PENDING:
        print("Waiting...")
    case Status.RUNNING:
        print("Active!")
    default:
        print("Done")
```

- Use `case` for specific values
- Use `default` for the catch-all (optional)
- Falls through without breaks between cases

### Return Statements

Explicit `return` keyword required (no implicit returns):

```luna
def square(int x) -> int:
    return x * x
```

### Comments

Single-line comments (implementation-defined):

```luna
# This is a comment
int value = 42 # end-of-line comment
```

Multi-line comments with triple quotes:

```luna
"""
This is a
multi-line comment
"""

'''
Also works with
single quotes
'''
```

---

## Naming Conventions

Functions and variables: `snake_case`
Structs and types: `PascalCase`
Constants: `UPPER_SNAKE_CASE` recommended

---

## Example Program

```luna
struct Vector2:
    float x
    float y

def magnitude(Vector2 v) -> float:
    return sqrt(v.x * v.x + v.y * v.y)

def main() -> int:
    Vector2 pos = Vector2{x: 3.0, y: 4.0}
    float len = magnitude(pos)
    print(len)
    return 0
```

---

## Decisions

| Feature | Decision |
|--------|----------|
| **Null handling** | `T?` nullable types (post-v1, requires generics) |
| **Memory management** | ARC; v1 is Python-style interpreter (like GDScript), v2 moves to VM+JIT |
| **Module/import** | `import module` (Python-style) |
| **Generics syntax** | `map<string, int>` (lowercase types, consistent with base types) |
| **Pattern matching** | Skip for v1 |
| **Operator overloading** | Skip for v1 |
| **Error handling** | Exceptions |
| **Semicolon insertion** | Auto-insert (Go-style): newline after expression token = implicit semicolon |
| **Loop control** | `break`, `continue` keywords |
| **For loop** | `for i in range(start, end):` (Python/GDScript style) |
| **Enums** | `enum Name: VARIANTS` with optional explicit values |
| **Switch** | `switch expr: case values: ... default:` |

---

## Open Questions

- Generic implementation details (v2)
- Abstract classes/interfaces (skip for v1)
- Build system / package manager
- Standard library scope

---

## Notes

Luna prioritizes readability and familiar syntax while enforcing strong static typing. The indentation-based approach reduces visual noise compared to brace-delimited languages, while the prefix type declarations maintain C-family familiarity.

## Implementation Notes / Gotchas

### Struct Copy Semantics
Since structs are value types, passing them to functions copies the entire struct. For large structs, this may be expensive. Consider using classes for large data structures, or passing by reference (post-v1 feature).

**Example:**
```luna
struct Vector3:
    float x
    float y
    float z

def process(Vector3 v) -> void:  # v is a copy of the struct
    v.x = 999                   # does not affect original
```

### Semicolon Auto-Insertion Rules
Go-style semicolon insertion only inserts semicolons when a line ends with a token that could complete a statement. Tokens that prevent auto-insertion include:
- Opening bracket: `(`, `[`, `{`
- Binary operators: `+`, `-`, `*`, `/`, `==`, `and`, `or`, etc.
- Comma: `,`
- Period: `.` (for method chaining)

**Always use parentheses for multi-line expressions** to avoid ambiguity.

### Struct Literal Syntax
The `Type{field: value}` syntax creates a new struct instance. This is a compile-time operation for structs (stack allocation), but a runtime operation for classes (heap allocation via `new`).