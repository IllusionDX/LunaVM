# 🌙 LunaVM — LunaScript Runtime

## 🌌 Overview

**LunaVM** is the register-based bytecode virtual machine and reference runtime for **LunaScript**, a dynamically-typed programming language with indentation-based syntax.

LunaScript draws inspiration from **Python** (indentation-based blocks, list comprehensions), **JavaScript** (flexible objects, prototype-style fields), **GDScript** (lightweight class syntax, optional type hints), and **Lua** (register-based VM internals, lightweight closures).

## ✨ Design Principles

- **Dynamic typing** - Types inferred at runtime, optional hints for documentation/optimization
- **Indentation-based blocks** - No braces; structure is determined by whitespace
- **Minimal syntax** - Clean, readable, low ceremony
- **Flexible** - Objects like JavaScript, collections like Python

---

## 📜 Syntax Specifications

### Variable Declarations

Two declaration keywords:

```luna
var count = 0                    # mutable, block scoped
const VERSION = "1.0.0"           # immutable after assignment
```

**Type hints** are optional and currently parsed but ignored (whitespace around `:` is flexible):

```luna
var x = 5                       # no hint
var x: int = 5                  # recommended
var x : int = 5                 # also valid
```

> Note: `list`, `dict`, `int`, `float`, `string`, `bool`, and `char` are reserved keywords and cannot be used as variable names. This restriction will be lifted in a future release.

### Base Types

**Boolean:**

| Type | Description | Example |
|------|-------------|---------|
| `bool` | true or false | `var active = true` |

**Integer:**

| Type | Description | Example |
|------|-------------|---------|
| `int` | signed 32-bit integer | `var n = 42` |

**Float:**

| Type | Description | Example |
|------|-------------|---------|
| `float` | 64-bit IEEE-754 (runtime) | `var f = 3.14` |

> `NaN` is a runtime value (`0/0`), not a type hint.

**Other:**

| Type | Description | Example |
|------|-------------|---------|
| `string` | UTF-8 string | `var name = "Luna"` |
| `null` | null value | `var player = null` |

> Basic types above are available now. Granular types (int8, int16, int32, int64, uint8, etc.) are planned for a future release.

### Collections

**Lists** - Dynamic arrays:

```luna
var numbers = [1, 2, 3, 4, 5]
var names = ["LunaScript", "Sol", "Terra"]
var mixed = [1, "hello", true]
```

**Slicing** — Python-style start:stop:step:

```luna
var a = [1, 2, 3, 4, 5]
print(a[1:3])     # [2, 3]
print(a[:3])      # [1, 2, 3]
print(a[2:])      # [3, 4, 5]
print(a[::-1])    # [5, 4, 3, 2, 1]
print(a[::2])     # [1, 3, 5]

var s = "hello"
print(s[1:4])     # ell
print(s[::-1])    # olleh
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
player_data[key] = "Hero"
```

### Operators

**Arithmetic:** `+`, `-`, `*`, `/`, `%` (modulo)

**Comparison:** `==`, `!=`, `<`, `>`, `<=`, `>=`

**Logical:** `and`, `or`, `not`

**Assignment:** `=`, `+=`, `-=`, `*=`, `/=`

**Multiple Assignment / Swap:**

```luna
var a = 1
var b = 2
a, b = b, a       # swap in one instruction
a, b = 10, 20     # parallel assignment
```

**Bitwise:** `&`, `|`, `^`, `<<`, `>>`, `~`

### F-Strings

Interpolated strings with `f"..."`:

```luna
var name = "Luna"
print(f"Hello, {name}!")
```

### List Comprehensions

```luna
var squares = [x * x for x in range(1, 6)]
var evens = [x for x in items if x % 2 == 0]
```

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

**Lambdas:** Arrow functions for short expressions:

```luna
var add = (a, b) => a + b
var square = (x) => x * x
var greet = () => "Hello"

var nums = [1, 2, 3, 4, 5]
var doubled = nums.map((x) => x * 2)
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

class Player:
    var score

    def _init(name):
        self.name = name
        self.score = 0

    def add_score(points):
        self.score = self.score + points
```

> Class inheritance (`extends`, `super`) is not yet implemented.

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

**Pass:**

```luna
def placeholder():
    pass
```

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
This is a multi-line comment.
It can span multiple lines.
"""
var value = 42
```

### Import (Placeholder)

```luna
import math
```

> `import` is parsed but not yet functional. Module resolution is planned for 0.3.0.
> `from ... import` syntax is parsed but has no runtime support.

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

## 🏷️ Naming Conventions

Functions and variables: `snake_case`
Classes and types: `PascalCase`
Constants: `UPPER_SNAKE_CASE`

---

## 💻 Example Program

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

## ⚖️ Decisions

| Feature | Decision |
|--------|----------|
| **Typing** | Dynamic with optional hints |
| **Declarations** | `var`, `const` |
| **Scope** | Block scoped (`var` at module level = module scoped) |
| **Pass by reference** | Use return-and-assign |
| **Collections** | Lists (dynamic), Dicts |
| **Module/import** | `import x` (parse-only placeholder). `from x import y` / `from x import *` parsed but not functional. |
| **Error handling** | Exceptions |
| **Semicolon insertion** | Auto-insert like Go |
| **Loop control** | `break`, `continue` |
| **For loop** | `for x in iterable:` |
| **Enums** | Keep |
| **Switch** | Keep |

---

## 📝 Notes

LunaScript is designed to be simple and flexible. The dynamic typing model allows rapid development while optional type hints provide documentation and optimization opportunities.

---

## Roadmap

See [IMPLEMENTATION.md](IMPLEMENTATION.md) for the full release roadmap and technical details.