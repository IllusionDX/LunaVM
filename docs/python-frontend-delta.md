# Python Frontend Delta — from Luna to a Python 3 subset

Example frontend: `src/luna/` (kept intact). Working copy: `src/py/`.
All code edits go in `src/py/*`, never in `src/luna/*`.

Audited against the current code. References point at `src/luna/` (they apply to
`src/py/`, which is an identical copy).

> **Status (audited at `python-subset @ 8751d6a`):** §1 (remove) and the parser
> half of §2 (add) are **complete**; §3 holds. §4 is partially done (object
> model: `self`/`super()`/instantiation done; arithmetic semantics — `/`
> true division, `//`, `**`, compound `//=`, `**=` — done (OP_IDIV/OP_POW);
> dunders and extra builtins pending). Struck-through items below are verified.

---

## 1. Remove (JS-ish / Luna-only syntax)

### Lexer keywords to drop (`lexer.c:145-181`)
> **Status: complete** — none of these are in the `src/py` keyword table; the
> typedefs (`TOK_VAR`, `TOK_NEW`, ...) are dead leftovers in `lexer.h`.
| Keyword | Token | Why it goes | Python alternative |
|---|---|---|---|
| `var` | `TOK_VAR` | does not exist in Python | plain assignment `x = v` |
| `const` | `TOK_CONST` | does not exist in Python | plain assignment |
| `new` | `TOK_NEW` | Python instantiates by call | `Foo(...)` — the class is a callable |
| `extends` | `TOK_EXTENDS` | -- | `class A(Base):` (`parse_decl.c:107`) |
| `self` | `TOK_SELF` | in Python `self` is a conventional param, not a keyword | explicit first parameter of methods |
| `super` | `TOK_SUPER` | Python exposes it as a builtin | `super()` (MRO-based) |
| `null` / `true` / `false` | `TOK_NULL/TRUE/FALSE` | JS spelling | `None` / `True` / `False` |
| `enum` | `TOK_ENUM` | Python has no enum keyword | stdlib enum class (future) |
| `switch` / `case` / `default` | `TOK_SWITCH/CASE/DEFAULT` | JS | `match`/`case` (3.10+, phase 2+ if at all) |
| `throw` | `TOK_THROW` | JS | `raise` |

`except` **stays**: it is a valid Python keyword (`except Exception as e:`).
`as`, `from`, `import`, `def`, `class`, `if`, `else`, `while`, `for`, `in`,
`break`, `continue`, `return`, `pass`, `and`, `or`, `not`, `try`, `finally`
**stay**: all are valid Python.

### Type keywords (`lexer.c:145-181`, annotation tokens)
`int`, `float`, `bool`, `char`, `string`, `list`, `dict`
(`TOK_INT_TYPE`, `TOK_FLOAT_TYPE`, `TOK_BOOL_TYPE`, `TOK_CHAR_TYPE`,
`TOK_STRING_TYPE`, `TOK_LIST_TYPE`, `TOK_DICT`).

In Python, `int`/`float`/`bool`/`str`/`list`/`dict` are builtins (identifiers),
**not keywords**. `char` and `string` do not even exist (it is `str`). The
annotation machinery can stay; what needs re-mapping is keyword vs identifier.

### Annotations — scope per [PEP 526](https://peps.python.org/pep-0526/)
Type hints follow PEP 526 (variable annotations, Python 3.6+) on top of the
PEP 484 function-annotation syntax. In this frontend they are **parsed and
ignored** — no runtime type checking, no `__annotations__` storage
(consistent with "subset, not CPython").

**Works in `src/py`:**
- Function parameter annotations: `def f(a: int, b: str):` (`parse_decl.c:39`).
- Return annotations: `def f(...) -> str:` via `TOK_ARROW` (`parse_decl.c:69`).
- Type names are plain identifiers, not keywords: `int = 3` / `str = "x"`
  remain valid Python.

**Missing (PEP 526 forms not yet accepted):**
- Variable annotation with/without value at module/function scope: `x: int = 5`
  and bare `x: int` — today `x:` is a parse error.
- Class-level annotated *assignment*: `captain: str = 'Picard'` fails (class
  fields parse bare `x: int` as a field declaration but accept no default
  value).
- Generic container annotations `list[int]` / `dict[str, int]` (PEP 585).
- `__annotations__` (out of scope by design).

### Operators and tokens (JS-ish)
> **Status: complete** — `!`, `&&`, `||`, `??`, `?.`, `?[`, `=>` all rejected /
> removed in `src/py/lexer.c:315-383` (hidden by the `should_fail` test).
| Syntax | Token | Where | Action |
|---|---|---|---|
| `=>` (arrow lambda) | `TOK_LAMBDA` | `lexer.c:313` (there is NO `lambda` keyword today; lambdas are `=>`) | replace with the `lambda` keyword |
| `!` prefix (unary not) | `TOK_NOT` | `lexer.c:317` (used by unary in `parse_expr.c:520`) | Python uses `not`; drop only the `!` prefix (keep `!=`) |
| `&&` | `TOK_AND` | `lexer.c:327` | Python uses `and` |
| `||` | `TOK_OR` | `lexer.c:330` | Python uses `or` |
| `??` | `TOK_COALESCE` | `lexer.c:357` | does not exist in Python |
| `?.` / `?[` | `TOK_QUESTION_DOT` / `TOK_QUESTION_LBRACKET` | `lexer.c:358-366` | does not exist in Python |

### Notes
- Comments support only `#` (`lexer.c:114-120`). Already Python style; nothing to
  remove (`//` and `/* */` do not exist).
- `'a'` as char: once the `char` type is gone, `'a'` becomes a 1-char `str`
  (string scanning accepts single quotes, `lexer.c:244` `scan_string`).

---

## 2. Add (missing Python keywords)

Done in `src/py` (verified at `8751d6a`):

| Keyword | Token | Status |
|---|---|---|
| `None` | `TOK_NULL` | Done (`lexer.c:130`) |
| `True` / `False` | `TOK_TRUE` / `TOK_FALSE` | Done (`lexer.c:131-132`) |
| `elif` | `TOK_ELIF` | Done (`lexer.c:136`, `parse_stmt.c:171`) |
| `is` | `TOK_IS` | Done (`lexer.c:141`; + `is not`, `parse_expr.c:649`) |
| `raise` | `TOK_RAISE` | Done (`lexer.c:150`, `parse_stmt.c:324`; no bare re-raise) |
| `lambda` | `TOK_LAMBDA` | Done — keyword replaces `=>` (`lexer.c:154`) |
| `with` | `TOK_WITH` | pending (context managers, phase 2) |
| `assert` | `TOK_ASSERT` | pending |
| `del` | `TOK_DEL` | pending |
| `yield` | `TOK_YIELD` | pending (generators, phase 2) |
| `global` / `nonlocal` | -- | pending (phase 2) |
| `async` / `await` | -- | pending (phase 2) |

---

## 3. Already compatible (verified)

- **Python-style indentation:** `TOK_INDENT`/`TOK_DEDENT` with a stack
  (`lexer.c:393-443`, `TOK_INDENT` in `lexer.h:96`).
- **Definitions:** `def` (`parse_decl.c:65`), `class` (`parse_decl.c:103`).
- **Control flow:** `if`/`else`, `while`, `for ... in` (+ numeric `range`
  fast-path in `compiler.c:1751`), `break`, `continue`, `return`, `pass`.
- **Logical operators:** `and`/`or`/`not` (keywords, `lexer.c:174-176`).
- **Imports:** `import`/`from ... import ... as` (`TOK_AS`, `module.c`).
- **Exceptions:** `try`/`except`/`finally`/`as` (`ast.h:177` `type_name`).
- **Return annotation `->`:** `TOK_ARROW` is `->` (`lexer.c:301`), used in `def`
  (`parse_decl.c:73`) and lambdas (`parse_expr.c:813`). Matches Python.
- **Slices:** `a[1:2]` via `TOK_COLON` inside `[ ]` (`parse_expr.c:456`).
- **Dicts:** `{key: value}` with `TOK_COLON` (`parse_expr.c:106`).
- **f-strings:** `f"..."` / `f'...'` (`lexer.c:479` + `fstring.c`).
- **Triple quotes:** `"""..."""` / `'''...'''` (`TOK_MULTILINE_STRING`,
  `skip_multiline_comment` `lexer.c:122`).
- **Operators:** `== != < <= > >= << >> & | ^ ~ %` and `+= -= *= /=` all match.
- **`;`** as a statement separator: valid in Python (`parser.h:67`).
- **Multi-assign / destructuring** (`ast.h:192-193` pattern).

---

## 4. Semantic delta (non-lexical, later phases)

- ~~**Methods and `self`:**~~ **done in `src/py`** — `self` is an explicit
  first parameter (`def __init__(self, name):`); `OP_INVOKE` binds it as in
  Python (commit `7aa1a2c`).
- ~~**Instantiation:**~~ **done in `src/py`** — `Foo(...)` calls the class via
  `py_class_call` (`object.c:528`), which runs inherited `__init__`.
- ~~**Inheritance:**~~ **done in `src/py`** — `class A(Base):` parses the
  parenthesized base (`parse_decl.c:103`); `super().method()` resolves through
  the base via `py_super` (`py.c:619`).
- ~~**Division:** `/` on integers is still integer division
  (`py.c:116-119` → `print(10 / 4)` gives `2`, not `2.5`). Python 3 requires
  float `/`; `//` and `**` are still missing (lexer/parser).~~ **done in
  `src/py`** — `/` is true division (int/int → float), `//` is floor division
  (sign-aware, int result for int operands), `**` is power (int for exact
  int64 results, float fallback); compound `//=` / `**=` included. New VM
  opcodes `OP_IDIV` / `OP_POW`. Tests: `tests_py/test_py_divpow.py`.
- ~~**Numbers as virtual objects:**~~ **done in `src/py`** — immediate
  int/float/bool resolve methods and attributes via canonical classes
  (`bit_length`, `is_integer`, dunders, `real`/`imag`, `__class__`) without
  boxing. The canonical classes live in the py frontend state
  (`py/frontend_state.h`, `py_fe`), not in the language-agnostic VM core.
  Tests: `tests_py/test_py_numobj.py`.
- **Literal subset:** `0x`/`0o`/`0b` and `_` digit separators unverified in
  `scan_number` (`lexer.c:189`). Optional in phase 2.
- **Builtins:** present: `print`, `len`, `range`, `type`, `str`, `int`,
  `float`, `isinstance` (`vm_builtins.c:748-768`).
  Missing: `enumerate`, `abs`, `min`, `max`, `sorted`, `repr`, `bool`.

---

## 5. Suggested work order

1. ~~**Lexer:** remove JS keywords/ops, add Python keywords~~
   (`None/True/False`, `elif`, `is`, `raise`, `lambda`). **Done** in `src/py`.
2. ~~**Parser:** `elif`, `is`, `raise`, call-based instantiation,
   parenthesized inheritance.~~ **Done** in `src/py`.
3. **Object model:** `self`, `super()` MRO, call-based instantiation, and
   arithmetic semantics (`/`, `//`, `**`) are done; dunders and missing
   builtins remain.
4. **Tests:** 8 `tests_py/*.py` files pass; keep adding failing `.py` cases
   for the remaining semantic gaps (builtins, dunders).