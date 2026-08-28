# LunaScript — Python 3 Subset for Embedding

**Goal:** an *embeddable* scripting engine with Python 3 semantics (a defined
subset) on top of the existing register VM. Door left open for JS later (covered
by QuickJS; not a priority). Baseline JIT in a late phase.

**Guiding principle:** "runs Python code of the supported subset", not "is
CPython". Inspired by MicroPython (subset, not a fork).

> Living document. The **Done** column reflects a code audit at
> `lunascript @ 69bf37d`. Unverified items are marked **To confirm** to avoid
> confusing "planned" with "implemented".

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
- Full `try/except/finally` (there is `exception_class` and `TryFrame`;
  scope not yet measured).
- Comprehensions (list/dict/set).
- `with` / context managers (`__enter__` / `__exit__`).
- `isinstance` / `super` (`OP_ISINSTANCE` exists; `super()` unverified).
- Available builtins (`print`, `len`, `range`, `type`, `enumerate`, ...).
- `tuple` / `set` as types.
- `f-strings` (seen in tests; full inventory pending).
- Multiple inheritance (only a single `base` today).

---

## 2. Python 3 Subset — what's IN (draft)

### Types
| Type | Status | Note |
|---|---|---|
| `int` | Done (int64-cap) | extend to bignum in phase 2 if required |
| `float` | Done | native double |
| `bool` | Done | |
| `None` | Done | |
| `str` | Done | interned |
| `list` | Done | inline SOO (4 elems) |
| `dict` | Done | insertion-ordered |
| `tuple` | To confirm | |
| `set` | Plan | |
| `bytes` | Plan | |
| `function` / `lambda` | Done / to confirm | |
| `class` | Done | single inheritance |
| `module` | Done | via `import` |
| `exception` | Done | |

### Syntax / constructs
- Functions, parameters, return.
- Classes with inheritance (single today; multiple in phase 2 with C3).
- `for` / `while` / `break` / `continue`.
- `if` / `elif` / `else`.
- `try` / `except` / `finally` (scope to confirm).
- `with` (phase 2: context managers).
- `f-strings` (inventory to confirm).
- Comprehensions (to confirm).
- `lambda` (to confirm).
- Multiple assignment / unpacking (to confirm).
- Decorators, generators, `async`/`await` → **phase 2**.

### Target core builtins
`print`, `len`, `range`, `isinstance`, `type`, `enumerate`, `abs`, `min`,
`max`, `sorted`, `repr`, `str`, `int`, `float`, `bool`. (Confirm which exist.)

### Semantics
- Dynamic typing, pass-by-object (reference).
- Attribute lookup: instance → class → MRO.
- Dunder methods in phase 2 (once descriptors land).
- `import` resolves own modules (not external CPython stdlib).

---

## 3. Work sequence (proposal)
1. **Close the "Done" column** by auditing the "To confirm" items (parser/VM).
2. **Frontend:** align parser/compiler to the subset's Python 3 semantics.
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
