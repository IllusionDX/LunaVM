# Luna Programming Language

## Overview
Luna is a dynamically-typed programming language with indentation-based syntax.

## Design Principles

- **Dynamic typing** - Types inferred at runtime, optional hints for documentation/optimization
- **Indentation-based blocks** - No braces; structure is determined by whitespace
- **Minimal syntax** - Clean, readable, low ceremony
- **Flexible** - Objects like JavaScript, collections like Python

---

## Syntax Specifications

### Variable Declarations

Three declaration keywords:

```luna
var count = 0                    # mutable, block scoped
const VERSION = "1.0.0"           # immutable after assignment
```

**Type hints** are optional (whitespace around `:` is flexible):

```luna
var x = 5                       # no hint
var x: int = 5                  # recommended
var x : int = 5                 # also valid
```

### Base Types

**Boolean:**

| Type | Description | Example |
|------|-------------|---------|
| `bool` | true or false | `var active = true` |

**Integer:**

| Type | Description | Example |
|------|-------------|---------|
| `int` | signed integer | `var n = 42` |
| `uint` | unsigned integer | `var u = 42` |

**Float:**

| Type | Description | Example |
|------|-------------|---------|
| `float` | 32-bit IEEE-754 | `var f = 3.14` |
| `double` | 64-bit IEEE-754 | `var d = 3.14159` |
| `NaN` | Not-a-Number (IEEE 754) | `var n = 0/0` |

**Other:**

| Type | Description | Example |
|------|-------------|---------|
| `string` | UTF-8 string | `var name = "Luna"` |
| `null` | null value | `var player = null` |

> V1 types above. Granular types (int8, int16, int32, int64, uint8, etc.) available in V2+.

### Collections

**Lists** - Dynamic arrays:

```luna
var numbers = [1, 2, 3, 4, 5]
var names = ["Luna", "Sol", "Terra"]
var mixed = [1, "hello", true]

numbers.add(6)                   # append
numbers.insert(0, 0)            # insert at index
var removed = numbers.pop()        # remove and return last
var removed = numbers.remove(2)   # remove and return at index
numbers.clear()                 # clear all
var len = numbers.length        # get size
```

**Dicts** - Key-value maps (like Python/JavaScript objects):

```luna
var config = {
    "gravity": 9.8,
    "theme": "Neon-Tundra",
    "spawn_points": [[0, 0], [100, 50], [200, -10]]
}

var theme = config["theme"]       # read
config["difficulty"] = "hard"   # write
var exists = config.has("theme")   # key check
config.remove("theme")          # delete key
var keys = config.keys()       # all keys
var vals = config.values()     # all values
```

**Bracket notation** for dynamic keys:

```luna
var key = "player_name"
var player_data[key] = "Hero"
```

### Operators

**Arithmetic:** `+`, `-`, `*`, `/`, `%` (modulo)

**Comparison:** `==`, `!=`, `<`, `>`, `<=`, `>=`

**Logical:** `and`, `or`, `not`

**Assignment:** `=`, `+=`, `-=`, `*=`, `/=`

**Bitwise:** `&`, `|`, `^`, `<<`, `>>`, `~`

### Escape Sequences

| Escape | Meaning |
|--------|---------|
| `\\` | Backslash |
| `\"` | Double quote |
| `\'` | Single quote |
| `\n` | Newline |
| `\t` | Tab |
| `\r` | Carriage return |

### Functions

Functions are declared with `def`, return type is optional:

```luna
def calculate_damage(base, crit):
    return base * crit

def add(a, b) -> int:
    return a + b

def greet(name) -> string:
    return "Hello, " + name
```

**Colon requirement:** The colon introduces the indented block body.

**Same-line bodies:** Simple one-liners may follow the colon:

```luna
def add(a, b): return a + b
```

**Block bodies:** Must be on new lines with consistent indentation:

```luna
def compute(x, y):
    var a = x * 2
    var b = y + 3
    return a + b
```

### Pass by Reference

Objects and collections (lists, dicts) are passed by reference automatically:

```luna
def add_item(list):
    list.add(99)

var items = [1, 2, 3]
add_item(items)
print(items)  # [1, 2, 3, 99] - modified!
```

For primitives, use return-and-assign:

```luna
def increment(x):
    return x + 1

var x = 5
x = increment(x)
print(x)  # 6
```

### Classes

Classes support single inheritance:

```luna
class Entity:
    var name
    var hp: float

    def _init(name, health: float):
        self.name = name
        self.hp = health

    def take_damage(amount):
        self.hp = self.hp - amount

    def is_alive() -> bool:
        return self.hp > 0

class Player extends Entity:
    var score

    def _init(name):
        super(name, 100)
        self.score = 0

    def add_score(points):
        self.score = self.score + points
```

**Instantiation:** Use `new` keyword:

```luna
var player = new Player("Hero")
player.take_damage(25)
```

**Self-reference:** Use `self` keyword.

### Enums

```luna
enum Status:
    PENDING
    RUNNING = 10
    COMPLETED

enum Color:
    RED = 0
    GREEN = 1
    BLUE = 2
```

Access: `Status.PENDING`, `Color.RED`

### Indentation Rules

- **Tabs or spaces** - Choose one per file, never mix
- **Four spaces recommended** - Standard
- **Significant indentation** - Block level determined by whitespace

### Semicolons

Semicolons are optional:

```luna
var x = 5
var y = 10
```

Or with semicolons:

```luna
var x = 5; var y = 10
```

### Control Flow

**If statements:**

```luna
if damage > threshold:
    trigger_alert()
    reduce_health()
```

**While loops:**

```luna
while counter > 0:
    process(counter)
    counter = counter - 1
```

**For loops:**

```luna
for item in items:
    print(item)

for i in range(0, 10):
    print(i)
```

**Loop control:** `break`, `continue`

```luna
for i in range(0, 100):
    if i % 2 == 0:
        continue
    if i > 50:
        break
    print(i)
```

**Switch statements:**

```luna
switch status:
    case Status.PENDING:
        print("Waiting...")
    case Status.RUNNING:
        print("Active!")
    default:
        print("Done")
```

### Return Statements

Explicit `return` required:

```luna
def square(x):
    return x * x
```

### Comments

Single-line comments:

```luna
# This is a comment
var value = 42
```

Multi-line comments:

```luna
"""
Multi-line
comment
"""
```

### Import

```luna
import math              # import entire module
from math import sin     # import specific
from math import *      # import all
```

### Error Handling

```luna
try:
    var result = divide(10, 0)
catch e:
    print("Error: " + e)

try:
    risky()
catch e:
    print("Error: " + e)
finally:
    cleanup()
```

---

## Naming Conventions

Functions and variables: `snake_case`
Classes and types: `PascalCase`
Constants: `UPPER_SNAKE_CASE`

---

## Example Program

```luna
const VERSION = "1.0.0"

class Vector2:
    var x
    var y

    def _init(x, y):
        self.x = x
        self.y = y

    def magnitude():
        return sqrt(self.x * self.x + self.y * self.y)

def main():
    var pos = new Vector2(3.0, 4.0)
    print(pos.magnitude())

main()
```

---

## Decisions

| Feature | Decision |
|--------|----------|
| **Typing** | Dynamic with optional hints |
| **Declarations** | `var`, `const` |
| **Scope** | Block scoped (`var` at module level = module scoped) |
| **Pass by reference** | Use return-and-assign |
| **Collections** | Lists (dynamic), Dicts |
| **Module/import** | `import x`, `from x import y`, `from x import *` |
| **Error handling** | Exceptions |
| **Semicolon insertion** | Auto-insert like Go |
| **Loop control** | `break`, `continue` |
| **For loop** | `for x in iterable:` |
| **Enums** | Keep |
| **Switch** | Keep |

---

## Notes

Luna is designed to be simple and flexible. The dynamic typing model allows rapid development while optional type hints provide documentation and optimization opportunities.

---

## Roadmap

### V2
- Array slicing and comprehensions

### V3
- Coroutines / async
- JIT compiler
- Type specialization from hints