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

> Note: `list`, `dict`, `int`, `float`, `string`, `bool`, and `char` are context-sensitive type-hint keywords. They are valid as identifiers everywhere except after `:` in declarations (e.g., `var x: int = 5`).

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

`[]` is strict and throws on missing keys.
Use `?.[]` when you want safe lookup that returns `null`.

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

### Import / Modules

```luna
import math
print(math.sin(0))

import helper
print(helper.greet("World"))
```

Modules are loaded from the current file's directory or the working directory. Each module gets its own isolated global scope. The module cache prevents duplicate loads within a single VM lifetime.

### Built-in Modules

Luna uses three unified API patterns across all built-in modules:

- **Functional (A):** Pure utility functions and constants — no state, no classes
- **Class (B):** Stateful types instantiated with `new`
- **Factory (C):** High-level constructors + optional low-level class

#### `math`

```luna
import math

math.sin(0)           # 0.0
math.cos(0)           # 1.0
math.sqrt(16)         # 4.0
math.pow(2, 8)        # 256.0
math.floor(3.7)       # 3.0
math.ceil(3.2)        # 4.0
math.round(3.5)       # 4.0
math.abs(-5)          # 5.0
math.clamp(15, 0, 10) # 10.0
math.lerp(0, 100, 0.5)# 50.0
math.sign(-42)        # -1.0
math.deg_to_rad(180)  # 3.14159...
math.rad_to_deg(pi)   # 180.0
math.log(1)           # 0.0
math.exp(1)           # 2.71828...

print(math.pi)        # 3.141592653589793
print(math.tau)       # 6.283185307179586
print(math.e)         # 2.718281828459045
print(math.inf)       # Infinity
```

> **Pattern A — Functional.** No classes, no state. Random number generation moved to `random` module.

#### `time`

```luna
import time

print(time.now())          # Unix timestamp in seconds (float)
print(time.ticks_usec())   # Microseconds since VM started
print(time.ticks_msec())   # Milliseconds since VM started
time.sleep(250)            # Sleep for 250ms
```

#### `os`

```luna
import os

# --- Filesystem ---
os.listdir(".")                      # List directory contents
os.getcwd()                          # Current working directory
os.chdir("/tmp")                     # Change directory
os.mkdir("new_dir")                  # Create directory
os.rename("old.txt", "new.txt")      # Rename file/dir
os.remove("temp.txt")                # Delete file (returns bool)
os.exists("data.txt")                # Check if path exists
os.stat("file.txt")                  # File metadata (dict)

# --- File I/O ---
var f = os.open("data.txt", "rb")    # Open for reading
print(f.read_all())                   # Read entire file
f.close()

var w = os.open("out.txt", "w")      # Open for writing
w.write("hello")
w.flush()
w.close()

# Convenience wrappers
var text = os.read_file("data.txt")  # Open, read, close
os.write_file("out.txt", "hello")    # Open, write, close
os.append_file("log.txt", "line\n")  # Append line

# --- System ---
print(os.platform)      # "win32" or "posix"
print(os.sep)           # "\" or "/"
print(os.pathsep)       # ";" or ":"
os.getenv("HOME")       # Environment variable (returns null if missing)
os.setenv("KEY", "val") # Set environment variable
os.args()               # Host argv as list
os.exit(0)              # Exit process
os.execute("cmd")       # Run command, return exit code
os.getpid()             # Process ID
os.hostname()           # Machine hostname
os.username()           # Current user name
os.tmpdir()             # Temp directory path
os.path_join("a", "b")  # Join path segments
```

> **Pattern A — Functional.** File objects created via `os.open()`.

#### `io`

```luna
import io
import os

var f = os.open("data.txt", "rb")
var content = io.read_all(f)    # Read entire reader
f.close()

var src = os.open("in.txt", "rb")
var dst = os.open("out.txt", "w")
io.copy(dst, src)               # Copy all bytes src -> dst
src.close()
dst.close()

var r, w = io.pipe()            # In-memory pipe
w.write("hello")
w.close()
io.read_all(r)                   # "hello"
```

> **Pattern A — Functional.** Stream utilities that work on any object with `read()`/`write()`/`close()`.

#### `json`

```luna
import json

var obj = json.parse('{"key": "value"}')  # Parse JSON string
var text = json.encode(obj)               # Encode to JSON string

json.parse("[1, 2, 3]")  # Returns list
json.encode([1, 2, 3])   # "[1, 2, 3]"
```

> **Pattern A — Functional.** Direct functions, no container class.

#### `buffer`

```luna
import buffer

var b = buffer.new(64)               # Empty buffer, capacity 64
var b2 = buffer.from_string("hello") # Buffer from UTF-8 string
b.read_byte()                        # Read byte
b.read_short()                       # Read 16-bit
b.read_int()                         # Read 32-bit
b.read_long()                        # Read 64-bit
```

> **Pattern A — Functional.** Direct constructors, no container class.

#### `string`

```luna
import string

string.from_byte(65)           # "A"

# Constants (Python-style)
string.ascii_letters           # "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
string.digits                  # "0123456789"
string.punctuation             # "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
string.whitespace              # " \t\n\r\x0b\x0c"
string.hexdigits               # "0123456789abcdefABCDEF"
```

> **Pattern A — Functional.** Constants on the module; methods like `split`, `trim`, `upper` go on string instances.

#### `random`

```luna
import random

var rng = new random.Random(12345)    # Seeded generator
rng.int(0, 100)                       # Random int in [0, 100]
rng.float()                           # Random float in [0, 1)
rng.seed(999)                         # Re-seed
rng.pick([1, 2, 3, 4])               # Random element
rng.shuffle([1, 2, 3, 4])            # Shuffle in-place
```

> **Pattern B — Stateful class.** PCG and Xorshift generators, independent of C `rand()`.

#### `noise`

```luna
import noise

var perlin = new noise.Perlin(42)
var v = perlin.sample(1.5, 2.5)       # Perlin noise at (x, y)

var simplex = new noise.Simplex(42)
simplex.sample(1.5, 2.5, 3.5)         # 3D simplex noise

var voronoi = new noise.Voronoi(42, 2.0)
voronoi.sample(3.0, 4.0)              # Cellular noise
voronoi.edge(3.0, 4.0)                # Edge detection (Voronoi only)
```

> **Pattern B — Three independent stateful classes.** Each algorithm is its own class with `_init(seed)` and `sample(x, y [, z])`.

#### `net`

```luna
import net

# High-level API (recommended)
var conn = net.dial("tcp", "google.com", 80)
conn.send("GET / HTTP/1.0\r\n\r\n")
var resp = conn.recv(4096)
conn.close()

var listener = net.listen("tcp", 8080)
var client = listener.accept()
client.send("hello")

var ip = net.resolve("google.com")    # DNS lookup

# Low-level API (raw sockets)
var sock = new net.Socket(net.IPV4, net.TCP)
sock.connect("host", 443)

# Constants
net.IPV4    # = 2 (AF_INET)
net.IPV6    # = 23 (AF_INET6)
net.TCP     # = 1 (SOCK_STREAM)
net.UDP     # = 2 (SOCK_DGRAM)
```

> **Pattern C — Factory.** `dial`/`listen` for 95% of use cases. `new net.Socket(...)` for raw control.

### Error Handling

```luna
try:
    var result = divide(10, 0)
except as e:
    print("Error: " + e)

try:
    risky()
except ValueError:
    print("Value error caught")
except as e:
    print("Other error: " + e)
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
| **Module/import** | `import x` with module cache and global isolation. `from x import y` and `from x import *` fully functional. |
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
