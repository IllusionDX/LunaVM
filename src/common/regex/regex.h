/* regex.h — Regex engine: compiler + backtracking NFA matcher.
 * Self-contained C library, no dependency on the VM or any frontend.
 *
 * Branch targets are ABSOLUTE instruction indices so compiled blocks can
 * be copied when expanding bounded quantifiers (X{m,n} -> m copies + loop).
 */
#ifndef REGEX_H
#define REGEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Opcodes ---- */
typedef enum {
    RE_OP_LITERAL,      /* match one specific char (arg) */
    RE_OP_CHAR_CLASS,   /* match any char in a class (bitmap) */
    RE_OP_DOT,          /* match any char except \n */
    RE_OP_ASSERT_START, /* ^ */
    RE_OP_ASSERT_END,   /* $ */
    RE_OP_WORD_BOUNDARY,
    RE_OP_NOT_WORD_BOUNDARY,
    RE_OP_START_GROUP,  /* ( — arg = group index (1-based) */
    RE_OP_END_GROUP,    /* ) — arg = group index */
    RE_OP_SPLIT,        /* fork: try a first, then b (absolute targets) */
    RE_OP_JUMP,         /* unconditional jump (absolute target) */
    RE_OP_BACKREFERENCE,/* \1..\9 (arg = group index) */
    RE_OP_LOOKAHEAD,    /* body = [ip+1, body_end); arg: 1 positive, 0 negative */
    RE_OP_LOOKBEHIND,   /* body must have ended exactly at current pos */
    RE_OP_END           /* end of pattern */
} RE_OpCode;

typedef struct RE_Instruction {
    RE_OpCode op;
    int arg;
    union {
        struct { uint32_t bitmap[8]; int negate; } charclass;
        struct {
            int a;        /* preferred branch (absolute target) */
            int b;        /* backtrack branch (absolute target) */
            int body;     /* body target for loop splits; -1 for plain */
            int loop_idx; /* >= 0: loop split with empty-cycle guard slot */
        } split;
        struct { int target; } jump;
        struct { int body_end; } look;
    } data;
} RE_Instruction;

#define RE_MAX_GROUPS 256
#define RE_MAX_REPEATS 32   /* loop SPLITs (bounded by empty-cycle guard slots) */

typedef struct RE_Pattern {
    RE_Instruction *code;
    int code_len;
    int group_count;
    char *group_names[RE_MAX_GROUPS]; /* group_names[i-1] = name of group i or NULL */
    int flags;
} RE_Pattern;

/* ---- Match result ---- */
typedef struct RE_Match {
    int start;
    int end;
    int group_count;
    struct { int start; int end; } groups[RE_MAX_GROUPS]; /* -1 = not matched */
} RE_Match;

/* ---- Flags ---- */
#define RE_FLAG_I     0x02   /* case-insensitive */
#define RE_FLAG_M     0x08   /* multiline (^ and $ match line boundaries) */
#define RE_FLAG_S     0x10   /* dotall (. matches \n) */
#define RE_FLAG_U     0x20   /* unicode (no-op for the ASCII engine) */
#define RE_FLAG_X     0x40   /* verbose (no-op for now) */

/* ---- API ---- */

/* Compile a pattern string into a RE_Pattern. Returns NULL on error.
 * The error message is written to err_buf. */
RE_Pattern *re_compile(const char *pattern, int flags, char *err_buf, int err_buf_len);

void re_free_pattern(RE_Pattern *pat);

/* Anchored match at position 0. */
int re_match(const RE_Pattern *pat, const char *str, int len, RE_Match *match_out);

/* Search anywhere in the string. */
int re_search(const RE_Pattern *pat, const char *str, int len, RE_Match *match_out);

/* Find all non-overlapping matches, invoking the callback per match. */
int re_findall(const RE_Pattern *pat, const char *str, int len,
               void (*callback)(const RE_Match *m, const char *str, void *ud), void *ud);

int re_group_count(const RE_Pattern *pat);

#endif /* REGEX_H */
