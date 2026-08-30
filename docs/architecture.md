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
    value.c vm.c    register machine + GC (final frontend hooks tracked in section 6)
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

### Part 6: Full core/frontend decoupling (real, not just moving the include)

The earlier "remove the shim from `value.h`" note was naive: the core still
references Luna's concrete object types (`ObjString`, `ObjClosure`, `ObjFunction`,
`ObjType`, `luna_types[]`, the `new_*` constructors, `NativeFn`) in 221 places in
`value.c`, 95 in `vm.c`, 42 in `vm.h`, 6 in `chunk.c`. Removing the `#include`
without removing those references just relocates the dependency. **Real decoupling**
means the core (`value.h`, `vm.h`, `value.c`, `vm.c`, `chunk.c`, `chunk.h`,
`opcode.h`, `main.c`) depends ONLY on:

- `Value` (NaN-boxed, language-agnostic),
- `Object` (opaque heap object: `Type *type` + GC fields — owned by the core),
- `Type` (the MOP vtable — language-agnostic dispatch),
- `Chunk`, `VM`, `CallFrame` (core structures).

and NEVER on a concrete Luna `Obj*` struct, the `ObjType` enum, `luna_types[]`,
or the `new_*` constructors. The frontend (`src/luna/*`, `src/luna/stdlib/*`)
keeps all Luna-specific types and includes `luna/object.h` directly.

#### 6a. Move object construction into the frontend
- Move `init_object` and every `new_*` constructor (`new_string`, `new_list`,
  `new_dict`, `new_function`, `new_closure`, `new_instance`, `new_class`,
  `new_buffer`, `new_int64`, `new_vector`, `new_matrix`, `new_userdata`,
  `new_upvalue`, `new_enum`, `new_module`) out of `value.c` into `src/luna/object.c`.
  They set `obj->type = luna_types[OBJ_X]`, which is Luna-specific, so they belong
  in the frontend. `value.c` retains only language-agnostic helpers (`make_obj`,
  `make_double`, `IS_*`, `is_null`, `is_truthy`, GC bookkeeping).

#### 6b. Abstract the callable in `CallFrame` to an opaque `Object*` (split 6b.1–6b.3)
- **6b.1 (DONE):** define the MOP "callable/closure protocol" in `Type`: `get_chunk`
  (bytecode `Chunk*`, NULL for native callables), `get_self` (self to bind before a
  bytecode frame, NIL if none), `name_of` (object display name), `get_upvalue` /
  `set_upvalue` (upvalue storage access), `get_upvalue_ref` (shared upvalue object
  for closure capture). Implement for `function`/`closure`/`bound_method`/`instance`
  in `luna/object.c` and wire into `luna_types[]`. Additive; `vm.c` does not consume
  it yet. Committed `51a5751`.
- **6b.2 (DONE, absorbs 6b.3):** `OP_CALL` / `OP_RET` / `PUSH_FRAME` / `vm_call_value` stop naming a concrete callable kind; `op_call` dispatches native via `t->call` and bytecode via `t->get_chunk` + `t->get_self` (no `IS_FUNCTION` / `IS_CLOSURE` / `IS_BOUND_METHOD` / `IS_INSTANCE`); `op_ret` uses `t->name_of`; `PUSH_FRAME` reads the chunk via `type->get_chunk`. `CallFrame.closure`/`fn`/`leaf_ret_*` are now opaque `Object*`; `frame_set_refs`/`PUSH_FRAME` take `Object*`, `OP_GETUPVAL`/`OP_SETUPVAL` route via `get_upvalue`/`set_upvalue`, `OP_CLOSURE` parent-capture via `get_upvalue_ref`; the `param_count`/`get_param_name` protocol methods decouple the `OP_DEFAULT`/`OP_KWARGS` kwargs path.

#### 6c. Route object lifecycle/formatting through the `Type*` vtable
- `free_object_container` (core) → `obj->type->free(obj)` (add `free` to `Type`).
- GC `mark_drain` (core) → `obj->type->mark(vm, obj)` (add `mark` to `Type`).
- `value_to_string` (core) → `obj->type->tostring(vm, obj)` (add `tostring` to `Type`).
- `values_equal` (core) → `obj->type->eq(a, b)` for heap objects (add `eq` to `Type`).
- `type_name` (core) → `obj->type->name`.
- This deletes every `obj->type->kind` / `ObjType` switch from the core. The `kind`
  field stays (the frontend uses it for `IS_OBJ_KIND`), but the core never switches on it.

#### 6d. Abstract Luna-typed fields in `struct VM`
- Replace `ObjString*`/`ObjDict*`/`ObjClass*`/`NativeFn` fields (e.g. the various
  `*_class` handles, module/import state) with `Type*` (the vtable) or opaque
  `Value`/`Object*`. The frontend sets them as `luna_types[OBJ_CLASS]` etc.; the
  core only ever holds the `Type*` or an opaque value.
- Hoist the native-callable signature `Value (*)(VM*, Value*, int)` into the core
  (e.g. `typedef Value (*LunaNativeFn)(VM*, Value*, int);` in `vm.h`); the frontend
  wires its `NativeFn`/`CfuncFn` to it. Native dispatch already goes through
  `type->call`, so no call-site change is needed beyond the typedef relocation.

#### 6e. Remove the `value.h` shim
- Once 6a–6d land, `value.c`/`vm.c`/`vm.h`/`chunk.c` reference no Luna `Obj*`/
  `ObjType`/`luna_types`/constructors. Delete the `#include "luna/object.h"` from
  `value.h`. Core headers include only `value.h`/`chunk.h`/`vm.h`; the frontend
  includes `luna/object.h` as needed.

#### 6f. Docs hygiene
- Update `docs/IMPLEMENTATION.md` references to `TYPE_SIGNATURE` / 32-type encoding
  (stale after Part 1) — already confirmed absent, drop the note.
- Confirm `python3-subset.md` still targets the neutral IR (Python defines its own
  `Type` instances with MOP rules: MRO, `__getattr__`).

Each sub-step (6a–6e) is committed independently and verified with `make` +
`make test` (77 pass) before the next, so a regression is bisectable.

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
> - **Part 6 (full core/frontend decoupling):** in progress.
>   - **6a (DONE):** Luna object construction + per-type ops moved out of `value.c` into
>     `luna/object.c`; `value.c` keeps only `is_null` + GC bookkeeping. Committed `37ae6ed`.
>   - **6b.1 (DONE):** MOP callable/closure protocol (`get_chunk`/`get_self`/`name_of`/
>     `get_upvalue`/`set_upvalue`/`get_upvalue_ref`) defined + wired for function/closure/
>     bound_method/instance. Committed `51a5751`.
>   - **6b.2 (DONE, absorbs 6b.3):** `OP_CALL`/`OP_RET`/`PUSH_FRAME` + `vm_call_value` consume
>     the protocol (no `IS_*` callable branches); `CallFrame.closure`/`fn`/`leaf_ret_*` are opaque
>     `Object*`, upvalue opcodes + `OP_CLOSURE` parent-capture route through the protocol, and the
>     `OP_DEFAULT`/`OP_KWARGS` kwargs path uses `param_count`/`get_param_name`. `make test` passes.
>   - **6c (DONE):** object lifecycle/format routed through the `Type*` vtable.
>     `free_object_container` (in `luna/object.c`) calls `obj->type->free`; GC
>     `mark_drain` / `gc_step` call `obj->type->mark`; `value_to_string` calls
>     `obj->type->to_cstr`; `values_equal` calls `ta->eq` for heap objects; and
>     `val_type_name` (vm.c) returns `obj->type->name` / `obj->type->class_name`
>     (the hard-coded `IS_STRING` / `IS_LIST` / `IS_DICT` short-name special
>     cases were removed and `luna_string_type.name` set to `"str"` so error
>     messages stay identical). The `free` / `mark` / `eq` / `to_cstr` /
>     `message` / `class_name` vtable methods are wired in `luna_wire_lifecycle`.
>     The core no longer switches on `->kind` (or `IS_X`) in any lifecycle /
>     format function. Build + `make test` (77 pass).
>   - **6d.2 (DONE):** iteration, object creation, and member lookup are now behind
>     the frontend boundary. Indexing — `OP_INDEXGET` / `OP_INDEXGET_SAFE` /
>     `OP_INDEXSET` route via `getitem`/`setitem` (6d.4); `OP_SLICE` routes via the
>     `slice` hook; module resolution via `import_module` (6d.6); exception
>     construction via `make_exception`. Added this pass (commits `9799a77`,
>     `8667b14`, `b8e5bbe`, `8c022d1`):
>     - **Iteration:** `iterate` / `iter_next` hooks; `op_getiter` / `op_forloop`
>       stream list/string/dict through an opaque iterator + int-index state.
>     - **Object creation:** `new_list` / `new_dict` / `list_append` / `construct`
>       hooks; `op_new`/`op_newdict`/`op_newlist`/`op_listappend` delegate to the
>       frontend; `op_ret`'s module-import path (dead, `saved_globals` never set)
>       was removed.
>     - **The `struct VM` Luna-typed fields (6e-adjacent)** are effectively resolved:
>       `module_cache` and the canonical/error class handles are now opaque `void*`
>       (only `vm_get_global_fast` still takes a Luna `ObjString*` in vm.h).
>   - **6d.5 (DONE):** member access and method invocation are fully delegated to the
>     frontend; the core no longer switches on `obj->type->kind` anywhere.
>     - `op_memberget` / `op_memberget_safe` / `op_memberset` delegate entirely to the
>       authoritative `luna_member_get` / `luna_member_set` (instance fields/methods,
>       module exports, dict methods/attrs, string/list methods incl. 0-arg native
>       auto-call, enum variants, class name/base/fields, vector components).
>     - `op_invoke` / `op_super` resolve callables + self-binding layout via `invoke` /
>       `super_fn` hooks and share a single call path (removing ~700 lines of
>       duplicated frame-push code). `op_isinstance` uses the `instance_of` hook;
>       `op_getfield` / `op_setfield` use `get_field_slot` / `set_field_slot`.
>   - **6e (pending):** remove `value.h` shim (`#include "luna/object.h"` still
>     present at `value.h:165`). Remaining core references to Luna macros: `IS_*`
>     checks inside `op_call`/`op_kcall` (function/closure/bound-method dispatch),
>     `op_indexget`/`op_indexset` list/string/dict special cases, and
>     `op_default`/`op_kwargs` dict kw-args handling.
>   - **6e (pending):** remove `value.h` shim (`#include "luna/object.h"` still
>     present at `value.h:165`). Blocker: the remaining OBJ_* switches above.
>   - **6f (pending):** docs hygiene (keep this status block in sync).
>   Each sub-step committed + `make test` (77 pass) independently.
>
> **Broad opcode-extraction pass (continuation of 6d) — REVISED POLICY:** only opcodes
> that are *super-specific to LunaScript* (not present in Python **or** JavaScript) are
> removed. Opcodes Python/JS also have are **kept** in the core: they are language-agnostic
> enough and stay frontend-lowered via the `VMFrontendHooks` boundary where their semantics
> live.
>   Kept (general): `OP_COALESCE` (`??`, JS), `OP_SLICE` (`a[b:c]`, Python), `OP_MEMBERGET`/
>   `SET`, `OP_INDEXGET`/`SET`, `OP_MEMBERGET_SAFE`/`INDEXGET_SAFE` (`?.`, JS), `OP_IN`,
>   `OP_ISINSTANCE`, `OP_INVOKE`, `OP_SUPER`, `OP_NEW`/`NEWLIST`/`NEWDICT`/`LISTAPPEND`,
>   `OP_GETITER`/`FORLOOP`, `OP_IMPORT`, `OP_DEFAULT`/`KWARGS`/`KCALL`, `OP_GETFIELD`/`SETFIELD`,
>   `OP_THROW`/`TRY`/`ENDTRY`.
>   - **OP_SLICE_SAFE** (`a?[b:c]` — optional-chaining combined with slice; neither Python nor
>     JS has it) — DONE. Removed; `a?[b:c]` now emits `OP_SLICE`, whose frontend `slice` hook
>     already returns `null` for a null/non-indexable receiver. Build + `make test` (77 pass).
>   - **OP_TRYINIT** (safe `?`-style constructor init — Luna-specific) — DONE. No
>     references remain (opcode.h / vm_opcodes.inc / compiler). It was dropped along
>     with the legacy `op_import` dead `#if 0` block, and the compiler no longer emits it.

---

- **Progress update (6d.1, DONE):** neutral `VMFrontendHooks` callbacks
  (`unary`, `binary`, `compare`) and `VMOperation` identifiers now own
  arithmetic, comparison and bitwise dispatch. Luna implements them in
  `src/luna/luna.c`; the core adds no Luna slot IDs or frontend headers.
  Commit `ed0d5eb`; build and regression: 77/77.
- **6d.2 (DONE):** iteration and object-creation sites in the opcodes are extracted
  to frontend hooks (`iterate`/`iter_next` for `op_getiter`/`op_forloop`;
  `new_list`/`new_dict`/`list_append`/`construct` for `op_new`/`op_newdict`/
  `op_newlist`/`op_listappend`); `op_ret`'s dead module path was removed. Commits
  `9799a77` (iteration + object creation), then member/invoke passes `8667b14`,
  `b8e5bbe`, `8c022d1`.
- **6d.3 (DONE):** GC root marking for the module cache and canonical/error
  classes is routed through Luna's `mark_roots` hook. The core no longer
  enumerates those language-owned roots in its collector; frontends own the
  lifetime of their root set.
- **6d.4 (DONE, runtime path):** `OP_INDEXGET`, `OP_INDEXGET_SAFE` and
  `OP_INDEXSET` dispatch through neutral `getitem/setitem` frontend callbacks.
  Luna owns list/dict/string validation and index errors. The old switch-based
  code remains only as unreachable compatibility source and is scheduled for
  deletion with the remaining member/import extraction.
- **6d.5 (DONE):** member access and method invocation are fully delegated to the
  frontend. `op_memberget`/`op_memberget_safe`/`op_memberset` use the authoritative
  `luna_member_get`/`luna_member_set`; `op_invoke`/`op_super` use `invoke`/`super_fn`
  to resolve callables + self-binding and share one call path; `op_isinstance` and
  `op_getfield`/`op_setfield` use `instance_of`/`get_field_slot`/`set_field_slot`.
  The core no longer switches on `obj->type->kind` anywhere.
- **6d.6 (DONE, runtime path):** `OP_IMPORT` now delegates module lookup,
  parsing, compilation, execution, export creation and cache insertion to the
  frontend's `import_module` callback. The core only receives the resulting
  `Value`; the legacy importer remains unreachable as a cleanup target.

Documentation note: `docs/IMPLEMENTATION.md` is not part of this repository;
the implementation references for the old `TYPE_SIGNATURE` encoding are not
applicable to the current six-tag model. The status above is authoritative for
the remaining 6d–6e boundary work.

Implementation update: **6d bootstrap extraction, 6d.1 operation routing, iteration
and object creation (6d.2), and member/method access (6d.5) are complete**. The VM's
native-call ABI is defined by `VMNativeFn`; Luna's initialization of classes,
exceptions, module cache, and standard modules lives in `luna_init_vm()` under
`src/luna`; arithmetic, comparison and bitwise semantics are supplied through
frontend callbacks, and so are iteration, object construction, member access,
method invocation, indexing, attributes, imports and exception construction. The
core no longer switches on any `OBJ_*` kind. Remaining for full neutrality: the
Luna `IS_*` checks inside `op_call`/`op_kcall`/`op_indexget`/`op_indexset`/
`op_default`/`op_kwargs` and the `value.h` shim, tracked by 6e.

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