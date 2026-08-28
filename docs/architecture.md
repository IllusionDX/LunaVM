# Luna VM Architecture — Language-Agnostic Core + Per-Language Frontends

**Thesis:** the VM must not know what a `list`, `dict`, `class`, or `Vector` is.
Those are *language* concepts that live in heap objects and their operation
tables (a Meta-Object Protocol). The VM is a managed runtime: a uniform value
representation, a GC, a register machine with *neutral* opcodes, and a dispatch
mechanism that delegates semantics to each object's type.

Result: **Luna is not deleted — it is promoted.** It stops being "the language
the VM knows" and becomes "the first frontend that uses the VM". Python is the
second frontend, on equal footing. JS later, if ever.

> This is the JVM / .NET CLR / GraalVM-Truffle model: the backend is
> language-neutral; each language is a frontend that lowers to a shared IR and
> installs its own type/operation tables and standard library.

---

## 1. Layered architecture

```
Layer 0  Machine:        Value (NaN-boxing) + GC + uniform Object header
Layer 1  Neutral IR:     LOAD/STORE/CALL/GET_ATTR/SET_ATTR/GET_INDEX/
                         SET_INDEX/BINOP/CMP/BRANCH/CLOSURE  (no semantics)
Layer 2  MOP (vtable):   each Type has add/cmp/getitem/getattr/call/tostring/
                         hash/iter...  (semantics per language)
Layer 3  Frontends:      Luna  lowers its AST  -> IR, wires its MOP
                         Python lowers its AST -> IR, wires its MOP
Layer 4  Stdlib:         builtins per language, as ordinary objects
```

The VM never names `list`/`dict`/`str`. It names "HeapObject + operation from
the MOP". Language differences (Python MRO vs JS prototype chain, `__add__` vs
`valueOf`) live entirely in the per-language vtable / runtime library.

---

## 2. Shared core (language-neutral)

Owns nothing language-specific:

- **`Value`** — NaN-boxing with a minimal inline tag set (see §3).
- **Register machine** — fetch/decode/execute of *neutral* opcodes.
- **GC** — mark-sweep + finalizers.
- **MOP dispatch** — opcodes consult an object's `Type` vtable; fast-paths are
  keyed on native-type identity (inline cache), never on inline value tags.
- **Embedding C-API** — the host surface (`luna_*` push/pop/call family) is
  language-neutral; each frontend supplies its own top-level `run_script`.

---

## 3. Value model (minimal)

For a Python-semantics VM, the in-`Value` layer needs only ~4–6 tags. All type
richness lives in the heap `Type`, not in the 64-bit register.

### Encoding note (important)
The tagged values MUST occupy the **NaN space** (top 16 bits = `QNAN_TAG`,
`0x7FF8…`), so they never collide with a finite `double`. The 6 immediate tags
reuse the **existing 3-bit sub-tag space** (bits 0–2) already in `value.h`; heap
objects carry *no* type bits — they are just `QNAN_TAG | ptr`. A plain `double`
is the default (exponent ≠ `0x7FF`). Do **not** place tags with the high bits at
`0` — that collides pointer vs double and is indistinguishable.

### Six-tag model (committed)

The immediates already occupy exactly 6 sub-tags (bits 0–2), so no new tag bits
are needed — the inline object-type field is removed entirely:

```
QNAN-space, low-3-bit sub-tag (bits 0–2):
   0  HeapObject   (malloc-aligned ptr -> low 3 bits = 0)
   1  NIL
   2  TRUE
   3  FALSE
   4  INT          (int32 immediate, payload in bits 3+)
   5  EMPTY        (dict sentinel)
   6  TOMB         (dict sentinel)
   7  (free -> e.g. NotImplemented / Ellipsis for Python)
double (raw):   any value with exponent != 0x7FF  (Float; INT is the small-int)
```

All object typing moves to the heap `Type` (the MOP vtable). There is no
per-value type tag for `list`/`dict`/`str`/… — those are `HeapObject` plus a
heap `Type`.

### Status: implemented

The 6-tag model is in place. What changed vs the old 5-bit scheme:

- Removed from `value.h`: `TYPE_SIGNATURE`, `OBJ_SIGNATURE_MASK`, the `make_obj`
  type split, the 17 inline `IS_X` macros, and the 3→4→5 bit "encoding evolution"
  comment. `make_obj(p) = QNAN_TAG | (uintptr_t)p` (one OR).
- Luna's object model (`ObjType`, `Object`/`Obj*`, the `IS_X` kind predicates,
  constructors/operations) moved to `src/luna/object.h` — the Luna frontend owns
  it. `value.h` keeps only the language-agnostic value model + a TEMPORARY shim
  `#include "luna/object.h"` until the MOP vtable lands (then the core stops
  switching on `ObjType` and dispatches via the frontend vtable).
- Type tests are heap `obj->type` word-compares (`IS_OBJ_KIND`), not 64-bit masks.

### Repository layout (post-split)

```
src/                VM-agnostic core
    value.h         minimal 6-tag value model (no Luna types)
    value.c vm.c    register machine + GC (still references Luna types via shim)
    chunk.* opcode.h vm_opcodes.inc
src/luna/           Luna frontend (first language)
    object.h        ObjType / Obj* / IS_X   (was inline in value.h)
    lexer parser compiler ast module fstring   (Luna source -> bytecode)
    stdlib/         Luna builtins
    luna.c main.c   embeddable C-API + entry
```

### Optimization wins
- `make_obj` drops a shift + conditional (bit 63) → single OR.
- Type checks stop doing a 64-bit `AND` with `OBJ_SIGNATURE_MASK` + 64-bit
  compare; replaced by one word load + compare on `obj->type` (cache-hot after
  the deref the vtable needs anyway).
- Uniform dispatch path → better branch prediction / inline caches.
- Frees bits 47–50 and 63 for future use (not needed now).
- Removes a latent footgun: `AS_OBJ` currently keeps bit 47, which is part of
  the 5-bit type tag — dropping inline type bits removes that class of bug.

### What stays
The QNAN-vs-`double` distinction (`IS_DOUBLE`) and the NaN-normalization to
payload `0x0A` remain — they are intrinsic to NaN-boxing with malloc-aligned
pointers (low 3 bits = 0 in the NaN space would collide with `HeapObject`).
That check is a single clean mask, not the 32-type machinery being removed.

- `SmallInt` — immediate int32/int62 in the NaN payload (loop counters, indexes;
  big ints box to the heap as `HeapObject`).
- `Float` — implicit raw `double`.
- `HeapObject` — universal pointer to any object (`list`, `dict`, `str`,
  `tuple`, `generator`, user instance, module, function, class, …).
- `Singletons` — language-unique immutables (`None`, `True`, `False`,
  `NotImplemented`, `Ellipsis`) and VM sentinels (`EMPTY_SLOT`, `TOMBSTONE`,
  `EXCEPTION_HANDLED`).

### Collapsing Luna's 17 inline tags

| Luna today (17 `ObjType` tags)            | In minimal model                |
|---|---|
| `OBJ_LIST`, `OBJ_DICT`, `OBJ_STRING`, `OBJ_INSTANCE`, `OBJ_CLASS`, `OBJ_BOUND_METHOD`, `OBJ_MODULE`, `OBJ_BUFFER` (bytes), `OBJ_INT64` (bigint) | **`HeapObject`** + `Type` in heap differentiates and carries the vtable |
| `OBJ_FUNCTION`, `OBJ_CLOSURE`, `OBJ_UPVALUE` | `HeapObject` (runtime-structural types; `Type` marks them) |
| `OBJ_ENUM`                                 | heap (Python `Enum` is library-level, not a VM type) |
| `OBJ_VECTOR`, `OBJ_MATRIX`                | **heap** — Luna domain types; confirm they must not consume in-value bits in a Python VM |
| `TAG_NIL` / `TRUE` / `FALSE`, `EMPTY`, `TOMB` | inline **singletons / sentinels** |

Net: **17 tags → ~4–6**. The types still exist; they are no longer discriminated
in the register, only in the heap `Type`.

---

## 4. Dispatch: MOP + inline cache

The opcode carries no language semantics. The fast-path is an inline cache keyed
on *native-type identity*, not an inline value tag. The slow path is the MOP.

```c
/* OP_GET_INDEX */
if (IS_HEAPOBJ(v) && obj_type(v) == LUNA_NATIVE_LIST)
    return ((ObjList*)v)->items[i];          /* Luna fast path */
if (IS_HEAPOBJ(v) && obj_type(v) == PY_NATIVE_LIST)
    return py_list_getitem(v, i);            /* Python fast path */
return obj_type(v)->vtab->getitem(v, i);     /* MOP: subclass / override */

/* OP_ADD */
if (IS_INT(L) && IS_INT(R)) return int_add(L, R);     /* universal immediate */
if (IS_FLOAT(L) || IS_FLOAT(R)) return float_add(L, R);
return left_type(v)->vtab->add(L, R);        /* MOP; fallback to R per language rule */
```

For a subclass (`class CustomList(list)`) or an object with `__getitem__`, the
fast path misses and the MOP runs (`ob_type->tp_as_mapping` in CPython terms).
This is exactly V8 / LuaJIT inline-cache behavior: the common case stays direct,
the rare case pays dispatch. A baseline JIT later devirtualizes hot paths.

---

## 5. Per-language layer (Luna today, Python tomorrow)

Each language owns:

- **Lexer / parser / AST→bytecode compiler** — Luna already has this in `src/`.
- **`Type` objects + vtables** — the MOP implementation: how `+`, `[]`,
  attribute get, and `call` resolve.
- **Stdlib / builtins / domain types** — `Vector`, `Matrix`, `Enum` for Luna;
  `list`/`dict`/`str` semantics + builtins for Python.

The coupling to remove is NOT in the parser — it is in `vm.c` / `value.h`, where
Luna semantics are baked inline (`IS_LIST`, `do_arith`, concatenation, …).

---

## 6. Migration strategy: extract, don't rewrite

Luna survives as a frontend. The work is *extracting* its baked-in semantics
from the VM into its vtable + stdlib, not deleting or re-implementing it.

1. **Generalize `Value`** to the 4–6 tag set (keep the QNAN mechanism).
2. **Introduce `Type` vtable** (`add`/`cmp`/`getitem`/`getattr`/`call`/`tostring`/
   `hash`/`iter`). Seed it from Luna's existing `Object.type`.
3. **Hoist `vm.c` type logic** into Luna's vtable: `do_arith`, `do_cmp`,
   `OP_GET_INDEX`, attribute lookup move from switch-on-tag to vtable calls,
   keeping native-type fast-paths as inline caches.
4. **Move domain/builtin types** (`Vector`, `Matrix`, `Enum`, stdlib) to heap
   `Type` + stdlib; drop their inline tags.
5. **Add the Python frontend** that compiles to the *same* neutral IR and
   installs its own vtable + stdlib. It gets the same status Luna now has.

What NOT to touch: lexer/parser/compiler of Luna (already a frontend); the GC;
the register machine shape.

---

## 7. MOP implementation steps (no shortcuts)

The MOP is built incrementally. Each step is a separate commit with `make` +
smoke test (`luna.exe /tmp/smoke.luna`). No part skips verification.

### Part 1: MOP infrastructure (core + Luna wiring)
- Define the neutral `Type` (vtable) in core (`value.h` or `mop.h`):
  function pointers for `add`, `sub`, `mul`, `div`, `mod`, `cmp`,
  `getitem`, `setitem`, `getattr`, `setattr`, `call`, `tostring`, `hash`, `len`.
- Change `Object.type` from the `ObjType` enum to `Type*` (language-agnostic
  heap object). The `ObjType` enum moves fully into `src/luna/object.h` as Luna's
  internal kind tag (used by GC `free_object_container`, by `IS_OBJ_KIND`,
  and by the fast-path inline caches).
- In `src/luna/object.c` (new file, split from `object.h`): define the concrete
  `Type` instances for Luna (`luna_string_type`, `luna_list_type`, ...), each
  with a vtable that calls the existing Luna-specific logic (e.g., list's
  `getitem` reads from `ObjList->items`).
- Update all constructors (`new_string`, `new_list`, ...) to set
  `obj->type = &luna_X_type` instead of assigning an enum value.
- Update `AS_OBJ` / `IS_OBJ_KIND` macros to work with `Type*`.

### Part 2: Arithmetic / comparison (`vm.c`)
- Replace `do_arith`'s `switch` / `if (IS_LIST(L) && IS_INT(R))` chain with:
  `if (both_small_int) native; else L_type->vtab->add(L_val, R_val);`
- The fast-path for native `int` / `float` stays direct; for objects the vtable
  resolves `__add__` semantics. Luna's vtable for `list` handles repetition,
  for `str` handles concatenation, etc.
- Same pattern for `do_cmp`: `L_type->vtab->cmp(L, R, op_code)` with inline
  cache comparing the `Type*` pointer.

### Part 3: Indexing (`vm.c` opcodes)
- `OP_GET_INDEX`: `L_type->vtab->getitem(L, index)`; `L_type->vtab->getitem` for
  `list` reads `items[i]`; for `dict` does hash lookup.
- `OP_SET_INDEX`: `L_type->vtab->setitem(L, index, value)`.
- Fast-path: `if (L_val->type == &luna_list_type)` native array access; else
  full vtable call (handles subclass / `__getitem__` override).

### Part 4: Attributes (`vm.c` / `luna.c` embedder API)
- `OP_GET_ATTR` / `OP_SET_ATTR`: `L_type->vtab->getattr(L, name_str)` /
  `L_type->vtab->setattr(L, name_str, value)`.
- The `luna_get_field` / `luna_set_field` C-API functions in `luna.c` become
  thin wrappers over the vtable, not direct `ObjDict` / `ObjInstance` lookups.

### Part 5: Call (`vm.c`)
- `OP_CALL`: `fn_type->vtab->call(vm, fn_value, args, nargs)`. The vtable for
  `function` / `closure` / `bound_method` / native C functions handles the
  different call conventions.

### Part 6: Cleanup
- Remove the temporary `#include "luna/object.h"` shim from `value.h` (the core
  is pure; `vm.c` and the frontend include what they need directly).
- Update `docs/IMPLEMENTATION.md` references to `TYPE_SIGNATURE` / 32-type
  encoding (stale after Part 1).
- Confirm the Python frontend spec (`python3-subset.md`) remains valid: Python
  will define its own `Type` instances with its MOP rules (MRO, `__getattr__`),
  targeting the same neutral IR.

No step skips `make` verification. Each part is committed independently.

> **Status (current session):**
> - **Part 1 (infrastructure):** complete — `Type` vtable defined in `luna/object.h`,
>   `Object.type` → `Type*`, `luna/object.c` defines all 17 `Type` instances +
>   `luna_types[]`, constructors (`init_object`) use the table, all `IS_X` macros
>   updated to compare `Type*`, switches (`free_object_container`, `value_to_string`,
>   `get_class`, GC `mark_drain`, `type_name`) updated to `->kind`. Build passes.
> - **Part 2 (arithmetic/comparison/index dispatch):** complete — `do_arith` and
>   `do_cmp` in `vm.c` delegate through `VM*vm` + `Type*` vtable for all heap-object
>   kinds (`string`, `list`, `dict`, `vector`, `matrix`, `instance`, `int64`,
>   defaults). Vtable functions (`luna_string_add`, `luna_string_mul`,
>   `luna_list_add`, `luna_list_mul`, `luna_string_cmp`, etc.) replicate the
>   current Luna semantics. `vm_opcodes.inc` call sites updated to pass `vm`.
>   Build passes; smoke test (`luna.exe smoke.luna`) passes; full regression
>   (`make test`, exit 0) passes.
> - **Part 3 (indexing):** complete — `op_indexget` (`getitem`), `op_indexget_safe`,
>   `op_indexset` (`setitem`) delegate to `Type*` vtable. Opcode keeps original
>   validation (bounds, `IS_INT`, `dict_has`, error messages); data access goes
>   through vtable.
> - **Part 4 (attributes):** complete — `luna_dict_getattr`/`setattr` wired; `luna_get_field`
>   / `luna_set_field` in `luna.c` delegate to `Type*` vtable (`getattr`/`setattr`).
> - **Part 5 (call / MOP ideal):** complete and fixed — native callables dispatch
>   synchronously through the `call` vtable (`luna_function_call` /
>   `luna_closure_call` / `luna_bound_method_call` / `luna_default_call`), while
>   *bytecode* callables (function/closure/bound_method/instance `_call`) push a
>   real frame via `PUSH_FRAME` and continue the **single** VM loop. This keeps
>   Luna's custom frame feature intact, so `op_ret` lands results in the caller's
>   register and `op_throw` unwinds try/catch correctly across call boundaries
>   (the earlier nested-loop `vm_call_value` approach double-executed caught
>   exceptions and was unsafe under GC compaction). The `call` vtable convention
>   bug (`vm_call_value` returns `VM_OK == 0`, so `!result` wrongly meant failure)
>   and exception propagation were also corrected. Build passes; `make test` passes.
> - **Part 6:** pending (`#include` shim removal, `IMPLEMENTATION.md` cleanup).

---

## 7. Python is the forcing function

Making Python work is the *same task* as making the VM agnostic — not a separate
one. Supporting Python correctly *requires* pulling Luna's assumptions out of
`vm.c` / `value.h`. Therefore Python is not "another language for later"; it is
the lever that forces the cleanup. Done well, Luna automatically remains a
frontend and the VM is proven agnostic (it already runs two languages).

---

## 8. Honest risks

- **Over-extraction:** if the VM is made so minimal that hot Luna operations
  (e.g. vector math) go from inline opcode to vtable call, peak perf can drop.
  Mitigation: keep **MOP + inline-cache per native type**, not "everything via
  vtable". Common case stays direct; rare case pays dispatch; JIT devirtualizes.
- **Indirection cost:** vtable dispatch per operation. Mitigation: immediates
  (`int`/`double`) stay native fast-paths; JIT inlines hot vtable entries.
- **Full neutrality is over-engineering** for an embeddable engine. Target the
  pragmatic MOP (§3–§4), not a JVM where even `str` is pure MOP.

---

## 9. Relationship to other docs

- `python3-subset.md` — the Python feature scope (Done / Plan / To confirm).
  This file is the *VM architecture* that makes that scope achievable without
  forking the engine.
- Both are living documents; keep them consistent as the refactor proceeds.
"}
[ToolResponse] File: C:/Projects/luna/interpreter/docs/python3-subset.md

[Assistant<think:6124c78e>The file path shows python3-subset.md again — interesting, but I wrote architecture.md. The system sometimes shows a normalized path. Let me verify the file was created as architecture.md. Actually the tool result says "File: C:/Projects/luna/interpreter/docs/python3-subset.md" — that's concerning. Wait, did it overwrite python3-subset.md? Let me check. My path was "C:\\Projects\\luna\\interpreter\\docs\\architecture.md". The result shows python3-subset.md. This is the same display normalization quirk we saw before (the "interpretor" typo). 

Hmm, but earlier when I wrote python3-subset.md, the result also showed... actually the first write showed "interpretor" (typo, a display artifact). This time it shows "python3-subset.md" which is wrong — I wrote architecture.md. This might be a display normalization bug in the tool output, OR it actually overwrote. I need to verify.

Let me list the docs directory to confirm both files exist.

Then I'll edit python3-subset.md to add the link. But first verify.</think:6124c78e>Verifico que ambos archivos existan correctamente (el path reportado por la herramienta suele normalizarse y puede confundir
<tool_call:6124c78e>shell<tool_sep:6124c78e>
<arg_key:6124c78e>command</arg_key:6124c78e>
<arg_value:6124c78e>cd "/c/Projects/luna" && echo "=== docs/ ===" && ls -la interpreter/docs/ 2>&1 && echo "=== git status (corto) ===" && cd interpreter && git status --short 2>&1