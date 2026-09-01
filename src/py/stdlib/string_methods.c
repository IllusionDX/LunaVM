/* string_methods.c — Python-subset string methods for the py frontend.
 *
 * Index-based methods operate on code points (not bytes), matching the
 * frontend's string indexing/slicing semantics. Case mapping covers ASCII
 * and Latin-1 (U+00C0..U+00FF) only, like the upper/lower builtins.
 *
 * Call convention: args[0] = self. Natives are eager-resolved on bare
 * attribute access with nargs == 1, so arg-taking methods return null
 * (instead of raising) when their arguments are missing, matching
 * list_method_add's behavior for bare "lst.append".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm.h"
#include "value.h"
#include "py/object.h"
#include "py/frontend_state.h"

/* ---- argument extraction helpers -------------------------------------- */

/* Extract self; throws TypeError when the receiver is not a string. */
static ObjString *sm_self(VM *vm, Value *args) {
    if (args && IS_STRING(args[0])) return (ObjString *)AS_OBJ(args[0]);
    luna_throw(vm, py_fe(vm)->type_error_class, "string method needs a string receiver");
    return NULL;
}

/* Optional string argument at args[i]: NULL when missing, throws on type. */
static ObjString *sm_opt_str(VM *vm, Value *args, int nargs, int i) {
    if (nargs <= i) return NULL;
    if (IS_STRING(args[i])) return (ObjString *)AS_OBJ(args[i]);
    luna_throw(vm, py_fe(vm)->type_error_class, "expected a string argument");
    return NULL;
}

/* Required string argument: throws TypeError when missing or wrong type. */
static ObjString *sm_req_str(VM *vm, Value *args, int nargs, int i) {
    ObjString *s = sm_opt_str(vm, args, nargs, i);
    if (!s) luna_throw(vm, py_fe(vm)->type_error_class, "expected a string argument");
    return s;
}

/* Optional integer argument: false when missing, throws on bad type/range. */
static bool sm_opt_int(VM *vm, Value *args, int nargs, int i, int *out) {
    if (nargs <= i) return false;
    if (!IS_INT(args[i]) && !IS_BIGINT(args[i])) {
        luna_throw(vm, py_fe(vm)->type_error_class, "expected an integer argument");
    }
    int64_t x;
    if (!int64_exact(args[i], &x)) {
        luna_throw(vm, py_fe(vm)->overflow_error_class,
                   "Python int too large to convert to C ssize_t");
    }
    *out = (x > INT32_MAX) ? INT32_MAX : (x < INT32_MIN) ? INT32_MIN : (int)x;
    return true;
}

/* Required integer argument. */
static int sm_req_int(VM *vm, Value *args, int nargs, int i) {
    int v = 0;
    if (!sm_opt_int(vm, args, nargs, i, &v)) {
        luna_throw(vm, py_fe(vm)->type_error_class, "expected an integer argument");
    }
    return v;
}

/* Optional int-or-None argument: -1 sentinel when missing or None. */
static int sm_int_or_none(VM *vm, Value *args, int nargs, int i) {
    if (nargs <= i || IS_NIL(args[i])) return -1;
    int v = 0;
    if (!sm_opt_int(vm, args, nargs, i, &v)) {
        luna_throw(vm, py_fe(vm)->type_error_class, "expected an integer or None");
    }
    return v;
}

/* ---- UTF-8 helpers ------------------------------------------------------ */

/* Decode the code point at chars[*i], advancing *i past it. */
static int sm_utf8_decode(const char *chars, int *i) {
    unsigned char c = (unsigned char)chars[*i];
    int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    int cp;
    if (len == 1) { cp = c; (*i)++; return cp; }
    cp = c & (0x7F >> len);
    for (int k = 1; k < len && (unsigned char)chars[*i + k]; k++)
        cp = (cp << 6) | ((unsigned char)chars[*i + k] & 0x3F);
    *i += len;
    return cp;
}

/* Encode cp into buf as UTF-8; returns the byte count (1..4). */
static int sm_utf8_encode(int cp, char *buf) {
    if (cp < 0x80) { buf[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Case mapping for one code point; false when cp has no single-cp mapping. */
static bool sm_case_map(int cp, bool upper, int *out) {
    if (upper) {
        if (cp >= 'a' && cp <= 'z') { *out = cp - 32; return true; }
        if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) { *out = cp - 32; return true; }
    } else {
        if (cp >= 'A' && cp <= 'Z') { *out = cp + 32; return true; }
        if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) { *out = cp + 32; return true; }
    }
    return false;
}

/* Cased-classification helpers (ASCII + Latin-1 letters). */
static bool sm_is_upper_cp(int cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7);
}

static bool sm_is_lower_cp(int cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7);
}

/* Code-point start offsets: (*out)[k] = byte offset of code point k.
 * Returns the code point count. */
static int sm_cp_starts(const ObjString *s, int **out) {
    int byte_len = s->length;
    int cp_count = utf8_code_point_count(s->chars, byte_len);
    int *starts = malloc((size_t)cp_count * sizeof(int));
    if (!starts) { fprintf(stderr, "OOM\n"); exit(1); }
    int ci = 0;
    for (int i = 0; i < byte_len; i++)
        if ((s->chars[i] & 0xC0) != 0x80) starts[ci++] = i;
    *out = starts;
    return cp_count;
}

/* Normalize Python-style [start, end) bounds over n code points. -1 is the
 * None/missing sentinel; negatives wrap; everything clamps into [0, n]. */
static void sm_bounds(int n, int start, int end, int *lo, int *hi) {
    if (start == -1) start = 0;
    if (end == -1) end = n;
    if (start < 0) start += n;
    if (end < 0) end += n;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (start > n) start = n;
    if (end > n) end = n;
    *lo = start;
    *hi = end;
}

/* Optional (start, end) bounds at args[i], args[i+1] over n code points. */
static void sm_bounds_args(VM *vm, Value *args, int nargs, int i, int n,
                           int *lo, int *hi) {
    int start = sm_int_or_none(vm, args, nargs, i);
    int end = sm_int_or_none(vm, args, nargs, i + 1);
    sm_bounds(n, start, end, lo, hi);
}

/* Optional fill character at args[i]; default " ". Throws on non-single-cp. */
static const char *sm_fill(VM *vm, Value *args, int nargs, int i, int *fill_len) {
    if (nargs <= i) { *fill_len = 1; return " "; }
    ObjString *f = sm_opt_str(vm, args, nargs, i);
    int j = 0;
    sm_utf8_decode(f->chars, &j);
    if (j != f->length) {
        luna_throw(vm, py_fe(vm)->type_error_class,
                   "fill character must be a single character");
    }
    *fill_len = j;
    return f->chars;
}

/* Build an ObjString from a heap buffer (frees buf). */
static Value sm_take(char *buf, int out_len) {
    ObjString *result = new_string(buf, out_len);
    free(buf);
    return make_obj((Object *)result);
}

/* Build a 3-tuple of strings (head, sep, tail). */
static Value sm_partition3(const ObjString *head, const ObjString *sep,
                           const ObjString *tail) {
    ObjTuple *t = new_tuple(3);
    t->items[0] = make_obj((Object *)new_string(head->chars, head->length));
    t->items[1] = make_obj((Object *)new_string(sep->chars, sep->length));
    t->items[2] = make_obj((Object *)new_string(tail->chars, tail->length));
    return make_obj((Object *)t);
}

/* Extract a list or tuple receiver's items for join. */
static bool sm_iter_items(Value v, Value **items, int *count) {
    if (IS_LIST(v)) {
        ObjList *l = (ObjList *)AS_OBJ(v);
        *items = l->items ? l->items : l->inline_items;
        *count = l->count;
        return true;
    }
    if (IS_TUPLE(v)) {
        ObjTuple *t = (ObjTuple *)AS_OBJ(v);
        *items = t->items;
        *count = t->count;
        return true;
    }
    return false;
}

/* ============================================================ */
/* Predicate methods                                             */
/* ============================================================ */

/* Shared single-pass predicate: fn(cp) decides per code point. */
static Value sm_pred(VM *vm, Value *args, bool (*fn)(int), bool empty_false) {
    ObjString *s = sm_self(vm, args);
    if (s->length == 0) return make_bool(!empty_false);
    int i = 0;
    while (i < s->length) {
        int cp = sm_utf8_decode(s->chars, &i);
        if (!fn(cp)) return make_bool(false);
    }
    return make_bool(true);
}

static bool sm_is_alpha(int cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
           (cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7);
}

static bool sm_is_digit(int cp) { return cp >= '0' && cp <= '9'; }

static bool sm_is_alnum(int cp) { return sm_is_alpha(cp) || sm_is_digit(cp); }

static bool sm_is_space(int cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\v' ||
           cp == '\f' || cp == '\r' || cp == 0x85;
}

static bool sm_is_printable(int cp) {
    return !(cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F));
}

static Value string_method_isalpha(VM *vm, Value *args, int nargs) {
    (void)nargs;
    return sm_pred(vm, args, sm_is_alpha, true);
}

static Value string_method_isdigit(VM *vm, Value *args, int nargs) {
    (void)nargs;
    return sm_pred(vm, args, sm_is_digit, true);
}

static Value string_method_isalnum(VM *vm, Value *args, int nargs) {
    (void)nargs;
    return sm_pred(vm, args, sm_is_alnum, true);
}

static Value string_method_isspace(VM *vm, Value *args, int nargs) {
    (void)nargs;
    return sm_pred(vm, args, sm_is_space, true);
}

static Value string_method_isprintable(VM *vm, Value *args, int nargs) {
    (void)nargs;
    return sm_pred(vm, args, sm_is_printable, true);
}

static Value string_method_isascii(VM *vm, Value *args, int nargs) {
    (void)nargs;
    ObjString *s = sm_self(vm, args);
    for (int i = 0; i < s->length; i++)
        if ((unsigned char)s->chars[i] >= 0x80) return make_bool(false);
    return make_bool(true);
}

static Value string_method_isidentifier(VM *vm, Value *args, int nargs) {
    (void)nargs;
    ObjString *s = sm_self(vm, args);
    if (s->length == 0) return make_bool(false);
    int i = 0, first = 1;
    while (i < s->length) {
        int cp = sm_utf8_decode(s->chars, &i);
        bool ok = sm_is_alpha(cp) || cp == '_' || (!first && sm_is_digit(cp));
        if (!ok) return make_bool(false);
        first = 0;
    }
    return make_bool(true);
}

static Value string_method_isupper(VM *vm, Value *args, int nargs) {
    (void)nargs;
    ObjString *s = sm_self(vm, args);
    if (s->length == 0) return make_bool(false);
    int i = 0, cased = 0;
    while (i < s->length) {
        int cp = sm_utf8_decode(s->chars, &i);
        if (sm_is_lower_cp(cp)) return make_bool(false);
        if (sm_is_upper_cp(cp)) cased = 1;
    }
    return make_bool(cased);
}

static Value string_method_islower(VM *vm, Value *args, int nargs) {
    (void)nargs;
    ObjString *s = sm_self(vm, args);
    if (s->length == 0) return make_bool(false);
    int i = 0, cased = 0;
    while (i < s->length) {
        int cp = sm_utf8_decode(s->chars, &i);
        if (sm_is_upper_cp(cp)) return make_bool(false);
        if (sm_is_lower_cp(cp)) cased = 1;
    }
    return make_bool(cased);
}

/* istitle: upper cps must follow an uncased one, lower cps a cased one. */
static Value string_method_istitle(VM *vm, Value *args, int nargs) {
    (void)nargs;
    ObjString *s = sm_self(vm, args);
    if (s->length == 0) return make_bool(false);
    int i = 0, cased = 0, prev_cased = 0;
    while (i < s->length) {
        int cp = sm_utf8_decode(s->chars, &i);
        bool up = sm_is_upper_cp(cp), lo = sm_is_lower_cp(cp);
        if (up && prev_cased) return make_bool(false);
        if (lo && !prev_cased) return make_bool(false);
        if (up || lo) { cased = 1; prev_cased = 1; }
        else prev_cased = 0;
    }
    return make_bool(cased);
}

/* ============================================================ */
/* Case conversion methods                                       */
/* ============================================================ */

static Value string_method_capitalize(VM *vm, Value *args, int nargs) {
    (void)nargs;
    ObjString *s = sm_self(vm, args);
    int i = 0, pos = 0, first = 1;
    char *buf = malloc((size_t)s->length + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    while (i < s->length) {
        int start = i;
        int cp = sm_utf8_decode(s->chars, &i);
        int mapped;
        if (sm_case_map(cp, first, &mapped)) {
            pos += sm_utf8_encode(mapped, buf + pos);
        } else {
            memcpy(buf + pos, s->chars + start, (size_t)(i - start));
            pos += i - start;
        }
        first = 0;
    }
    buf[pos] = '\0';
    (void)vm;
    return sm_take(buf, pos);
}

/* title: uppercase a cased cp after an uncased one; lowercase after cased. */
static Value string_method_title(VM *vm, Value *args, int nargs) {
    (void)nargs;
    ObjString *s = sm_self(vm, args);
    int i = 0, pos = 0, prev_cased = 0;
    char *buf = malloc((size_t)s->length + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    while (i < s->length) {
        int start = i;
        int cp = sm_utf8_decode(s->chars, &i);
        bool up = sm_is_upper_cp(cp), lo = sm_is_lower_cp(cp);
        int mapped;
        if ((up || lo) && sm_case_map(cp, !prev_cased, &mapped)) {
            pos += sm_utf8_encode(mapped, buf + pos);
        } else {
            memcpy(buf + pos, s->chars + start, (size_t)(i - start));
            pos += i - start;
        }
        prev_cased = up || lo;
    }
    buf[pos] = '\0';
    (void)vm;
    return sm_take(buf, pos);
}

static Value string_method_swapcase(VM *vm, Value *args, int nargs) {
    (void)nargs;
    ObjString *s = sm_self(vm, args);
    int i = 0, pos = 0;
    char *buf = malloc((size_t)s->length + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    while (i < s->length) {
        int start = i;
        int cp = sm_utf8_decode(s->chars, &i);
        int mapped;
        if (sm_case_map(cp, sm_is_lower_cp(cp), &mapped)) {
            pos += sm_utf8_encode(mapped, buf + pos);
        } else {
            memcpy(buf + pos, s->chars + start, (size_t)(i - start));
            pos += i - start;
        }
    }
    buf[pos] = '\0';
    (void)vm;
    return sm_take(buf, pos);
}

/* ============================================================ */
/* Search methods                                                */
/* ============================================================ */

/* Shared scanner for find/rfind/count. dir < 0 scans backwards. Returns the
 * first (or last) match's cp index, or -1; *matches receives the
 * non-overlapping match count within [lo, hi). */
static int sm_scan(const ObjString *s, const ObjString *sub, const int *starts,
                   int lo, int hi, int dir, long *matches) {
    int sub_n = utf8_code_point_count(sub->chars, sub->length);
    int found = -1;
    long count = 0;
    if (sub_n == 0) {
        /* CPython: empty needle matches at every boundary. */
        found = (dir < 0) ? hi : lo;
        count = (long)(hi - lo) + 1;
    } else if (dir > 0) {
        for (int k = lo; k + sub_n <= hi; k++) {
            if (memcmp(s->chars + starts[k], sub->chars, (size_t)sub->length) == 0) {
                if (found < 0) found = k;
                count++;
                k += sub_n - 1; /* non-overlapping */
            }
        }
    } else {
        for (int k = hi - sub_n; k >= lo; k--) {
            if (memcmp(s->chars + starts[k], sub->chars, (size_t)sub->length) == 0) {
                if (found < 0) found = k;
                count++;
                k -= sub_n - 1; /* non-overlapping */
            }
        }
    }
    *matches = count;
    return found;
}

/* Shared find/rfind/index/rindex body. Returns the cp index or -1. */
static long sm_search(VM *vm, Value *args, int nargs, int dir) {
    ObjString *s = sm_self(vm, args);
    if (nargs < 2) return -1; /* bare access: no needle given */
    ObjString *sub = sm_req_str(vm, args, nargs, 1);
    int *starts = NULL;
    int n = sm_cp_starts(s, &starts);
    int lo, hi;
    sm_bounds_args(vm, args, nargs, 2, n, &lo, &hi);
    long matches = 0;
    long found = sm_scan(s, sub, starts, lo, hi, dir, &matches);
    free(starts);
    (void)vm;
    return found;
}

static Value string_method_find(VM *vm, Value *args, int nargs) {
    return make_int((int)sm_search(vm, args, nargs, 1));
}

static Value string_method_rfind(VM *vm, Value *args, int nargs) {
    return make_int((int)sm_search(vm, args, nargs, -1));
}

static Value string_method_index(VM *vm, Value *args, int nargs) {
    long found = sm_search(vm, args, nargs, 1);
    if (found < 0 && nargs >= 2) {
        luna_throw(vm, py_fe(vm)->value_error_class, "substring not found");
    }
    return make_int((int)found);
}

static Value string_method_rindex(VM *vm, Value *args, int nargs) {
    long found = sm_search(vm, args, nargs, -1);
    if (found < 0 && nargs >= 2) {
        luna_throw(vm, py_fe(vm)->value_error_class, "substring not found");
    }
    return make_int((int)found);
}

static Value string_method_count(VM *vm, Value *args, int nargs) {
    ObjString *s = sm_self(vm, args);
    if (nargs < 2) return make_int(0); /* bare access: no needle given */
    ObjString *sub = sm_req_str(vm, args, nargs, 1);
    int *starts = NULL;
    int n = sm_cp_starts(s, &starts);
    int lo, hi;
    sm_bounds_args(vm, args, nargs, 2, n, &lo, &hi);
    long matches = 0;
    sm_scan(s, sub, starts, lo, hi, 1, &matches);
    free(starts);
    return make_int((int)matches);
}

/* spec is a string or tuple of strings; matched against [lo, hi) at the
 * start (at_end = false) or end (at_end = true) of the range. */
static bool sm_spec_match(VM *vm, Value spec, const ObjString *s, const int *starts,
                          int lo, int hi, bool at_end, bool *result) {
    if (IS_STRING(spec)) {
        ObjString *m = (ObjString *)AS_OBJ(spec);
        int m_n = utf8_code_point_count(m->chars, m->length);
        if (m_n == 0) { *result = true; return true; }
        if (at_end) {
            if (hi - m_n < lo) { *result = false; return true; }
            *result = memcmp(s->chars + starts[hi - m_n], m->chars,
                             (size_t)m->length) == 0;
        } else {
            if (lo + m_n > hi) { *result = false; return true; }
            *result = memcmp(s->chars + starts[lo], m->chars,
                             (size_t)m->length) == 0;
        }
        return true;
    }
    if (IS_TUPLE(spec)) {
        ObjTuple *t = (ObjTuple *)AS_OBJ(spec);
        for (int k = 0; k < t->count; k++) {
            bool r = false;
            if (!sm_spec_match(vm, t->items[k], s, starts, lo, hi, at_end, &r)) {
                return false;
            }
            if (r) { *result = true; return true; }
        }
        *result = false;
        return true;
    }
    luna_throw(vm, py_fe(vm)->type_error_class, "expected a string or tuple of strings");
    return false;
}

/* Shared startswith/endswith body. */
static Value sm_affix(VM *vm, Value *args, int nargs, bool at_end) {
    ObjString *s = sm_self(vm, args);
    if (nargs < 2) return make_null(); /* bare access */
    int *starts = NULL;
    int n = sm_cp_starts(s, &starts);
    int lo, hi;
    sm_bounds_args(vm, args, nargs, 2, n, &lo, &hi);
    bool result = false;
    bool ok = sm_spec_match(vm, args[1], s, starts, lo, hi, at_end, &result);
    free(starts);
    if (!ok) return make_null();
    return make_bool(result);
}

static Value string_method_startswith(VM *vm, Value *args, int nargs) {
    return sm_affix(vm, args, nargs, false);
}

static Value string_method_endswith(VM *vm, Value *args, int nargs) {
    return sm_affix(vm, args, nargs, true);
}

/* ============================================================ */
/* Transformation methods                                        */
/* ============================================================ */

static Value string_method_replace(VM *vm, Value *args, int nargs) {
    ObjString *s = sm_self(vm, args);
    if (nargs < 3) return make_null(); /* bare access or missing argument */
    ObjString *old = sm_req_str(vm, args, nargs, 1);
    ObjString *new = sm_req_str(vm, args, nargs, 2);
    long limit = -1; /* -1 = replace all */
    if (nargs >= 4 && !IS_NIL(args[3])) {
        int c = 0;
        if (!sm_opt_int(vm, args, nargs, 3, &c)) return make_null();
        limit = c;
    }
    int old_n = utf8_code_point_count(old->chars, old->length);
    int n = utf8_code_point_count(s->chars, s->length);
    /* Old is empty: CPython inserts new before each cp and after the last. */
    if (old_n == 0) {
        long slots = (long)n + 1;
        if (limit >= 0 && limit < slots) slots = limit;
        if (slots < 0) slots = 0;
        char *buf = malloc((size_t)s->length + (size_t)slots * (size_t)new->length + 1);
        if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
        int pos = 0, i = 0;
        long ins = 0;
        while (i <= s->length) {
            if (limit < 0 || ins < limit) {
                memcpy(buf + pos, new->chars, (size_t)new->length);
                pos += new->length;
                ins++;
            }
            if (i < s->length) {
                int j = i;
                sm_utf8_decode(s->chars, &j);
                memcpy(buf + pos, s->chars + i, (size_t)(j - i));
                pos += j - i;
                i = j;
            } else break;
        }
        buf[pos] = '\0';
        return sm_take(buf, pos);
    }
    int *starts = NULL;
    sm_cp_starts(s, &starts);
    /* Collect match cp positions (up to the limit). */
    int *hits = malloc((size_t)n * sizeof(int));
    if (!hits) { fprintf(stderr, "OOM\n"); exit(1); }
    int hit_count = 0;
    for (int k = 0; k + old_n <= n; k++) {
        if (memcmp(s->chars + starts[k], old->chars, (size_t)old->length) == 0) {
            hits[hit_count++] = k;
            k += old_n - 1;
            if (limit >= 0 && hit_count >= limit) break;
        }
    }
    if (hit_count == 0) { free(starts); free(hits); return args[0]; }
    long delta = (long)hit_count * ((long)new->length - (long)old->length);
    char *buf = malloc((size_t)s->length + (size_t)(delta > 0 ? delta : 0) + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    int pos = 0, k = 0, h = 0;
    while (k < n) {
        if (h < hit_count && hits[h] == k) {
            memcpy(buf + pos, new->chars, (size_t)new->length);
            pos += new->length;
            k += old_n;
            h++;
        } else {
            int end = (k + 1 < n) ? starts[k + 1] : s->length;
            memcpy(buf + pos, s->chars + starts[k], (size_t)(end - starts[k]));
            pos += end - starts[k];
            k++;
        }
    }
    buf[pos] = '\0';
    free(starts);
    free(hits);
    return sm_take(buf, pos);
}

/* Shared split/rsplit body. sep == NULL splits on whitespace runs.
 * byte_of maps a cp index to its byte offset; cp index n maps to the end. */
static Value string_do_split(VM *vm, ObjString *s, ObjString *sep, long maxsplit,
                             bool from_right) {
    int n;
    int *starts = NULL;
    n = sm_cp_starts(s, &starts);
#define SM_BYTE_OF(k) (((k) >= n) ? s->length : starts[k])
    ObjList *out = new_list(0);
    if (!sep) {
        /* Whitespace split: leading/trailing runs are separators. */
        if (!from_right) {
            int i = 0, pieces = 0;
            while (i < n) {
                int b = starts[i];
                if (sm_is_space(sm_utf8_decode(s->chars, &b))) { i++; continue; }
                if (maxsplit >= 0 && pieces >= maxsplit) {
                    /* remainder is verbatim to the end (keeps trailing ws) */
                    list_add(out, make_obj((Object *)new_string(
                        s->chars + starts[i], s->length - starts[i])));
                    break;
                }
                int j = i + 1;
                while (j < n) {
                    int b2 = starts[j];
                    if (sm_is_space(sm_utf8_decode(s->chars, &b2))) break;
                    j++;
                }
                list_add(out, make_obj((Object *)new_string(
                    s->chars + starts[i], SM_BYTE_OF(j) - starts[i])));
                pieces++;
                i = j;
            }
        } else {
            /* From the right; the leftmost remainder keeps leading ws. */
            int i = n - 1, pieces = 0;
            while (i >= 0) {
                int b = starts[i];
                if (sm_is_space(sm_utf8_decode(s->chars, &b))) { i--; continue; }
                if (maxsplit >= 0 && pieces >= maxsplit) {
                    list_add(out, make_obj((Object *)new_string(s->chars, SM_BYTE_OF(i + 1))));
                    break;
                }
                int j = i - 1;
                while (j >= 0) {
                    int b2 = starts[j];
                    if (sm_is_space(sm_utf8_decode(s->chars, &b2))) break;
                    j--;
                }
                list_add(out, make_obj((Object *)new_string(
                    s->chars + SM_BYTE_OF(j + 1), SM_BYTE_OF(i + 1) - SM_BYTE_OF(j + 1))));
                pieces++;
                i = j;
            }
        }
        free(starts);
        return make_obj((Object *)out);
    }
    int sep_n = utf8_code_point_count(sep->chars, sep->length);
    if (sep_n == 0) {
        luna_throw(vm, py_fe(vm)->value_error_class, "empty separator");
    }
    if (from_right) {
        /* Find up to maxsplit separators scanning right to left, then emit
         * pieces left to right: head, between matches, tail. */
        int *ms = malloc((size_t)n * sizeof(int));
        if (!ms) { fprintf(stderr, "OOM\n"); exit(1); }
        int mc = 0, cut = n;
        while (cut > 0) {
            if (maxsplit >= 0 && mc >= maxsplit) break;
            int m = -1;
            for (int k = cut - sep_n; k >= 0; k--) {
                if (memcmp(s->chars + starts[k], sep->chars, (size_t)sep->length) == 0) {
                    m = k;
                    break;
                }
            }
            if (m < 0) break;
            ms[mc++] = m;
            cut = m;
        }
        if (mc == 0) {
            list_add(out, make_obj((Object *)new_string(s->chars, s->length)));
        } else {
            list_add(out, make_obj((Object *)new_string(s->chars, SM_BYTE_OF(ms[mc - 1]))));
            for (int j = mc - 1; j >= 0; j--) {
                int ts = SM_BYTE_OF(ms[j] + sep_n);
                int te = (j == 0) ? s->length : SM_BYTE_OF(ms[j - 1]);
                list_add(out, make_obj((Object *)new_string(s->chars + ts, te - ts)));
            }
        }
        free(ms);
    } else {
        int k = 0, pieces = 0;
        while (1) {
            if (maxsplit >= 0 && pieces >= maxsplit) {
                list_add(out, make_obj((Object *)new_string(
                    s->chars + SM_BYTE_OF(k), s->length - SM_BYTE_OF(k))));
                break;
            }
            int m = -1;
            for (int j = k; j + sep_n <= n; j++) {
                if (memcmp(s->chars + starts[j], sep->chars, (size_t)sep->length) == 0) {
                    m = j;
                    break;
                }
            }
            if (m < 0) {
                list_add(out, make_obj((Object *)new_string(
                    s->chars + SM_BYTE_OF(k), s->length - SM_BYTE_OF(k))));
                break;
            }
            list_add(out, make_obj((Object *)new_string(
                s->chars + starts[k], starts[m] - starts[k])));
            pieces++;
            k = m + sep_n;
        }
    }
    free(starts);
    return make_obj((Object *)out);
#undef SM_BYTE_OF
}

/* split(None, n) / split(None) split on whitespace: sep == None (nil) or
 * omitted means whitespace mode. */
static Value string_method_split_ws(VM *vm, Value *args, int nargs, bool rsplit) {
    ObjString *s = sm_self(vm, args);
    ObjString *sep = NULL;
    bool ws = true;
    if (nargs >= 2 && !IS_NIL(args[1])) {
        sep = sm_opt_str(vm, args, nargs, 1);
        ws = false;
    }
    long maxsplit = -1;
    if (nargs >= 3 && !IS_NIL(args[2])) {
        int c = 0;
        if (!sm_opt_int(vm, args, nargs, 2, &c)) return make_null();
        maxsplit = c;
    }
    return string_do_split(vm, s, ws ? NULL : sep, maxsplit, rsplit);
}

static Value string_method_split(VM *vm, Value *args, int nargs) {
    return string_method_split_ws(vm, args, nargs, false);
}

static Value string_method_rsplit(VM *vm, Value *args, int nargs) {
    return string_method_split_ws(vm, args, nargs, true);
}

/* splitlines: split on \n, \r, \r\n (and \v, \f, \x1c..\x1e, \x85). */
static Value string_method_splitlines(VM *vm, Value *args, int nargs) {
    ObjString *s = sm_self(vm, args);
    bool keepends = (nargs >= 2 && IS_TRUE(args[1]));
    ObjList *out = new_list(0);
    int i = 0, line_start = 0;
    while (i < s->length) {
        unsigned char c = (unsigned char)s->chars[i];
        int eol = 0, adv = 1;
        if (c == '\r') {
            eol = 1;
            if (i + 1 < s->length && s->chars[i + 1] == '\n') adv = 2;
        } else if (c == '\n' || c == '\v' || c == '\f' || c == 0x85 ||
                   (c >= 0x1C && c <= 0x1E)) {
            eol = 1;
        }
        if (eol) {
            int end = keepends ? i + adv : i;
            list_add(out, make_obj((Object *)new_string(s->chars + line_start,
                                                        end - line_start)));
            i += adv;
            line_start = i;
        } else {
            i++;
        }
    }
    if (line_start < s->length) {
        list_add(out, make_obj((Object *)new_string(
            s->chars + line_start, s->length - line_start)));
    }
    return make_obj((Object *)out);
}

static Value string_method_join(VM *vm, Value *args, int nargs) {
    ObjString *s = sm_self(vm, args);
    if (nargs < 2) return make_null(); /* bare access */
    Value *items = NULL;
    int count = 0;
    if (!sm_iter_items(args[1], &items, &count)) {
        luna_throw(vm, py_fe(vm)->type_error_class,
                   "join expects a list or tuple");
    }
    /* Validate types first so nothing is allocated on failure. */
    for (int i = 0; i < count; i++) {
        if (!IS_STRING(items[i])) {
            luna_throw(vm, py_fe(vm)->type_error_class,
                       "join items must be strings");
        }
    }
    long total = (count > 1) ? (long)(count - 1) * s->length : 0;
    for (int i = 0; i < count; i++)
        total += (long)((ObjString *)AS_OBJ(items[i]))->length;
    char *buf = malloc((size_t)total + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    int pos = 0;
    for (int i = 0; i < count; i++) {
        if (i > 0) { memcpy(buf + pos, s->chars, (size_t)s->length); pos += s->length; }
        ObjString *item = (ObjString *)AS_OBJ(items[i]);
        memcpy(buf + pos, item->chars, (size_t)item->length);
        pos += item->length;
    }
    buf[pos] = '\0';
    return sm_take(buf, pos);
}

/* Strip arg is optional; strip set = whitespace by default. */
static bool sm_strip_char(ObjString *set, int cp) {
    if (set) {
        int i = 0;
        while (i < set->length) {
            int c = sm_utf8_decode(set->chars, &i);
            if (c == cp) return true;
        }
        return false;
    }
    return sm_is_space(cp);
}

/* Shared strip/lstrip/rstrip body: mode < 0 left, > 0 right, 0 both. */
static Value string_do_strip(VM *vm, Value *args, int nargs, int mode) {
    ObjString *s = sm_self(vm, args);
    ObjString *set = sm_opt_str(vm, args, nargs, 1);
    int start = 0, end = s->length;
    if (mode <= 0) {
        while (start < end) {
            int i = start;
            int cp = sm_utf8_decode(s->chars, &i);
            if (!sm_strip_char(set, cp)) break;
            start = i;
        }
    }
    if (mode >= 0) {
        while (end > start) {
            /* walk back to the start of the last code point */
            int back = end - 1;
            while (back > start && ((unsigned char)s->chars[back] & 0xC0) == 0x80) back--;
            int i = back;
            int cp = sm_utf8_decode(s->chars, &i);
            if (!sm_strip_char(set, cp)) break;
            end = back;
        }
    }
    return make_obj((Object *)new_string(s->chars + start, end - start));
}

static Value string_method_strip(VM *vm, Value *args, int nargs) {
    return string_do_strip(vm, args, nargs, 0);
}

static Value string_method_lstrip(VM *vm, Value *args, int nargs) {
    return string_do_strip(vm, args, nargs, -1);
}

static Value string_method_rstrip(VM *vm, Value *args, int nargs) {
    return string_do_strip(vm, args, nargs, 1);
}

/* Shared center/ljust/rjust body: mode 0 center, -1 left, 1 right. */
static Value string_do_pad(VM *vm, Value *args, int nargs, int mode) {
    ObjString *s = sm_self(vm, args);
    if (nargs < 2) return make_null(); /* bare access */
    int width = sm_req_int(vm, args, nargs, 1);
    int fill_len;
    const char *fill = sm_fill(vm, args, nargs, 2, &fill_len);
    int n = utf8_code_point_count(s->chars, s->length);
    long pad = (long)width - n;
    if (pad <= 0) return args[0];
    long left = 0, right = 0;
    if (mode == 0) { left = pad / 2; right = pad - left; }
    else if (mode < 0) right = pad;
    else left = pad;
    char *buf = malloc((size_t)s->length + (size_t)(left + right) * (size_t)fill_len + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    int pos = 0;
    for (long i = 0; i < left; i++) {
        memcpy(buf + pos, fill, (size_t)fill_len);
        pos += fill_len;
    }
    memcpy(buf + pos, s->chars, (size_t)s->length);
    pos += s->length;
    for (long i = 0; i < right; i++) {
        memcpy(buf + pos, fill, (size_t)fill_len);
        pos += fill_len;
    }
    buf[pos] = '\0';
    return sm_take(buf, pos);
}

static Value string_method_center(VM *vm, Value *args, int nargs) {
    return string_do_pad(vm, args, nargs, 0);
}

static Value string_method_ljust(VM *vm, Value *args, int nargs) {
    return string_do_pad(vm, args, nargs, -1);
}

static Value string_method_rjust(VM *vm, Value *args, int nargs) {
    return string_do_pad(vm, args, nargs, 1);
}

/* zfill: like rjust with '0' but keeps a leading sign before the zeros. */
static Value string_method_zfill(VM *vm, Value *args, int nargs) {
    ObjString *s = sm_self(vm, args);
    if (nargs < 2) return make_null();
    int width = sm_req_int(vm, args, nargs, 1);
    int n = utf8_code_point_count(s->chars, s->length);
    long pad = (long)width - n;
    if (pad <= 0) return args[0];
    int sign_len = 0;
    if (s->length > 0 && (s->chars[0] == '+' || s->chars[0] == '-')) sign_len = 1;
    char *buf = malloc((size_t)s->length + (size_t)pad + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    int pos = 0;
    if (sign_len) { buf[pos++] = s->chars[0]; }
    memset(buf + pos, '0', (size_t)pad);
    pos += (int)pad;
    memcpy(buf + pos, s->chars + sign_len, (size_t)(s->length - sign_len));
    pos += s->length - sign_len;
    buf[pos] = '\0';
    return sm_take(buf, pos);
}

/* expandtabs: replace \t with spaces to the next tab stop (default 8). */
static Value string_method_expandtabs(VM *vm, Value *args, int nargs) {
    ObjString *s = sm_self(vm, args);
    long tabsize = 8;
    if (nargs >= 2 && !IS_NIL(args[1])) {
        int c = 0;
        if (!sm_opt_int(vm, args, nargs, 1, &c)) return make_null();
        tabsize = c;
    }
    char *buf = malloc((size_t)s->length + (size_t)s->length * 8 + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    int pos = 0, i = 0, col = 0;
    while (i < s->length) {
        int start = i;
        int cp = sm_utf8_decode(s->chars, &i);
        if (cp == '\t') {
            long spaces = (tabsize > 0) ? (tabsize - (col % tabsize)) : 0;
            memset(buf + pos, ' ', (size_t)spaces);
            pos += (int)spaces;
            col += (int)spaces;
        } else if (cp == '\n' || cp == '\r') {
            memcpy(buf + pos, s->chars + start, (size_t)(i - start));
            pos += i - start;
            col = 0;
        } else {
            memcpy(buf + pos, s->chars + start, (size_t)(i - start));
            pos += i - start;
            col++;
        }
    }
    buf[pos] = '\0';
    return sm_take(buf, pos);
}

static Value string_method_partition(VM *vm, Value *args, int nargs) {
    ObjString *s = sm_self(vm, args);
    if (nargs < 2) return make_null();
    ObjString *sep = sm_req_str(vm, args, nargs, 1);
    if (sep->length == 0) {
        luna_throw(vm, py_fe(vm)->value_error_class, "empty separator");
    }
    int *starts = NULL;
    int n = sm_cp_starts(s, &starts);
    int sep_n = utf8_code_point_count(sep->chars, sep->length);
    for (int k = 0; k + sep_n <= n; k++) {
        if (memcmp(s->chars + starts[k], sep->chars, (size_t)sep->length) == 0) {
            ObjString *head = (ObjString *)AS_OBJ(make_obj(
                (Object *)new_string(s->chars, starts[k])));
            ObjString *tail = (ObjString *)AS_OBJ(make_obj(
                (Object *)new_string(s->chars + starts[k + sep_n],
                                     s->length - starts[k + sep_n])));
            free(starts);
            return sm_partition3(head, sep, tail);
        }
    }
    free(starts);
    return sm_partition3(s, (ObjString *)AS_OBJ(make_obj((Object *)new_string("", 0))),
                         (ObjString *)AS_OBJ(make_obj((Object *)new_string("", 0))));
}

static Value string_method_rpartition(VM *vm, Value *args, int nargs) {
    ObjString *s = sm_self(vm, args);
    if (nargs < 2) return make_null();
    ObjString *sep = sm_req_str(vm, args, nargs, 1);
    if (sep->length == 0) {
        luna_throw(vm, py_fe(vm)->value_error_class, "empty separator");
    }
    int *starts = NULL;
    int n = sm_cp_starts(s, &starts);
    int sep_n = utf8_code_point_count(sep->chars, sep->length);
    for (int k = n - sep_n; k >= 0; k--) {
        if (memcmp(s->chars + starts[k], sep->chars, (size_t)sep->length) == 0) {
            ObjString *head = (ObjString *)AS_OBJ(make_obj(
                (Object *)new_string(s->chars, starts[k])));
            ObjString *tail = (ObjString *)AS_OBJ(make_obj(
                (Object *)new_string(s->chars + starts[k + sep_n],
                                     s->length - starts[k + sep_n])));
            free(starts);
            return sm_partition3(head, sep, tail);
        }
    }
    free(starts);
    /* CPython: (empty, empty, self) when the separator is absent. */
    ObjString *empty = (ObjString *)AS_OBJ(make_obj((Object *)new_string("", 0)));
    return sm_partition3(empty, empty, s);
}

static Value string_method_removeprefix(VM *vm, Value *args, int nargs) {
    ObjString *s = sm_self(vm, args);
    if (nargs < 2) return make_null();
    ObjString *pre = sm_req_str(vm, args, nargs, 1);
    if (pre->length > 0 && s->length >= pre->length &&
        memcmp(s->chars, pre->chars, (size_t)pre->length) == 0) {
        return make_obj((Object *)new_string(s->chars + pre->length,
                                             s->length - pre->length));
    }
    return args[0];
}

static Value string_method_removesuffix(VM *vm, Value *args, int nargs) {
    ObjString *s = sm_self(vm, args);
    if (nargs < 2) return make_null();
    ObjString *suf = sm_req_str(vm, args, nargs, 1);
    if (suf->length > 0 && s->length >= suf->length &&
        memcmp(s->chars + (s->length - suf->length), suf->chars,
               (size_t)suf->length) == 0) {
        return make_obj((Object *)new_string(s->chars, s->length - suf->length));
    }
    return args[0];
}

/* format: "{}" auto-numbered and "{0}" explicit positional fields only.
 * "{{" and "}}" are literal braces; "{name}" raises (no kwargs support). */
static Value string_method_format(VM *vm, Value *args, int nargs) {
    ObjString *s = sm_self(vm, args);
    long auto_n = 0;
    /* Pass 1: size the output. */
    long total = 0;
    for (int i = 0; i < s->length; i++) {
        char c = s->chars[i];
        if (c != '{' && c != '}') { total++; continue; }
        if (i + 1 < s->length && s->chars[i + 1] == c) { total++; i++; continue; }
        if (c == '}') {
            luna_throw(vm, py_fe(vm)->value_error_class,
                       "single '}' in format string");
        }
        /* Find the closing brace of this field. */
        int j = i + 1;
        while (j < s->length && s->chars[j] != '}') j++;
        if (j >= s->length) {
            luna_throw(vm, py_fe(vm)->value_error_class,
                       "unmatched '{' in format string");
        }
        int flen = j - i - 1;
        const char *f = s->chars + i + 1;
        long idx;
        if (flen == 0) {
            idx = auto_n++;
        } else {
            idx = 0;
            for (int p = 0; p < flen; p++) {
                if (f[p] < '0' || f[p] > '9') {
                    luna_throw(vm, py_fe(vm)->type_error_class,
                               "only {} and {number} fields are supported");
                }
                idx = idx * 10 + (f[p] - '0');
            }
        }
        if (idx + 2 > nargs) {
            luna_throw(vm, py_fe(vm)->index_error_class, "format field out of range");
        }
        char *rendered = value_to_string(args[idx + 1]);
        total += (long)strlen(rendered);
        free(rendered);
        i = j;
    }
    /* Pass 2: render. */
    char *buf = malloc((size_t)total + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    int pos = 0;
    auto_n = 0;
    for (int i = 0; i < s->length; i++) {
        char c = s->chars[i];
        if (c != '{' && c != '}') { buf[pos++] = c; continue; }
        if (i + 1 < s->length && s->chars[i + 1] == c) { buf[pos++] = c; i++; continue; }
        if (c == '}') continue; /* pass 1 already rejected this */
        int j = i + 1;
        while (j < s->length && s->chars[j] != '}') j++;
        int flen = j - i - 1;
        const char *f = s->chars + i + 1;
        long idx;
        if (flen == 0) idx = auto_n++;
        else {
            idx = 0;
            for (int p = 0; p < flen; p++) idx = idx * 10 + (f[p] - '0');
        }
        char *rendered = value_to_string(args[idx + 1]);
        size_t rlen = strlen(rendered);
        memcpy(buf + pos, rendered, rlen);
        pos += (int)rlen;
        free(rendered);
        i = j;
    }
    buf[pos] = '\0';
    return sm_take(buf, pos);
}

/* ============================================================ */
/* Registration                                                  */
/* ============================================================ */

void vm_register_string_methods(VM *vm) {
    ObjClass *str_class = py_fe(vm)->string_class;
    class_add_native_method(str_class, "isalpha", string_method_isalpha);
    class_add_native_method(str_class, "isdigit", string_method_isdigit);
    class_add_native_method(str_class, "isalnum", string_method_isalnum);
    class_add_native_method(str_class, "isspace", string_method_isspace);
    class_add_native_method(str_class, "isprintable", string_method_isprintable);
    class_add_native_method(str_class, "isascii", string_method_isascii);
    class_add_native_method(str_class, "isidentifier", string_method_isidentifier);
    class_add_native_method(str_class, "isupper", string_method_isupper);
    class_add_native_method(str_class, "islower", string_method_islower);
    class_add_native_method(str_class, "istitle", string_method_istitle);
    class_add_native_method(str_class, "capitalize", string_method_capitalize);
    class_add_native_method(str_class, "title", string_method_title);
    class_add_native_method(str_class, "swapcase", string_method_swapcase);
    class_add_native_method(str_class, "find", string_method_find);
    class_add_native_method(str_class, "rfind", string_method_rfind);
    class_add_native_method(str_class, "index", string_method_index);
    class_add_native_method(str_class, "rindex", string_method_rindex);
    class_add_native_method(str_class, "count", string_method_count);
    class_add_native_method(str_class, "startswith", string_method_startswith);
    class_add_native_method(str_class, "endswith", string_method_endswith);
    class_add_native_method(str_class, "replace", string_method_replace);
    class_add_native_method(str_class, "split", string_method_split);
    class_add_native_method(str_class, "rsplit", string_method_rsplit);
    class_add_native_method(str_class, "splitlines", string_method_splitlines);
    class_add_native_method(str_class, "join", string_method_join);
    class_add_native_method(str_class, "strip", string_method_strip);
    class_add_native_method(str_class, "lstrip", string_method_lstrip);
    class_add_native_method(str_class, "rstrip", string_method_rstrip);
    class_add_native_method(str_class, "center", string_method_center);
    class_add_native_method(str_class, "ljust", string_method_ljust);
    class_add_native_method(str_class, "rjust", string_method_rjust);
    class_add_native_method(str_class, "zfill", string_method_zfill);
    class_add_native_method(str_class, "expandtabs", string_method_expandtabs);
    class_add_native_method(str_class, "partition", string_method_partition);
    class_add_native_method(str_class, "rpartition", string_method_rpartition);
    class_add_native_method(str_class, "removeprefix", string_method_removeprefix);
    class_add_native_method(str_class, "removesuffix", string_method_removesuffix);
    class_add_native_method(str_class, "format", string_method_format);
}
