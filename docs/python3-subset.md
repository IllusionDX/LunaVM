# LunaScript — Python 3 Subset for Embedding

**Goal:** an *embeddable* scripting engine with Python 3 semantics (a defined
subset) on top of the existing register VM. Door left open for JS later (covered
by QuickJS; not a priority). Baseline JIT in a late phase.

**Guiding principle:** "runs Python code of the supported subset", not "is
CPython". Inspired by MicroPython (subset, not a fork).

> Living document. The **Done** column reflects a code audit at
> `lunascript @ 69bf37d`. Unverified items are marked **To confirm** to avoid
> confusing "planned" with "implemented".
>
> The **Python frontend** (`src/py/`) was re-audited at
> `python-subset @ 8751d6a`. Struck-through items were verified by running
> `pyluna.exe` (7/7 `tests_py` tests pass).

> **VM architecture:** the language-agnostic core + per-language frontend design
> (Luna as the first frontend, Python as the second) is in
> [`architecture.md`](./architecture.md). This spec is the *feature scope*; that
> doc is the *engine design* that makes the scope achievable without forking.

---

## 1. Status: Done vs Plan

### Done (verified in code)
- **NaN-boxing value model:** `nil`, `bool`, `int32`, boxed `int64`,
  native IEEE-754 `float64`, `str`, `list`, `dict`, objects (`value.h`).
- **Lua-style embeddable C-API:** `luna_State`, `luna_new_state`,
  `luna_push_*` / `luna_to_*` / `luna_is_*`, `luna_push_cfunction`,
  `luna_get_field` / `luna_set_field` (~40 functions in `luna.c`).
- **`import`:** `TOK_IMPORT` in lexer, `parse_import_declaration`, `module.c`.
  (Resolution scope to be audited.)
- **Insertion-ordered dict** (Python 3.7+-style semantics) via the `order`
  array (`ObjDict`).
- **Interned strings** (pointer-equality comparison).
- **Classes / instances / methods / closures / exceptions:** `ObjClass`,
  `ObjInstance`, `ObjClosure`, `ObjFunction`, `exception_class`, `TryFrame`.
- **Single inheritance** (`ObjClass.base`).
- **Stdlib present:** `math`, `random`, `noise`, `io`, `time`, `os`,
  `buffer`, `string`, `net`, `json`.
- **Mark-sweep GC** + finalizers (`Object.finalizer_next`, etc.).

### Plan (phase 2+)
- **Descriptors:** `__get__` / `__set__` / `__delete__`, `property`.
- **Metaclasses** (and `type(x)`).
- **`async` / `await` + event loop.**
- **Decorators** (sugar over function assignment).
- **Python generators** (`yield`) and coroutines for the game tick.
- **Formal C3 MRO** if multiple inheritance is added.
- **Cyclic GC** (cycle detection for object graphs in long-running scripts).
- **Bignum** (arbitrary-precision `int`) — currently `int64`-cap.
- **Stdlib expansion** oriented toward games.
- **Baseline JIT** (hot bytecode → assembly via a simple LIR).
- **Fuller C FFI** and **sandboxing**.

### To confirm (audit pending)
Re-audited in the Python frontend (`src/py`). Struck-through items were
verified working in `pyluna` (`tests_py` suite + probes below).

- ~~Full `try/except/finally`:~~ `raise X`, `except X as e`, and `finally`
  verified (`test_py_oop.py`, interactive probe). No bare re-raise yet.
- ~~List comprehensions:~~ `[e for x in it if c]` verified
  (`parse_expr.c:33`, `compiler.c:1249`). Dict/set comprehensions absent.
- `with` / context managers (`__enter__` / `__exit__`) — not implemented
  (no `TOK_WITH`, no dunders).
- ~~`super()`~~ — verified (`py.c` `py_super`; `super().__init__()` in
  `test_py_oop.py`). Global `isinstance()` still missing (`OP_ISINSTANCE`
  exists but is only used by `except Type:`).
- Available builtins — verified in `src/py/stdlib/vm_builtins.c`: `print`,
  `len`, `range`, `type`, `str`, `int`, `float` exist; `isinstance`,
  `enumerate`, `abs`, `min`, `max`, `sorted`, `repr`, `bool` do not
  (only `math.abs`).
- `tuple` / `set` as types — absent (`tuple` no type in lexer/AST; both
  `tuple` and `set` are `Plan`).
- ~~f-strings~~ — verified: `f"..."` / `f'...'` (`lexer.c:491`, `fstring.c`);
  used throughout `tests_py`.
- ~~Numbers as virtual objects:~~ immediate `int`/`float`/`bool` resolve
  methods and attributes through canonical classes without boxing —
  `bit_length`, `is_integer`, `__int__`/`__float__`/`__bool__`/`__index__`,
  `real`/`imag`, `__class__` (`test_py_numobj.py`). `bool` maps to `int`.
- Multiple inheritance — not implemented: `src/py/parse_decl.c:103` accepts a
  single base only.

---

## 2. Python 3 Subset — what's IN (draft)

### Types
| Type | Status | Note |
|---|---|---|
| `int` | Done (int64-cap) | virtual-object methods: `bit_length`, `real`/`imag`, `__int__`/`__index__`/`__float__`/`__bool__`; bignum in phase 2 |
| `float` | Done | native double; `is_integer` + conversion dunders via virtual class |
| `bool` | Done | |
| `None` | Done | |
| `str` | Done | interned |
| `list` | Done | inline SOO (4 elems) |
| `dict` | Done | insertion-ordered |
| `tuple` | Plan | absent from lexer/AST |
| `set` | Plan | |
| `bytes` | Plan | |
| ~~`function` / `lambda`~~ | Done | lambda keyword verified in `src/py` |
| `class` | Done | single inheritance |
| `module` | Done | via `import` |
| `exception` | Done | |

### Syntax / constructs
- ~~Functions, parameters, return.~~
- ~~Classes with inheritance~~ (single today; multiple in phase 2 with C3).
- ~~`for` / `while` / `break` / `continue`.~~
- ~~`if` / `elif` / `else`.~~ (`elif` added in the Python frontend)
- ~~`try` / `except` / `finally`.~~ (no bare re-raise yet)
- `with` (phase 2: context managers).
- ~~f-strings.~~
- ~~List comprehensions~~ (dict/set comprehensions: phase 2).
- ~~`lambda`.~~
- ~~Multiple assignment / unpacking.~~ (`a, b = b, a` verified)
- Type annotations per [PEP 526](https://peps.python.org/pep-0526/):
  function params + `->` return hints parsed & ignored (PEP 484 syntax).
  Variable annotations `x: int = 5` pending — see
  [`python-frontend-delta.md`](./python-frontend-delta.md).
- Decorators, generators, `async`/`await` → **phase 2**.

### Target core builtins
Target: `print`, `len`, `range`, `isinstance`, `type`, `enumerate`, `abs`,
`min`, `max`, `sorted`, `repr`, `str`, `int`, `float`, `bool`.

Verified present in `src/py/stdlib/vm_builtins.c`: `print`, `len`, `range`,
`type`, `str`, `int`, `float`. Still missing: `isinstance`, `enumerate`,
`abs` (only in `math`), `min`, `max`, `sorted`, `repr`, `bool`.

### Semantics
- Dynamic typing, pass-by-object (reference).
- Attribute lookup: instance → class → MRO.
- Dunder methods in phase 2 (once descriptors land).
- `import` resolves own modules (not external CPython stdlib).

---

## 3. Work sequence (proposal)
1. ~~Close the "Done" column by auditing the "To confirm" items~~ (parser/VM
   audit done for `src/py`; see §1).
2. **Frontend:** parser/compiler aligned to the subset — lexical/parser work
   is done (`src/py`); arithmetic semantics done (`/` true division, `//`,
   `**`, `//=`, `**=`). Remaining deltas: builtins, dunders. Tracked in
   [`python-frontend-delta.md`](./python-frontend-delta.md).
3. **Trimmed stdlib** oriented toward games.
4. **C-API / sandboxing / FFI** (solid base already present).
5. **Baseline JIT** only once the interpreter is stable.

---

## 4. Inherited design notes (don't touch without reason)
- 47-bit NaN-boxing (128 TiB): covers real hardware; don't chase PML5.
- `values_equal` compares `int64` in the integer domain (fixed in 69bf37d).
- `IS_INT64` is a heap-object kind check (`obj->type == OBJ_INT64`) in
  `src/luna/object.h` — no inline type bits in the `Value`.
- `make_obj` returns `NIL_VAL` on `NULL` (defensive guard).
