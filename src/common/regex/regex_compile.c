/* regex_compile.c — Regex pattern compiler.
 *
 * Emits a flat instruction stream with ABSOLUTE branch targets so atom
 * blocks can be compiled into temporary buffers and spliced (copied with
 * a uniform target shift) when expanding quantifiers and alternations.
 *
 * Quantifiers expand to canonical SPLIT/JUMP shapes (Pike/Thompson style):
 *   X*  : [SPLIT(a=body, b=exit)] [X] [JUMP -> split]
 *   X+  : [X] [SPLIT(a=body, b=exit)]
 *   X?  : [SPLIT(a=body, b=exit)] [X]
 *   X{m,n}: m linear copies of X, then (n-m) optional copies or an X* loop.
 * Lazy quantifiers swap which branch is preferred (a/b), never the shape.
 *
 * Case-insensitivity and dotall/multiline are folded at compile time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "regex.h"

typedef struct {
    const char *pat;
    int pos;
    int len;
    int flags;              /* effective flags while compiling */
    RE_Instruction *code;
    int code_cap;
    int code_len;
    int loop_count;         /* loop-guard slots assigned */
    char *group_names[RE_MAX_GROUPS]; /* indexed by group-1 */
    int group_count;
    char *err;
    int err_len;
} Compiler;

/* ---- Emit helpers ---- */

static void emit_ins(Compiler *c, RE_Instruction ins) {
    if (c->code_len >= c->code_cap) {
        c->code_cap = c->code_cap ? c->code_cap * 2 : 64;
        c->code = realloc(c->code, sizeof(RE_Instruction) * (size_t)c->code_cap);
    }
    c->code[c->code_len++] = ins;
}

static void emit_op(Compiler *c, RE_OpCode op, int arg) {
    RE_Instruction ins;
    memset(&ins, 0, sizeof(ins));
    ins.op = op;
    ins.arg = arg;
    emit_ins(c, ins);
}

static int peek(Compiler *c) {
    return c->pos < c->len ? (unsigned char)c->pat[c->pos] : -1;
}

static int advance(Compiler *c) {
    return c->pos < c->len ? (unsigned char)c->pat[c->pos++] : -1;
}

static void fail(Compiler *c, const char *msg) {
    if (!c->err[0]) snprintf(c->err, (size_t)c->err_len, "%s", msg);
}

/* ---- Temp-block mechanism ----
 * Atoms compile into a fresh buffer so quantifier expansion can copy the
 * block (m copies for {m,n}). All targets inside a fresh buffer are
 * internal, so splicing shifts them uniformly.
 */
typedef struct {
    RE_Instruction *code;
    int cap;
    int len;
} CodeState;

static void code_save(Compiler *c, CodeState *s) {
    s->code = c->code;
    s->cap = c->code_cap;
    s->len = c->code_len;
    c->code = NULL;
    c->code_cap = 0;
    c->code_len = 0;
}

static RE_Instruction *code_take(Compiler *c, CodeState *s, int *out_len) {
    RE_Instruction *blk = c->code;
    *out_len = c->code_len;
    c->code = s->code;
    c->code_cap = s->cap;
    c->code_len = s->len;
    return blk;
}

/* Copy block into the main buffer, shifting internal targets by base. */
static void emit_block(Compiler *c, RE_Instruction *blk, int n, int base) {
    for (int i = 0; i < n; i++) {
        RE_Instruction ins = blk[i];
        switch (ins.op) {
        case RE_OP_SPLIT:
            ins.data.split.a += base;
            ins.data.split.b += base;
            if (ins.data.split.body >= 0) ins.data.split.body += base;
            break;
        case RE_OP_JUMP:
            ins.data.jump.target += base;
            break;
        case RE_OP_LOOKAHEAD:
        case RE_OP_LOOKBEHIND:
            ins.data.look.body_end += base;
            break;
        default:
            break;
        }
        emit_ins(c, ins);
    }
}

static int new_loop_idx(Compiler *c) {
    if (c->loop_count >= RE_MAX_REPEATS) {
        fail(c, "too many quantifiers");
        return -1;
    }
    return c->loop_count++;
}

/* ---- Character class bitmap helpers ---- */

static void class_add_char(uint32_t bitmap[8], int ch, int fold) {
    if (ch < 0 || ch > 127) return;
    bitmap[ch / 32] |= 1u << (ch % 32);
    if (fold && isalpha(ch)) {
        int other = islower(ch) ? toupper(ch) : tolower(ch);
        bitmap[other / 32] |= 1u << (other % 32);
    }
}

static void class_add_range(uint32_t bitmap[8], int lo, int hi, int fold) {
    for (int i = lo; i <= hi; i++) class_add_char(bitmap, i, fold);
}

static void shorthand_to_bitmap(uint32_t bitmap[8], char type) {
    switch (type) {
    case 'd': class_add_range(bitmap, '0', '9', 0); break;
    case 'D':
        class_add_range(bitmap, 0, '0' - 1, 0);
        class_add_range(bitmap, '9' + 1, 127, 0);
        break;
    case 'w':
        class_add_range(bitmap, 'a', 'z', 0);
        class_add_range(bitmap, 'A', 'Z', 0);
        class_add_range(bitmap, '0', '9', 0);
        class_add_char(bitmap, '_', 0);
        break;
    case 'W': {
        for (int i = 0; i < 128; i++) class_add_char(bitmap, i, 0);
        class_add_range(bitmap, 'a', 'z', 0);
        class_add_range(bitmap, 'A', 'Z', 0);
        class_add_range(bitmap, '0', '9', 0);
        class_add_char(bitmap, '_', 0);
        break;
    }
    case 's':
        class_add_char(bitmap, ' ', 0);  class_add_char(bitmap, '\t', 0);
        class_add_char(bitmap, '\n', 0); class_add_char(bitmap, '\r', 0);
        class_add_char(bitmap, '\f', 0); class_add_char(bitmap, '\v', 0);
        break;
    case 'S': {
        for (int i = 0; i < 128; i++) class_add_char(bitmap, i, 0);
        class_add_char(bitmap, ' ', 0);  class_add_char(bitmap, '\t', 0);
        class_add_char(bitmap, '\n', 0); class_add_char(bitmap, '\r', 0);
        class_add_char(bitmap, '\f', 0); class_add_char(bitmap, '\v', 0);
        break;
    }
    }
}

/* Read one class element (char or escape). Returns the char value, or
 * -2 when it added shorthand-class bits instead, -1 on error. */
static int class_elem(Compiler *c, uint32_t bitmap[8]) {
    int ch = advance(c);
    if (ch != '\\') return ch;
    int esc = advance(c);
    if (esc < 0) { fail(c, "bad escape (end of pattern)"); return -1; }
    switch (esc) {
    case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
        shorthand_to_bitmap(bitmap, (char)esc);
        return -2;
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return '\0';
    case 'a': return '\a';
    case 'b': return '\b';   /* backspace inside a class */
    case 'x': case 'u': {
        int val = 0, digits = (esc == 'x') ? 2 : 4;
        for (int i = 0; i < digits; i++) {
            int h = peek(c);
            if (h >= '0' && h <= '9') val = val * 16 + (advance(c) - '0');
            else if (h >= 'a' && h <= 'f') val = val * 16 + (advance(c) - 'a' + 10);
            else if (h >= 'A' && h <= 'F') val = val * 16 + (advance(c) - 'A' + 10);
            else break;
        }
        return val;
    }
    default:
        return esc;   /* \. \[ \* \\ etc */
    }
}

static int parse_char_class(Compiler *c, uint32_t bitmap[8], int *negate) {
    memset(bitmap, 0, 32);
    *negate = 0;
    if (peek(c) == '^') { advance(c); *negate = 1; }

    for (int first = 1; c->pos < c->len && (peek(c) != ']' || first); first = 0) {
        int lo = class_elem(c, bitmap);
        if (lo == -1) return -1;
        if (lo == -2) continue;
        if (peek(c) == '-' && c->pos + 1 < c->len && c->pat[c->pos + 1] != ']') {
            advance(c); /* '-' */
            int hi = class_elem(c, bitmap);
            if (hi == -1) return -1;
            if (hi == -2) { fail(c, "bad character range"); return -1; }
            if (hi < lo) { fail(c, "bad character range"); return -1; }
            class_add_range(bitmap, lo, hi, c->flags & RE_FLAG_I);
        } else {
            class_add_char(bitmap, lo, c->flags & RE_FLAG_I);
        }
    }
    if (peek(c) != ']') { fail(c, "unterminated character set"); return -1; }
    advance(c);
    return 0;
}

/* ---- Quantifier parsing ---- */

typedef struct {
    int has;
    int min;
    int max;   /* -1 = unbounded */
    int lazy;
} Quant;

/* Parse a quantifier at c->pos. Malformed '{' is left unconsumed. */
static void parse_quant(Compiler *c, Quant *q) {
    q->has = 0; q->min = 1; q->max = 1; q->lazy = 0;
    int ch = peek(c);
    if (ch == '*')      { advance(c); q->has = 1; q->min = 0; q->max = -1; }
    else if (ch == '+') { advance(c); q->has = 1; q->min = 1; q->max = -1; }
    else if (ch == '?') { advance(c); q->has = 1; q->min = 0; q->max = 1; }
    else if (ch == '{') {
        int save = c->pos;
        advance(c);
        int n1 = 0, has1 = 0;
        while (peek(c) >= '0' && peek(c) <= '9') { n1 = n1 * 10 + (advance(c) - '0'); has1 = 1; }
        if (!has1) { c->pos = save; return; }
        int n2;
        if (peek(c) == ',') {
            advance(c);
            n2 = -1;
            while (peek(c) >= '0' && peek(c) <= '9') n2 = (n2 < 0 ? 0 : n2) * 10 + (advance(c) - '0');
        } else {
            n2 = n1;
        }
        if (peek(c) != '}') { c->pos = save; return; }
        advance(c);
        if (n2 != -1 && n2 < n1) { fail(c, "min repeat greater than max repeat"); return; }
        if (n1 > 1000 || (n2 != -1 && n2 > 1000)) { fail(c, "repetition count too large"); return; }
        q->has = 1; q->min = n1; q->max = n2;
    }
    if (q->has && peek(c) == '?') { advance(c); q->lazy = 1; }
}

/* ---- Quantifier expansion (splice templates) ---- */

static void emit_star(Compiler *c, RE_Instruction *blk, int n, int lazy) {
    int idx = new_loop_idx(c);
    if (idx < 0) return;
    int split = c->code_len;
    int body = split + 1;
    int jump = body + n;
    int exit = jump + 1;
    RE_Instruction s;
    memset(&s, 0, sizeof(s));
    s.op = RE_OP_SPLIT;
    s.data.split.a = lazy ? exit : body;
    s.data.split.b = lazy ? body : exit;
    s.data.split.body = body;
    s.data.split.loop_idx = idx;
    emit_ins(c, s);
    emit_block(c, blk, n, body);
    emit_op(c, RE_OP_JUMP, 0);
    c->code[jump].data.jump.target = split;
}

static void emit_plus(Compiler *c, RE_Instruction *blk, int n, int lazy) {
    int body = c->code_len;
    emit_block(c, blk, n, body);
    int idx = new_loop_idx(c);
    if (idx < 0) return;
    int split = body + n;
    int exit = split + 1;
    RE_Instruction s;
    memset(&s, 0, sizeof(s));
    s.op = RE_OP_SPLIT;
    s.data.split.a = lazy ? exit : body;
    s.data.split.b = lazy ? body : exit;
    s.data.split.body = body;
    s.data.split.loop_idx = idx;
    emit_ins(c, s);
}

static void emit_opt(Compiler *c, RE_Instruction *blk, int n, int lazy) {
    int split = c->code_len;
    int body = split + 1;
    int exit = body + n;
    RE_Instruction s;
    memset(&s, 0, sizeof(s));
    s.op = RE_OP_SPLIT;
    s.data.split.a = lazy ? exit : body;
    s.data.split.b = lazy ? body : exit;
    s.data.split.body = -1;      /* plain split: no empty-cycle guard */
    s.data.split.loop_idx = -1;
    emit_ins(c, s);
    emit_block(c, blk, n, body);
}

static void splice_quant(Compiler *c, RE_Instruction *blk, int n, Quant *q) {
    if (!q->has || (q->min == 1 && q->max == 1)) {
        emit_block(c, blk, n, c->code_len);
        return;
    }
    if (q->min == 0 && q->max == 0) return;   /* X{0}: matches empty */

    if (q->min == 0 && q->max == -1) { emit_star(c, blk, n, q->lazy); return; }
    if (q->min == 1 && q->max == -1) { emit_plus(c, blk, n, q->lazy); return; }
    if (q->min == 0 && q->max == 1)  { emit_opt(c, blk, n, q->lazy);  return; }

    for (int i = 0; i < q->min; i++) emit_block(c, blk, n, c->code_len);
    if (q->max == -1) {
        emit_star(c, blk, n, q->lazy);
    } else {
        for (int i = 0; i < q->max - q->min; i++) emit_opt(c, blk, n, q->lazy);
    }
}

/* ---- Verbose-mode whitespace/comment skipping ---- */

static void skip_x(Compiler *c) {
    if (!(c->flags & RE_FLAG_X)) return;
    for (;;) {
        int ch = peek(c);
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f') {
            advance(c);
            continue;
        }
        if (ch == '#') {
            while (c->pos < c->len && peek(c) != '\n') advance(c);
            continue;
        }
        break;
    }
}

/* ---- Atoms ---- */

static void compile_alternation(Compiler *c);

static void emit_ins_at_front(Compiler *c, RE_Instruction *ins) {
    if (c->code_len >= c->code_cap) {
        c->code_cap = c->code_cap ? c->code_cap * 2 : 64;
        c->code = realloc(c->code, sizeof(RE_Instruction) * (size_t)c->code_cap);
    }
    memmove(c->code + 1, c->code, sizeof(RE_Instruction) * (size_t)c->code_len);
    c->code[0] = *ins;
    c->code_len++;
}

/* Parse a group name after (?P< or (?< (cursor at first name char).
 * Registers the name and returns the 1-based group index. */
static int parse_and_register_group_name(Compiler *c) {
    int ns = c->pos;
    while (c->pos < c->len && peek(c) != '>') advance(c);
    if (peek(c) != '>') { fail(c, "missing >, unterminated name"); return -1; }
    int nl = c->pos - ns;
    advance(c); /* '>' */
    if (nl <= 0) { fail(c, "missing group name"); return -1; }
    if (c->group_count + 1 >= RE_MAX_GROUPS) { fail(c, "too many groups"); return -1; }
    int gi = ++c->group_count;
    c->group_names[gi - 1] = malloc((size_t)nl + 1);
    memcpy(c->group_names[gi - 1], c->pat + ns, (size_t)nl);
    c->group_names[gi - 1][nl] = '\0';
    return gi;
}

static void compile_atom(Compiler *c) {
    int ch = peek(c);

    if (ch == '(') {
        advance(c);
        int capturing = 1;
        int look_pos_neg = -1;   /* 1 = (?=, 0 = (?!, 3 = (?<=, 4 = (?<!, -1 n/a */

        if (peek(c) == '?') {
            advance(c);
            int next = peek(c);
            if (next == ':') { advance(c); capturing = 0; }
            else if (next == '=')  { advance(c); look_pos_neg = 1; }
            else if (next == '!')  { advance(c); look_pos_neg = 0; }
            else if (next == 'P') {
                advance(c);
                int p2 = peek(c);
                if (p2 == '=') {
                    /* (?P=name) backreference */
                    advance(c);
                    int ns = c->pos;
                    while (c->pos < c->len && peek(c) != ')') advance(c);
                    if (peek(c) != ')') { fail(c, "missing ), unterminated name"); return; }
                    int nl = c->pos - ns;
                    char name[128];
                    if (nl >= (int)sizeof(name)) { fail(c, "group name too long"); return; }
                    memcpy(name, c->pat + ns, (size_t)nl);
                    name[nl] = '\0';
                    advance(c); /* ')' */
                    int gi = -1;
                    for (int i = 0; i < c->group_count; i++) {
                        if (c->group_names[i] && strcmp(c->group_names[i], name) == 0) { gi = i + 1; break; }
                    }
                    if (gi < 0) { fail(c, "unknown group name"); return; }
                    emit_op(c, RE_OP_BACKREFERENCE, gi);
                    return;
                }
                if (p2 != '<') { fail(c, "unknown extension (?P..."); return; }
            }
            else if (next == '<') {
                advance(c); /* consume '<' */
                int p2 = peek(c);
                if (p2 == '=' || p2 == '!') {
                    /* (?<=...) / (?<!...) lookbehind */
                    advance(c);
                    look_pos_neg = (p2 == '=') ? 3 : 4;
                    /* fall through to lookaround handling below */
                } else {
                    /* (?<name>...) named group (Python 3.11 syntax) */
                    if (peek(c) == '>') { fail(c, "missing group name"); return; }
                    int gi = parse_and_register_group_name(c);
                    if (c->err[0]) return;
                    emit_op(c, RE_OP_START_GROUP, gi);
                    compile_alternation(c);
                    if (c->err[0]) return;
                    if (peek(c) != ')') { fail(c, "missing ), unterminated subpattern"); return; }
                    advance(c);
                    emit_op(c, RE_OP_END_GROUP, gi);
                    return;
                }
            }
            else if (next == 'i' || next == 'm' || next == 's' || next == 'x' ||
                     next == 'u' || next == 'a' || next == 'L' || next == '-') {
                /* inline flags: (?flags) global toggle or (?flags:...) scoped.
                 * Flags fold at compile time; scoped form applies to body only. */
                int new_flags = c->flags;
                int neg = 0;
                for (;;) {
                    int f = peek(c);
                    if (f == '-') { neg = 1; advance(c); continue; }
                    int bit = 0;
                    switch (f) {
                    case 'i': bit = RE_FLAG_I; break;
                    case 'm': bit = RE_FLAG_M; break;
                    case 's': bit = RE_FLAG_S; break;
                    case 'x': bit = RE_FLAG_X; break;
                    case 'u': case 'a': case 'L': bit = 0; break; /* accepted, no-op */
                    default: bit = -1; break;
                    }
                    if (bit < 0) break;
                    advance(c);
                    if (bit) {
                        if (neg) new_flags &= ~bit;
                        else new_flags |= bit;
                    }
                }
                if (peek(c) == ')') {
                    advance(c);
                    /* global toggle: affects the rest of the pattern */
                    c->flags = new_flags;
                    return;   /* emits nothing */
                }
                if (peek(c) != ':') { fail(c, "unknown flag"); return; }
                advance(c);
                int saved_flags = c->flags;
                c->flags = new_flags;
                compile_alternation(c);
                if (c->err[0]) return;
                if (peek(c) != ')') { fail(c, "missing ), unterminated subpattern"); return; }
                advance(c);
                c->flags = saved_flags;
                return;
            }
            else { fail(c, "unknown extension"); return; }
            if (next == 'P') {
                /* (?P<name>...) � '<' not yet consumed */
                advance(c); /* consume '<' */
                if (peek(c) == '>') { fail(c, "missing group name"); return; }
                int gi = parse_and_register_group_name(c);
                if (c->err[0]) return;
                emit_op(c, RE_OP_START_GROUP, gi);
                compile_alternation(c);
                if (c->err[0]) return;
                if (peek(c) != ')') { fail(c, "missing ), unterminated subpattern"); return; }
                advance(c);
                emit_op(c, RE_OP_END_GROUP, gi);
                return;
            }
        }

        if (look_pos_neg >= 0) {
            /* lookahead / lookbehind: body follows inline */
            int ins_idx = c->code_len;
            emit_op(c, look_pos_neg <= 1 ? RE_OP_LOOKAHEAD : RE_OP_LOOKBEHIND,
                    look_pos_neg == 1 || look_pos_neg == 3 ? 1 : 0);
            compile_alternation(c);
            if (c->err[0]) return;
            if (peek(c) != ')') { fail(c, "missing ), unterminated subpattern"); return; }
            advance(c);
            c->code[ins_idx].data.look.body_end = c->code_len;
            return;
        }

        if (capturing) {
            if (c->group_count + 1 >= RE_MAX_GROUPS) { fail(c, "too many groups"); return; }
            int gi = ++c->group_count;
            emit_op(c, RE_OP_START_GROUP, gi);
            compile_alternation(c);
            if (c->err[0]) return;
            if (peek(c) != ')') { fail(c, "missing ), unterminated subpattern"); return; }
            advance(c);
            emit_op(c, RE_OP_END_GROUP, gi);
        } else {
            compile_alternation(c);
            if (c->err[0]) return;
            if (peek(c) != ')') { fail(c, "missing ), unterminated subpattern"); return; }
            advance(c);
        }
        return;
    }

    if (ch == '[') {
        advance(c);
        uint32_t bitmap[8];
        int negate;
        if (parse_char_class(c, bitmap, &negate) < 0) return;
        emit_op(c, RE_OP_CHAR_CLASS, 0);
        RE_Instruction *ins = &c->code[c->code_len - 1];
        memcpy(ins->data.charclass.bitmap, bitmap, 32);
        ins->data.charclass.negate = negate;
        return;
    }

    if (ch == '.') {
        advance(c);
        emit_op(c, RE_OP_DOT, (c->flags & RE_FLAG_S) ? 1 : 0);
        return;
    }

    if (ch == '^') { advance(c); emit_op(c, RE_OP_ASSERT_START, (c->flags & RE_FLAG_M) ? 0 : 1); return; }
    if (ch == '$') { advance(c); emit_op(c, RE_OP_ASSERT_END, (c->flags & RE_FLAG_M) ? 0 : 1); return; }

    if (ch == '\\') {
        advance(c);
        int esc = advance(c);
        if (esc < 0) { fail(c, "bad escape (end of pattern)"); return; }

        if (esc >= '1' && esc <= '9') {
            emit_op(c, RE_OP_BACKREFERENCE, esc - '0');
            return;
        }
        if (esc == 'A') { emit_op(c, RE_OP_ASSERT_START, 1); return; }
        if (esc == 'Z') { emit_op(c, RE_OP_ASSERT_END, 1); return; }
        if (esc == 'b') { emit_op(c, RE_OP_WORD_BOUNDARY, 0); return; }
        if (esc == 'B') { emit_op(c, RE_OP_NOT_WORD_BOUNDARY, 0); return; }

        int val = -1;
        if (esc == 'x' || esc == 'u') {
            val = 0;
            int digits = (esc == 'x') ? 2 : 4;
            for (int i = 0; i < digits; i++) {
                int h = peek(c);
                if (h >= '0' && h <= '9') val = val * 16 + (advance(c) - '0');
                else if (h >= 'a' && h <= 'f') val = val * 16 + (advance(c) - 'a' + 10);
                else if (h >= 'A' && h <= 'F') val = val * 16 + (advance(c) - 'A' + 10);
                else break;
            }
        } else {
            switch (esc) {
            case 'n': val = '\n'; break;
            case 'r': val = '\r'; break;
            case 't': val = '\t'; break;
            case 'f': val = '\f'; break;
            case 'v': val = '\v'; break;
            case '0': val = '\0'; break;
            case 'a': val = '\a'; break;
            case 'd': case 'D': case 'w': case 'W': case 's': case 'S': {
                uint32_t bitmap[8] = {0};
                shorthand_to_bitmap(bitmap, (char)esc);
                emit_op(c, RE_OP_CHAR_CLASS, 0);
                RE_Instruction *ins = &c->code[c->code_len - 1];
                memcpy(ins->data.charclass.bitmap, bitmap, 32);
                ins->data.charclass.negate = 0;
                return;
            }
            default:
                val = esc;   /* \. \* \\ etc */
                break;
            }
        }
        if (val >= 0) {
            if (c->flags & RE_FLAG_I) {
                uint32_t bitmap[8] = {0};
                class_add_char(bitmap, val, 1);
                emit_op(c, RE_OP_CHAR_CLASS, 0);
                RE_Instruction *ins = &c->code[c->code_len - 1];
                memcpy(ins->data.charclass.bitmap, bitmap, 32);
                ins->data.charclass.negate = 0;
            } else {
                emit_op(c, RE_OP_LITERAL, val);
            }
        }
        return;
    }

    /* plain literal */
    advance(c);
    if (c->flags & RE_FLAG_I) {
        uint32_t bitmap[8] = {0};
        class_add_char(bitmap, ch, 1);
        emit_op(c, RE_OP_CHAR_CLASS, 0);
        RE_Instruction *ins = &c->code[c->code_len - 1];
        memcpy(ins->data.charclass.bitmap, bitmap, 32);
        ins->data.charclass.negate = 0;
    } else {
        emit_op(c, RE_OP_LITERAL, ch);
    }
}

/* ---- Sequences and alternation ---- */

static void compile_sequence(Compiler *c) {
    for (;;) {
        skip_x(c);
        int ch = peek(c);
        if (ch < 0 || ch == ')' || ch == '|') break;

        CodeState saved;
        code_save(c, &saved);
        compile_atom(c);
        if (c->err[0]) return;
        int blk_len;
        RE_Instruction *blk = code_take(c, &saved, &blk_len);

        skip_x(c);
        Quant q;
        parse_quant(c, &q);
        if (c->err[0]) { free(blk); return; }

        splice_quant(c, blk, blk_len, &q);
        free(blk);
        if (c->err[0]) return;
    }
}

static void compile_alternation(Compiler *c) {
    /* Branches compile into temp blocks (branch 0 included) so the
     * canonical chain can be laid out without front-insertion. */
    RE_Instruction **blks = NULL;
    int *lens = NULL;
    int nbranch = 0, cap = 0;

    for (;;) {
        CodeState saved;
        code_save(c, &saved);
        compile_sequence(c);
        if (c->err[0]) goto done;
        int l;
        RE_Instruction *b = code_take(c, &saved, &l);
        if (nbranch == cap) {
            cap = cap ? cap * 2 : 4;
            blks = realloc(blks, sizeof(RE_Instruction *) * (size_t)cap);
            lens = realloc(lens, sizeof(int) * (size_t)cap);
        }
        blks[nbranch] = b;
        lens[nbranch] = l;
        nbranch++;
        if (peek(c) != '|') break;
        advance(c);
    }
    if (nbranch == 1) {
        /* single branch: splice it back; done: frees blks[0] */
        emit_block(c, blks[0], lens[0], c->code_len);
        goto done;
    }

    /* Layout (branches B0..Bk, k = nbranch-1), all offsets relative to
     * `base` = c->code_len at entry (may be > 0 inside groups):
     *   base+sp0: SPLIT(a=body0, b=sp1); B0; JUMP(end)
     *   ...
     *   base+spk: Bk
     *   base+end:
     */
    int base = c->code_len;
    int last_body = base;
    for (int i = 0; i < nbranch - 1; i++) last_body += 1 + lens[i] + 1;
    int end = last_body + lens[nbranch - 1];

    int sp = base;
    for (int i = 0; i < nbranch - 1; i++) {
        RE_Instruction s;
        memset(&s, 0, sizeof(s));
        s.op = RE_OP_SPLIT;
        s.data.split.a = sp + 1;
        s.data.split.b = (i + 1 < nbranch - 1) ? sp + 1 + lens[i] + 1 : last_body;
        s.data.split.body = -1;
        s.data.split.loop_idx = -1;
        emit_ins(c, s);
        emit_block(c, blks[i], lens[i], sp + 1);
        emit_op(c, RE_OP_JUMP, 0);
        c->code[c->code_len - 1].data.jump.target = end;
        sp += 1 + lens[i] + 1;
    }
    emit_block(c, blks[nbranch - 1], lens[nbranch - 1], last_body);

done:
    for (int i = 0; i < nbranch; i++) free(blks[i]);
    free(blks);
    free(lens);
}

/* ---- Top level ---- */

RE_Pattern *re_compile(const char *pattern, int flags, char *err_buf, int err_len) {
    Compiler c;
    memset(&c, 0, sizeof(c));
    c.pat = pattern;
    c.len = (int)strlen(pattern);
    c.flags = flags;
    c.err = err_buf;
    c.err_len = err_len;
    err_buf[0] = '\0';

    compile_alternation(&c);
    if (err_buf[0]) {
        free(c.code);
        for (int i = 0; i < c.group_count; i++) free(c.group_names[i]);
        return NULL;
    }
    if (c.pos < c.len) {
        snprintf(err_buf, (size_t)err_len, "unbalanced parenthesis at position %d", c.pos);
        free(c.code);
        for (int i = 0; i < c.group_count; i++) free(c.group_names[i]);
        return NULL;
    }

    emit_op(&c, RE_OP_END, 0);

    RE_Pattern *pat = malloc(sizeof(RE_Pattern));
    pat->code = c.code;
    pat->code_len = c.code_len;
    pat->group_count = c.group_count;
    pat->flags = flags;
    for (int i = 0; i < RE_MAX_GROUPS; i++) pat->group_names[i] = c.group_names[i];

    return pat;
}

void re_free_pattern(RE_Pattern *pat) {
    if (!pat) return;
    free(pat->code);
    for (int i = 0; i < RE_MAX_GROUPS; i++) free(pat->group_names[i]);
    free(pat);
}

int re_group_count(const RE_Pattern *pat) { return pat->group_count; }
