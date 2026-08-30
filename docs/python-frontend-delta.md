# Python Frontend Delta — from Luna to a Python 3 subset

Example frontend: `src/luna/` (kept intact). Working copy: `src/py/`.
All code edits go in `src/py/*`, never in `src/luna/*`.

Audited against the current code. References point at `src/luna/` (they apply to
`src/py/`, which is an identical copy).

---

## 1. Remove (JS-ish / Luna-only syntax)

### Lexer keywords to drop (`lexer.c:145-181`)
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

### Operators and tokens (JS-ish)
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

| Keyword | Token to create | Notes |
|---|---|---|
| `None` | reuse `TOK_NULL` with the `None` spelling (or a new token) | same runtime nil |
| `True` / `False` | reuse `TOK_TRUE`/`TOK_FALSE` (or new) | same runtime bool |
| `elif` | `TOK_ELIF` | does not exist today (`lexer.c:145-181`); the parser only sees `else` |
| `is` | `TOK_IS` | identity comparison; compose `is not` |
| `raise` | `TOK_RAISE` | replaces `throw`; support re-raise |
| `lambda` | keyword -> `TOK_LAMBDA` | today `=>` produces `TOK_LAMBDA` (`lexer.c:313`) |
| `with` | `TOK_WITH` | context (`__enter__`/`__exit__`), phase 2 |
| `assert` | `TOK_ASSERT` | |
| `del` | `TOK_DEL` | `del x`, `del x[i]` statements |
| `yield` | `TOK_YIELD` | generators, phase 2 |
| `global` / `nonlocal` | -- | phase 2 |
| `async` / `await` | -- | phase 2 |

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

- **Methods and `self`:** today `self` is an implicit keyword (first register of
  the method, `parse_decl.c:30`). Python declares `self` explicitly as the first
  parameter. Affects method lookup and `OP_INVOKE` (front-end `invoke` hook in
  `luna.c:565`).
- **Instantiation:** `new Foo(...)` -> call the class `Foo(...)`. The
  `construct` hook (`luna.c:473`) needs revisiting.
- **Inheritance:** `extends` -> parenthesized base list. `super` hook
  (`luna.c:637`) must re-map onto MRO.
- **Division:** today `/` on integers is integer division (`luna.c:134`).
  Python 3: `/` is always float, `//` is floor div. Missing `//`, `**`, and
  their compound `=` forms.
- **Literal subset:** `0x`/`0o`/`0b` and `_` digit separators are unverified in
  `scan_number` (`lexer.c:213`). Optional in phase 2.
- **Builtins:** confirm `print`, `len`, `type`, `isinstance`, `enumerate`,
  `abs`, `min`, `max`, `sorted`, `repr`, `str`, `int`, `float`, `bool`
  (actual state in `vm_builtins.c`).

---

## 5. Suggested work order

1. **Lexer** (`src/py/lexer.c`): remove JS keywords/ops, add Python keywords
   (`None/True/False`, `elif`, `is`, `raise`, `lambda`).
2. **Parser** (`parse_stmt.c`/`parse_expr.c`/`parse_decl.c`): `elif`, `is`,
   `raise`, call-based instantiation, parenthesized inheritance.
3. **Object model** (`object.c`/`luna.c` hooks): explicit `self`, `super()`
   MRO, `/` vs `//`, dunders.
4. **Tests** driven by failing `.py` cases.