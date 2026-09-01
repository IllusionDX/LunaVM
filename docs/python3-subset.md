# LunaScript — Python 3 Subset for Embedding

**Goal:** an *embeddable* scripting engine with Python 3 semantics (a defined
subset) on top of the existing register VM. Door left open for JS later (covered
by QuickJS; not a priority). Baseline JIT in a late phase.

**Guiding principle:** "runs Python code of the supported subset", not "is
CPython". Inspired by MicroPython (subset, not a fork).

> **Living document.** Data below reflects a code audit at
> `python-subset @ a5d43f2` (working tree). Items marked **Verified** were run
> through `pyluna.exe`; the `tests_py` suite (14 tests) passes 14/14, and the
> LunaScript suite passes 77/77.
>
> **VM architecture:** the language-agnostic core + per-language frontend design
> (Luna as the first frontend, Python as the second) is in
> [`architecture.md`](./architecture.md). This spec is the *feature scope*; that
> doc is the *engine design* that makes the scope achievable without forking.

---

## 1. Status: Done vs Plan

### Done (verified)

#### Value model & core
- **NaN-boxing value model:** `null`, `bool`, `int32` immediates, boxed
  arbitrary-precision `int` (PyLong-style, base 2^30 — `src/py/bigint.c`),
  native IEEE-754 `float64`, `str`, `list`, `dict`, heap objects (`value.h`).
- **Interned strings** (pointer-equality comparison).
- **Insertion-ordered dict** (Python 3.7+ semantics) with small-object
  optimization (up to 4 inline entries).
- **Mark-sweep GC** + finalizers.
- **Embeddable C-API** (`api.h`): language-agnostic core, frontend-registered
  builtins/modules.

#### Object model (all verified in `src/py/object.c/.h`)
- `ObjString`, `ObjList` (SOO, 4 inline elems), `ObjTuple` (single-allocation,
  immutable), `ObjDict`, `ObjInstance`, `ObjClass`, `ObjFunction`,
  `ObjClosure`, `ObjUpvalue`, `ObjBoundMethod`, `ObjModule`, `ObjBuffer`
  (internal only — not exposed to scripts), `ObjBigInt`, `ObjRange`,
  `ObjRangeIter`, `ObjSlice`.
- **Single inheritance** (`ObjClass.base`), prototypes, field slot maps.
- **Canonical virtual classes** for immediates: `int`, `float` — number methods
  (`bit_length`, `is_integer`, `__int__`/`__float__`/`__bool__`/`__index__`,
  `real`/`imag`, `__class__`) resolve without boxing.

#### Frontend (`src/py/`)
- **Lexer/parser/compiler**: full statement/expression grammar for the subset;
  f-strings, docstrings, type hints on params + return (parsed, ignored —
  PEP 484 syntax), `elif`, lambda, multiple assignment/unpacking
  (`a, b = b, a`).
- **`try`/`except`/`finally`** with `raise X`, `except X as e`; zero-cost
  try/catch via static `ExceptionEntry` table (see `dc44093`).
- **List comprehensions** `[e for x in it if c]`.
- **`super()`** (chained base calls), `isinstance()` with tuples of types.
- **`switch`/`case`/`default`** statement (extension, not Python).
- **Exception hierarchy**: `Exception`, `TypeError`, `ValueError`,
  `KeyError`, `IndexError`, `AttributeError`, `RuntimeError`,
  `ArgumentError`, `OverflowError`.
- **`/` true division, `//`, `**`, `%=`, `//=`, `**=`, `is` / `is not`,
  `in` / `not in`** operators.
- **Arbitrary-precision `int`** (was "bignum phase 2" — **done**): literals,
  arithmetic, bitwise ops, shifts, divmod, comparisons, `int()`/`str()`/`float()`
  conversion, `bit_length`, `range` over bigints, pow with negative exp,
  hex/oct/bin via string formatting. Verified in `test_py_bigint.py` (~100 cases).

#### Regex engine (all verified — `src/common/regex/` + `src/py/stdlib/stdlib_re.c`)
- **Self-contained C engine, zero VM coupling**: compiles to flat bytecode with
  absolute-target `SPLIT`/`JUMP` (Pike-VM style, per Russ Cox's
  "Regular Expression Matching: the Virtual Machine Approach").
- Quantifier expansion to canonical shapes: `X*`, `X+`, `X?`,
  `X{m,n}` → m linear copies + (n-m) optional copies or an `X*` loop.
- **Empty-loop guard per repeat context** (CPython's `rep->last_ptr` /
  Russ Cox's progress rule): zero-width body iterations are discarded and the
  backtrack takes the pre-iteration exit fork — captures match CPython 3.7+
  behavior (`re.match(r'(a*)*', 'aaa').group(1) == 'aaa'`).
- Case-insensitive (`re.I`, `(?i)`, `(?i:...)`) and dotall/multiline flags
  folded at compile time.
- **Captures**: thread-local spans (CPython's "marks"), restored by the LIFO
  backtrack; named groups `(?P<n>...)` / `(?<n>...)`; backrefs `\1`, `(?P=name)`.
- Lookahead `(?=)`/`(?!)`; lookbehind `(?<=)`/`(?<!)` with fixed-width fast
  path + variable-width brute force.
- **Module API**: `match`, `search`, `fullmatch`, `findall` (whole match, 1
  group, or tuple of groups), `sub`, `split` (with `maxsplit`), `escape`,
  `compile` with LRU cache in C (no C pointers stored in `ObjInstance`).
- **Match methods**: `group(i | "name")`, `start()`, `end()`, `span()`,
  `groups()`.
- **Pattern methods**: `match`, `search`, `fullmatch`, `findall`, `sub`,
  `split` (same natives as module functions — identical arg layout).

#### Stdlib (verified imports)
`re`, `enum`, `math`, `json`, `os`, `time`, `io`, `random`, `string`, `socket`.
`buffer` and `noise` were **removed** (not Python 3 stdlib); `net` was renamed
to `socket` (Python 3 name).

- `enum` (Python 3-style): `class Color(Enum):` with class-level members and
  `auto()`; per-class auto counter; member access returns values.
- `string` constants per Python 3.11: `ascii_letters`, `ascii_lowercase`,
  `ascii_uppercase`, `digits`, `hexdigits`, `octdigits`, `punctuation`,
  `printable`, `whitespace`.

### Builtins (verified via REPL probe)
Present: `print`, `input`, `len`, `range`, `repr`, `type`, `isinstance`,
`str`, `int`, `float`, `abs`, `ord`, `chr`, `slice`, `clock`, `gc`, `gc_info`,
`isnan`, `isinf`, `_build_tuple`.

**Missing** (still null in the REPL): `min`, `max`, `sorted`, `enumerate`,
`bool`, `sum`, `divmod`, `zip`, `all`, `any`, `map`, `filter`, `hash`,
`id`, `getattr`, `setattr`, `hasattr`, `callable`, `format`, `vars`, `round`.

### Not implemented (plan / phase 2+)
- **Descriptors**: `__get__` / `__set__` / `__delete__`, `property`.
- **Metaclasses** (`type(x)` exists as a builtin; metaclass protocol absent).
- **`async`/`await`** + event loop.
- **Decorators**.
- **Generators** (`yield`) and coroutines.
- **Multiple inheritance** (parser accepts a single base).
- **Cyclic GC** (cycle detection for long-running scripts).
- **`with`** / context managers (`__enter__`/`__exit__`) — no `TOK_WITH`.
- **Dict/set comprehensions**.
- **`set` / `bytes` / `bytearray` / `complex`** types.
- **Dunder protocols**: `__getattr__`/`__setattr__` on classes, operator
  overloading via dunders (`__add__`, `__eq__`, `__len__`, `__getitem__`...).
- **Baseline JIT** (hot bytecode → assembly via a simple LIR).
- **Fuller C FFI** and **sandboxing**.
- Stdlib gaps: `itertools`, `functools`, `collections`, `datetime`,
  `unittest`, `sys`, `collections`, etc.

### Regex — nice-to-haves / known gaps
- **`finditer()`** — missing (AttributeError). Findall exists; finditer should
  yield Match objects.
- **`Match.re` / `Match.string` / `Pattern.pattern` / `Pattern.flags`
  attributes** — missing (AttributeError on attribute access).
- **`sub()` group references** — no `\1`, `\g<name>`, or callable `repl`
  support (replacement is literal).
- **Posessive quantifiers** `X*+`, `X{n}+` — no support.
- **Atomic groups** `(?>...)` — no support.
- **Conditional groups** `(?(1)yes|no)`, `(?(name)...)` — no support.
- **`\g<1>` / `\g<name>` in sub** — no support.
- **Unicode `\w`/`\d`/`\b`** — ASCII only (engine is byte/ASCII).
- **`\N{...}` named unicode escapes, `\p{...}`/`\P{...}` unicode classes** — no.
- **Recursive patterns** `(?R)`, `(?1)` — no (recursive backtracking only via
  C recursion in lookarounds).
- **Full multiline `^`/`$` edge cases** with `\n` in classes, `\Z` vs `\z`.
- **`re.X` (verbose) in inline `(?x:...)` scope** — parsed as no-op flag;
  verbose whitespace stripping works via `skip_x()` only when the flag is set
  before the sequence.
- **`Match.lastindex`, `Match.lastgroup`** — missing.
- **`Pattern.split` with named-group retention in output** (Python 3.7+ keeps
  captured separators) — split drops captured groups.
- **`findall` zero-width edge cases** — Python 3.7+ semantics for empty matches
  adjacent to non-empty ones not fully replicated.
- **Performance**: no memoization of repeated subpattern failures; no
  prefix/charset scan optimization (CPython's `_compile_info`); linear scan
  per position in `re_search`.
- **Error messages** don't cite pattern position (CPython reports
  `at position N`).

---

## 2. Python 3 Subset — what's IN

### Types
| Type | Status | Note |
|---|---|---|
| `int` | **Done** | arbitrary precision (PyLong-style base 2^30) |
| `float` | Done | native double; `is_integer` + conversion dunders |
| `bool` | Done | maps to `int` virtual class |
| `None` | Done | |
| `str` | Done | interned |
| `list` | Done | inline SOO (4 elems) |
| `dict` | Done | insertion-ordered, SOO |
| `tuple` | **Done** | single-allocation immutable; `_build_tuple` + tuple literals |
| `range` | **Done** | lazy O(1); bigints supported; `range` iter over bigints |
| `slice` | **Done** | `slice()` builtin; `[a:b:c]` syntax |
| `function` / `lambda` | Done | |
| `class` | Done | single inheritance |
| `module` | Done | via `import` |
| `exception` | Done | hierarchy with `Exception` base |
| `enum` | **Done** | via `enum` module (Python 3 style) |
| `set` | Plan | |
| `bytes` | Plan | |
| `complex` | Plan | `real`/`imag` attrs exist on numbers only |

### Syntax / constructs
- Functions, parameters, defaults, `*args` (kwargs thunks exist —
  `bind_keyword_arguments` in the MOP), `return`.
- Classes with inheritance (single), `super()`, methods, docstrings.
- `for` / `while` / `break` / `continue` / `else`-clauses on loops.
- `if` / `elif` / `else`.
- `try` / `except` / `finally` / `raise`.
- f-strings (`f"..."`).
- List comprehensions with `if` filter.
- `lambda`.
- Multiple assignment / unpacking (`a, b = b, a`).
- Type hints on params/returns (parsed, ignored); `x: int = 5` variable
  annotations — pending (`python-frontend-delta.md`).
- `import` / `from ... import ...` (single module per statement —
  `import a, b` is NOT supported, verified: parse error).
- Boolean ops `and`/`or`/`not`, ternary `x if c else y`, `is`/`is not`,
  `in`/`not in`, chained comparisons.
- `switch`/`case`/`default` (extension, not Python).
- Decorators, generators, `async`/`await`, `with` → **phase 2**.

### Semantics
- Dynamic typing, pass-by-object (reference).
- Attribute lookup: instance → class → base chain.
- Operator semantics: `/` true division, `//` floor division, `**` power,
  bigint promotion on int32 overflow.
- `import` resolves own modules (not external CPython stdlib); built-in
  modules resolved via a module cache.

---

## 3. Work sequence (proposal)
1. ~~Close the "Done" column by auditing the "To confirm" items~~ — done.
2. ~~Frontend: parser/compiler aligned to the subset~~ — done (arithmetic,
   control flow, f-strings, bigints, tuples, ranges).
3. ~~Trimmed stdlib~~ — done for the current set (`re`, `enum` are the
   latest additions). Remaining: expand builtins (`min`/`max`/`sorted`/
   `enumerate`/`bool`/...), then new modules.
4. **Regex nice-to-haves** (see §1 gaps list): `finditer`, sub group refs,
   `Match.re`/`Pattern.pattern` attrs, Unicode classes.
5. **C-API / sandboxing / FFI** (solid base already present).
6. **Baseline JIT** only once the interpreter is stable.

---

## 4. Inherited design notes (don't touch without reason)
- 47-bit NaN-boxing (128 TiB): covers real hardware; don't chase PML5.
- Frontends never expose concrete C object kinds to the VM core; everything
  crosses the MOP vtable (`Type` struct in `value.h`) or native functions.
- Regex engine lives in `src/common/regex/` — shared-ready for other
  frontends; the VM core has zero regex awareness (no opcodes, no types).
- `make_obj` returns `NIL_VAL` on `NULL` (defensive guard).
- The `py` frontend keeps Luna-specific leftovers out: no `vector`/`matrix`/
  `userdata`/`int64`/`enum` object kinds, no `buffer`/`noise`/`net` modules.
