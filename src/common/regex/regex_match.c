/* regex_match.c — Backtracking NFA regex matcher.
 *
 * Bytecode uses SPLIT/JUMP with absolute targets (Pike/Thompson style);
 * quantifier bounds live in the instruction graph, not in thread state.
 * Thread state lives in a heap-allocated pool: a Thread is ~2.2 KB and a
 * stack allocation of the pool would overflow the 1 MB default stack.
 *
 * Loop splits carry a guard slot (last_pos[loop_idx]) that aborts the loop
 * branch when the body consumed nothing since the previous dispatch, which
 * terminates empty-body loops like (a*)* exactly like CPython does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "regex.h"

typedef struct Thread {
    int ip;                        /* instruction pointer */
    int pos;                       /* position in input string */
    int groups[RE_MAX_GROUPS * 2]; /* (start, end) pairs, -1 = unset */
    int last_pos[RE_MAX_REPEATS];  /* pos at previous loop dispatch, -1 = none */
    struct Thread *next;
} Thread;

#define RE_POOL_SIZE 1024

typedef struct {
    Thread *pool;
    int used;
} ThreadPool;

static Thread *thread_alloc(ThreadPool *tp) {
    if (tp->used >= RE_POOL_SIZE) return NULL;
    return &tp->pool[tp->used++];
}

static inline int is_word_char(int ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
}

static int do_match(const RE_Pattern *pat, const char *str, int str_len,
                    int start_pos, RE_Match *match_out);

/* Lookaround bodies end right before data.look.body_end (exclusive). */
static int run_body(const RE_Pattern *pat, const RE_Instruction *ins,
                    const char *str, int str_len, int pos, RE_Match *m_out) {
    RE_Pattern sub = *pat;
    sub.code = (RE_Instruction *)(ins) + 1;
    sub.code_len = ins->data.look.body_end - (int)((ins - pat->code) + 1);
    return do_match(&sub, str, str_len, pos, m_out);
}

static int do_match(const RE_Pattern *pat, const char *str, int str_len,
                    int start_pos, RE_Match *match_out) {
    ThreadPool tp;
    tp.pool = malloc(sizeof(Thread) * RE_POOL_SIZE);
    if (!tp.pool) return 0;
    tp.used = 0;

    Thread *first = thread_alloc(&tp);
    if (!first) { free(tp.pool); return 0; }
    first->ip = 0;
    first->pos = start_pos;
    memset(first->groups, 0xff, sizeof(first->groups));
    memset(first->last_pos, 0xff, sizeof(first->last_pos));
    first->next = NULL;

    Thread *head = first;
    int ok = 0;
    int best_end = -1;
    int best_groups[RE_MAX_GROUPS * 2];
    memset(best_groups, 0xff, sizeof(best_groups));

    long steps = 0;
    long max_steps = (long)str_len * 256 + 100000;

    while (head && steps < max_steps) {
        steps++;
        Thread *t = head;
        head = t->next;

        if (t->ip >= pat->code_len || pat->code[t->ip].op == RE_OP_END) {
            best_end = t->pos;
            memcpy(best_groups, t->groups, sizeof(best_groups));
            ok = 1;
            break;
        }

        RE_Instruction *ins = &pat->code[t->ip];
        int pos = t->pos;
        int nip = -1;      /* next ip; -1 = thread dies */
        int npos = pos;

        switch (ins->op) {
        case RE_OP_LITERAL:
            if (pos < str_len && (unsigned char)str[pos] == ins->arg) {
                nip = t->ip + 1;
                npos = pos + 1;
            }
            break;

        case RE_OP_CHAR_CLASS: {
            if (pos < str_len) {
                int ch = (unsigned char)str[pos];
                if (ch < 128) {
                    int found = (ins->data.charclass.bitmap[ch / 32] >> (ch % 32)) & 1;
                    if (ins->data.charclass.negate) found = !found;
                    if (found) { nip = t->ip + 1; npos = pos + 1; }
                }
            }
            break;
        }

        case RE_OP_DOT:
            if (pos < str_len && !((unsigned char)str[pos] == '\n' && ins->arg == 0)) {
                nip = t->ip + 1;
                npos = pos + 1;
            }
            break;

        case RE_OP_ASSERT_START:
            if (ins->arg == 1 ? pos == 0
                              : (pos == 0 || (unsigned char)str[pos - 1] == '\n'))
                nip = t->ip + 1;
            break;

        case RE_OP_ASSERT_END:
            if (ins->arg == 1 ? pos == str_len
                              : (pos == str_len || (unsigned char)str[pos] == '\n'))
                nip = t->ip + 1;
            break;

        case RE_OP_WORD_BOUNDARY:
        case RE_OP_NOT_WORD_BOUNDARY: {
            int before = (pos > 0 && is_word_char((unsigned char)str[pos - 1]));
            int after = (pos < str_len && is_word_char((unsigned char)str[pos]));
            int at_boundary = (before != after);
            if (at_boundary == (ins->op == RE_OP_WORD_BOUNDARY)) nip = t->ip + 1;
            break;
        }

        case RE_OP_START_GROUP: {
            int gi = ins->arg;
            if (gi >= 1 && gi <= RE_MAX_GROUPS) {
                t->groups[(gi - 1) * 2] = pos;
                t->groups[(gi - 1) * 2 + 1] = -1;
            }
            nip = t->ip + 1;
            break;
        }

        case RE_OP_END_GROUP: {
            int gi = ins->arg;
            if (gi >= 1 && gi <= RE_MAX_GROUPS) {
                t->groups[(gi - 1) * 2 + 1] = pos;
            }
            nip = t->ip + 1;
            break;
        }

        case RE_OP_SPLIT: {
            int li = ins->data.split.loop_idx;
            int pref = ins->data.split.a;
            int alt = ins->data.split.b;
            int body = ins->data.split.body;

            if (li >= 0 && t->last_pos[li] == pos) {
                /* Empty-cycle guard (CPython's rep->last_ptr / Russ Cox's
                 * progress rule): the body consumed nothing since the last
                 * dispatch, so this iteration is discarded entirely. The
                 * thread dies; backtracking resumes at the exit-branch fork
                 * from the previous arrival, which holds the pre-empty-
                 * iteration captures (Python keeps those, not the empty
                 * one: re.match(r'(a*)*', 'aaa').group(1) == 'aaa'). */
                break;
            }

            /* fork the backtracking branch */
            Thread *nt = thread_alloc(&tp);
            if (nt) {
                nt->ip = alt;
                nt->pos = pos;
                memcpy(nt->groups, t->groups, sizeof(nt->groups));
                memcpy(nt->last_pos, t->last_pos, sizeof(nt->last_pos));
                if (li >= 0) nt->last_pos[li] = (alt == body) ? pos : -1;
                nt->next = head;
                head = nt;
            }

            /* current thread continues on the preferred branch */
            t->ip = pref;
            if (li >= 0) t->last_pos[li] = (pref == body) ? pos : -1;
            t->next = head;
            head = t;
            continue;
        }

        case RE_OP_JUMP:
            nip = ins->data.jump.target;
            break;

        case RE_OP_BACKREFERENCE: {
            int gi = ins->arg;
            if (gi >= 1 && gi <= RE_MAX_GROUPS) {
                int gs = t->groups[(gi - 1) * 2];
                int ge = t->groups[(gi - 1) * 2 + 1];
                if (gs >= 0 && ge >= 0) {
                    int glen = ge - gs;
                    if (pos + glen <= str_len) {
                        int match = 1;
                        for (int i = 0; i < glen; i++) {
                            int a = (unsigned char)str[pos + i];
                            int b = (unsigned char)str[gs + i];
                            if (pat->flags & RE_FLAG_I) {
                                if (tolower(a) != tolower(b)) { match = 0; break; }
                            } else if (a != b) { match = 0; break; }
                        }
                        if (match) { nip = t->ip + 1; npos = pos + glen; }
                    }
                }
            }
            break;
        }

        case RE_OP_LOOKAHEAD: {
            int found = run_body(pat, ins, str, str_len, pos, NULL);
            if (found == ins->arg) nip = ins->data.look.body_end;
            break;
        }

        case RE_OP_LOOKBEHIND: {
            int body_start = (int)((ins - pat->code) + 1);
            int body_len = ins->data.look.body_end - body_start;

            /* fixed-width fast path */
            int width = 0, fixed = 1;
            for (int i = body_start; i < body_start + body_len; i++) {
                RE_OpCode op = pat->code[i].op;
                if (op == RE_OP_LITERAL || op == RE_OP_DOT || op == RE_OP_CHAR_CLASS) width++;
                else { fixed = 0; break; }
            }

            int found = 0;
            if (fixed) {
                int s = pos - width;
                if (s >= 0) {
                    RE_Match m;
                    if (run_body(pat, ins, str, str_len, s, &m) && m.end == pos) found = 1;
                }
            } else {
                for (int s = 0; s <= pos && !found; s++) {
                    RE_Match m;
                    if (run_body(pat, ins, str, str_len, s, &m) && m.end == pos) found = 1;
                }
            }
            if (found == ins->arg) nip = ins->data.look.body_end;
            break;
        }

        case RE_OP_END:
            nip = t->ip; /* unreachable; handled by top-of-loop check */
            break;

        default:
            break;
        }

        if (nip >= 0) {
            t->ip = nip;
            t->pos = npos;
            t->next = head;
            head = t;
        }
        /* else: thread dies */
    }

    if (ok && match_out) {
        match_out->start = start_pos;
        match_out->end = best_end;
        match_out->group_count = pat->group_count;
        for (int i = 0; i < pat->group_count; i++) {
            match_out->groups[i].start = best_groups[i * 2];
            match_out->groups[i].end = best_groups[i * 2 + 1];
        }
    }

    free(tp.pool);
    return ok;
}

int re_match(const RE_Pattern *pat, const char *str, int len, RE_Match *match_out) {
    /* re.match anchors at 0 — except ^ with re.M, which matches at any
     * line start, so scan like search in that case only. */
    if (pat->code_len > 0 && pat->code[0].op == RE_OP_ASSERT_START && pat->code[0].arg == 0) {
        int i;
        for (i = 0; i <= len; i++) {
            if (i > 0 && (unsigned char)str[i - 1] != '\n') continue;
            if (do_match(pat, str, len, i, match_out)) return 1;
        }
        return 0;
    }
    return do_match(pat, str, len, 0, match_out);
}

int re_search(const RE_Pattern *pat, const char *str, int len, RE_Match *match_out) {
    /* anchored absolute-start patterns only match at 0 */
    if (pat->code_len > 0 && pat->code[0].op == RE_OP_ASSERT_START && pat->code[0].arg == 1)
        return do_match(pat, str, len, 0, match_out);
    for (int i = 0; i <= len; i++) {
        if (do_match(pat, str, len, i, match_out)) return 1;
    }
    return 0;
}

int re_findall(const RE_Pattern *pat, const char *str, int len,
               void (*callback)(const RE_Match *m, const char *str, void *ud), void *ud) {
    int count = 0;
    int pos = 0;
    RE_Match m;
    while (pos <= len) {
        if (do_match(pat, str, len, pos, &m)) {
            callback(&m, str, ud);
            count++;
            pos = (m.end > pos) ? m.end : pos + 1;
        } else {
            pos++;
        }
    }
    return count;
}
