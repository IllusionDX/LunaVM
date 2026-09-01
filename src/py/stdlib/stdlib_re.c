/* stdlib_re.c — Python 're' module.
 * Pattern pointers live in a C-side cache, never in ObjInstance.
 * Match data lives entirely as standard VM fields.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vm.h"
#include "value.h"
#include "py/object.h"
#include "py/frontend_state.h"
#include "common/regex/regex.h"

/* ==================================================================
 * Pattern cache — C-side, keyed by (source, flags)
 * ================================================================== */

#define RE_CACHE_SIZE 64

typedef struct {
    char *source;       /* heap-owned copy of the pattern source */
    int  flags;
    RE_Pattern *compiled;
    int  lru;           /* monotonically increasing timestamp */
} RegexCacheEntry;

static RegexCacheEntry re_cache[RE_CACHE_SIZE];
static int re_cache_clock = 0;

static RE_Pattern *cache_lookup(const char *src, int flags) {
    for (int i = 0; i < RE_CACHE_SIZE; i++) {
        if (re_cache[i].compiled &&
            re_cache[i].flags == flags &&
            strcmp(re_cache[i].source, src) == 0) {
            re_cache[i].lru = ++re_cache_clock;
            return re_cache[i].compiled;
        }
    }
    return NULL;
}

static RE_Pattern *cache_insert(const char *src, int flags, RE_Pattern *pat) {
    /* find empty slot or evict LRU */
    int victim = -1;
    int min_lru = INT32_MAX;
    for (int i = 0; i < RE_CACHE_SIZE; i++) {
        if (!re_cache[i].compiled) { victim = i; break; }
        if (re_cache[i].lru < min_lru) { min_lru = re_cache[i].lru; victim = i; }
    }
    if (re_cache[victim].compiled) {
        re_free_pattern(re_cache[victim].compiled);
        free(re_cache[victim].source);
    }
    re_cache[victim].source = strdup(src);
    if (!re_cache[victim].source) { re_free_pattern(pat); return NULL; }
    re_cache[victim].flags = flags;
    re_cache[victim].compiled = pat;
    re_cache[victim].lru = ++re_cache_clock;
    return pat;
}

static RE_Pattern *get_or_compile(const char *src, int flags, char *err, int err_len) {
    RE_Pattern *pat = cache_lookup(src, flags);
    if (pat) return pat;
    pat = re_compile(src, flags, err, err_len);
    if (!pat) return NULL;
    return cache_insert(src, flags, pat);
}

/* ==================================================================
 * Canonical classes
 * ================================================================== */

static ObjClass *re_pattern_class = NULL;
static ObjClass *re_match_class = NULL;

/* ==================================================================
 * Helpers
 * ================================================================== */

static const char *get_cstr(Value v) {
    return IS_STRING(v) ? ((ObjString *)AS_OBJ(v))->chars : "";
}

static int get_strlen(Value v) {
    return IS_STRING(v) ? ((ObjString *)AS_OBJ(v))->length : 0;
}

static int get_flags(Value v, int idx, int n) {
    return (idx < n && IS_INT(v)) ? AS_INT(v) : 0;
}

/* ==================================================================
 * Pattern instance: fields are _source, _flags, _group_count
 * All standard VM values, no C pointers.
 * ================================================================== */

/* Extract RE_Pattern* from cache using the instance fields.
 * Caller must NOT free the result (it lives in the cache). */
static RE_Pattern *pattern_from_instance(ObjInstance *inst, char *err, int err_len) {
    Value src = instance_get_field(inst, "_source");
    Value flg = instance_get_field(inst, "_flags");
    if (!IS_STRING(src)) return NULL;
    int flags = IS_INT(flg) ? AS_INT(flg) : 0;
    return get_or_compile(((ObjString *)AS_OBJ(src))->chars, flags, err, err_len);
}

/* Compile or extract pattern from either a Pattern instance or a raw string. */
static RE_Pattern *resolve_pattern(VM *vm, Value v, int flags, char *err, int err_len) {
    (void)vm;
    if (IS_OBJ(v) && AS_OBJ(v) && ((ObjInstance *)AS_OBJ(v))->klass == re_pattern_class)
        return pattern_from_instance((ObjInstance *)AS_OBJ(v), err, err_len);
    if (!IS_STRING(v)) {
        snprintf(err, err_len, "expected str or Pattern");
        return NULL;
    }
    return get_or_compile(get_cstr(v), flags, err, err_len);
}

/* ==================================================================
 * Match instance: fields are _string, _start, _end, _group_count,
 *                 _spans (ObjList of ints: [s0,e0, s1,e1, ...])
 * ================================================================== */

static ObjInstance *make_match_obj(const RE_Match *m, const char *str, int str_len) {
    ObjInstance *inst = new_instance(re_match_class, 5);

    instance_set_field(inst, "_string",
        make_obj((Object *)new_string(str, str_len)));
    instance_set_field(inst, "_start", make_int(m->start));
    instance_set_field(inst, "_end", make_int(m->end));
    instance_set_field(inst, "_group_count", make_int(m->group_count));

    /* _spans: flat list [start0, end0, start1, end1, ...]; -1 = unmatched */
    ObjList *spans = new_list(m->group_count * 2);
    for (int i = 0; i < m->group_count; i++) {
        list_add(spans, make_int(m->groups[i].start));
        list_add(spans, make_int(m->groups[i].end));
    }
    instance_set_field(inst, "_spans", make_obj((Object *)spans));

    return inst;
}

/* ==================================================================
 * Native functions: re.match, re.search, re.fullmatch, etc.
 * ================================================================== */

/* re.compile(pattern, flags=0) -> Pattern */
static Value re_compile_fn(VM *vm, Value *args, int n) {
    if (n < 1) { luna_throw(vm, py_fe(vm)->argument_error_class, "compile() requires pattern"); return make_null(); }
    const char *pat_str = get_cstr(args[0]);
    int flags = get_flags(args[1], 1, n);

    char err_buf[256];
    RE_Pattern *pat = get_or_compile(pat_str, flags, err_buf, sizeof(err_buf));
    if (!pat) {
        luna_throw(vm, py_fe(vm)->runtime_error_class, "re error: %s", err_buf);
        return make_null();
    }

    ObjInstance *inst = new_instance(re_pattern_class, 3);
    instance_set_field(inst, "_source", args[0]);
    instance_set_field(inst, "_flags", (n >= 2) ? args[1] : make_int(0));
    instance_set_field(inst, "_group_count", make_int(pat->group_count));
    return make_obj((Object *)inst);
}

/* re.match(pattern, string, flags=0) -> Match|None */
static Value re_match_fn(VM *vm, Value *args, int n) {
    if (n < 2) { luna_throw(vm, py_fe(vm)->argument_error_class, "match() requires 2 args"); return make_null(); }
    char err_buf[256];
    RE_Pattern *pat = resolve_pattern(vm, args[0], get_flags(args[2], 2, n), err_buf, sizeof(err_buf));
    if (!pat) { luna_throw(vm, py_fe(vm)->runtime_error_class, "re error: %s", err_buf); return make_null(); }

    const char *str = get_cstr(args[1]);
    int len = get_strlen(args[1]);
    RE_Match m;
    if (!re_match(pat, str, len, &m)) return make_null();
    return make_obj((Object *)make_match_obj(&m, str, len));
}

/* re.search(pattern, string, flags=0) -> Match|None */
static Value re_search_fn(VM *vm, Value *args, int n) {
    if (n < 2) { luna_throw(vm, py_fe(vm)->argument_error_class, "search() requires 2 args"); return make_null(); }
    char err_buf[256];
    RE_Pattern *pat = resolve_pattern(vm, args[0], get_flags(args[2], 2, n), err_buf, sizeof(err_buf));
    if (!pat) { luna_throw(vm, py_fe(vm)->runtime_error_class, "re error: %s", err_buf); return make_null(); }

    const char *str = get_cstr(args[1]);
    int len = get_strlen(args[1]);
    RE_Match m;
    if (!re_search(pat, str, len, &m)) return make_null();
    return make_obj((Object *)make_match_obj(&m, str, len));
}

/* re.fullmatch(pattern, string, flags=0) -> Match|None */
static Value re_fullmatch_fn(VM *vm, Value *args, int n) {
    if (n < 2) { luna_throw(vm, py_fe(vm)->argument_error_class, "fullmatch() requires 2 args"); return make_null(); }
    char err_buf[256];
    RE_Pattern *pat = resolve_pattern(vm, args[0], get_flags(args[2], 2, n), err_buf, sizeof(err_buf));
    if (!pat) { luna_throw(vm, py_fe(vm)->runtime_error_class, "re error: %s", err_buf); return make_null(); }

    const char *str = get_cstr(args[1]);
    int len = get_strlen(args[1]);
    RE_Match m;
    if (!re_match(pat, str, len, &m)) return make_null();
    if (m.start != 0 || m.end != len) return make_null();
    return make_obj((Object *)make_match_obj(&m, str, len));
}

/* re.findall(pattern, string, flags=0) -> list
 * Python semantics: no groups -> whole matches; 1 group -> that group;
 * 2+ groups -> tuple of groups per match. */
typedef struct { ObjList *list; VM *vm; const char *str; int ngroups; } FA_Ctx;

static void findall_cb(const RE_Match *m, const char *str, void *ud) {
    FA_Ctx *ctx = (FA_Ctx *)ud;
    if (ctx->ngroups == 0) {
        list_add(ctx->list, make_obj((Object *)new_string(str + m->start, m->end - m->start)));
    } else if (ctx->ngroups == 1) {
        if (m->groups[0].start >= 0 && m->groups[0].end >= 0)
            list_add(ctx->list, make_obj((Object *)new_string(
                str + m->groups[0].start, m->groups[0].end - m->groups[0].start)));
        else
            list_add(ctx->list, make_null());
    } else {
        ObjTuple *t = new_tuple(ctx->ngroups);
        for (int i = 0; i < ctx->ngroups; i++) {
            if (m->groups[i].start >= 0 && m->groups[i].end >= 0)
                t->items[i] = make_obj((Object *)new_string(
                    str + m->groups[i].start, m->groups[i].end - m->groups[i].start));
            else
                t->items[i] = make_null();
        }
        list_add(ctx->list, make_obj((Object *)t));
    }
}

static Value re_findall_fn(VM *vm, Value *args, int n) {
    if (n < 2) { luna_throw(vm, py_fe(vm)->argument_error_class, "findall() requires 2 args"); return make_null(); }
    char err_buf[256];
    RE_Pattern *pat = resolve_pattern(vm, args[0], get_flags(args[2], 2, n), err_buf, sizeof(err_buf));
    if (!pat) { luna_throw(vm, py_fe(vm)->runtime_error_class, "re error: %s", err_buf); return make_null(); }

    const char *str = get_cstr(args[1]);
    int len = get_strlen(args[1]);
    ObjList *result = new_list(8);
    FA_Ctx ctx = { .list = result, .vm = vm, .str = str, .ngroups = pat->group_count };
    re_findall(pat, str, len, findall_cb, &ctx);
    return make_obj((Object *)result);
}

/* re.sub(pattern, repl, string, count=0, flags=0) -> string */
static Value re_sub_fn(VM *vm, Value *args, int n) {
    if (n < 3) { luna_throw(vm, py_fe(vm)->argument_error_class, "sub() requires 3 args"); return make_null(); }
    char err_buf[256];
    RE_Pattern *pat = resolve_pattern(vm, args[0], get_flags(args[4], 4, n), err_buf, sizeof(err_buf));
    if (!pat) { luna_throw(vm, py_fe(vm)->runtime_error_class, "re error: %s", err_buf); return make_null(); }

    const char *repl = get_cstr(args[1]);
    const char *str = get_cstr(args[2]);
    int len = get_strlen(args[2]);
    int max_count = (n >= 4 && IS_INT(args[3])) ? AS_INT(args[3]) : 0;
    int rlen = (int)strlen(repl);

    int cap = len + 64;
    char *buf = malloc(cap);
    int bpos = 0;
    int pos = 0;
    int count = 0;
    RE_Match m;

    while (pos <= len && (max_count == 0 || count < max_count)) {
        if (!re_search(pat, str + pos, len - pos, &m)) break;
        int pre = m.start;
        int mlen = m.end - m.start;
        while (bpos + pre + rlen >= cap) { cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + bpos, str + pos, pre); bpos += pre;
        memcpy(buf + bpos, repl, rlen); bpos += rlen;
        pos += m.end;
        count++;
        if (mlen == 0) pos++;   /* avoid infinite loop on zero-width match */
    }
    if (pos < len) {
        while (bpos + (len - pos) >= cap) { cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + bpos, str + pos, len - pos); bpos += len - pos;
    }

    ObjString *result = new_string(buf, bpos);
    free(buf);
    return make_obj((Object *)result);
}

/* re.split(pattern, string, maxsplit=0, flags=0) -> list */
static Value re_split_fn(VM *vm, Value *args, int n) {
    if (n < 2) { luna_throw(vm, py_fe(vm)->argument_error_class, "split() requires 2 args"); return make_null(); }
    char err_buf[256];
    RE_Pattern *pat = resolve_pattern(vm, args[0], get_flags(args[3], 3, n), err_buf, sizeof(err_buf));
    if (!pat) { luna_throw(vm, py_fe(vm)->runtime_error_class, "re error: %s", err_buf); return make_null(); }

    const char *str = get_cstr(args[1]);
    int len = get_strlen(args[1]);
    int maxsplit = (n >= 3 && IS_INT(args[2])) ? AS_INT(args[2]) : 0;

    ObjList *result = new_list(8);
    int pos = 0;
    int sc = 0;
    RE_Match m;

    while (pos <= len) {
        if (maxsplit > 0 && sc >= maxsplit) break;
        if (re_search(pat, str + pos, len - pos, &m)) {
            list_add(result, make_obj((Object *)new_string(str + pos, m.start)));
            pos += m.end;
            sc++;
            if (m.end == m.start) pos++;   /* zero-width separator */
        } else {
            break;
        }
    }
    list_add(result, make_obj((Object *)new_string(str + pos, len - pos)));
    return make_obj((Object *)result);
}

/* re.escape(string) -> string */
static Value re_escape_fn(VM *vm, Value *args, int n) {
    if (n < 1) { luna_throw(vm, py_fe(vm)->argument_error_class, "escape() requires 1 arg"); return make_null(); }
    if (!IS_STRING(args[0])) { luna_throw(vm, py_fe(vm)->type_error_class, "escape() arg must be str"); return make_null(); }
    const char *s = ((ObjString *)AS_OBJ(args[0]))->chars;
    int len = ((ObjString *)AS_OBJ(args[0]))->length;
    int cap = len * 2 + 1;
    char *buf = malloc(cap);
    int j = 0;
    for (int i = 0; i < len; i++) {
        char ch = s[i];
        if (ch == '\\' || ch == '.' || ch == '*' || ch == '+' || ch == '?' ||
            ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == '{' ||
            ch == '}' || ch == '|' || ch == '^' || ch == '$' || ch == '/')
            buf[j++] = '\\';
        buf[j++] = ch;
    }
    buf[j] = '\0';
    ObjString *result = new_string(buf, j);
    free(buf);
    return make_obj((Object *)result);
}

/* ==================================================================
 * Match methods: group(i=0), start(i=0), end(i=0), groups(), span(i=0)
 * ================================================================== */

static ObjInstance *match_self(VM *vm, Value v) {
    if (!IS_OBJ(v) || !AS_OBJ(v) || ((ObjInstance *)AS_OBJ(v))->klass != re_match_class) {
        luna_throw(vm, py_fe(vm)->type_error_class, "expected Match self");
        return NULL;
    }
    return (ObjInstance *)AS_OBJ(v);
}

/* Group span: idx 0 = whole match; idx k (1-based) = spans[2k-2], spans[2k-1].
 * Returns false when idx is out of range. */
static int match_get_span(VM *vm, ObjInstance *inst, int idx, int *out_s, int *out_e) {
    (void)vm;
    Value gc_v = instance_get_field(inst, "_group_count");
    int gc = IS_INT(gc_v) ? AS_INT(gc_v) : 0;
    if (idx < 0 || idx > gc) return 0;
    if (idx == 0) {
        Value s = instance_get_field(inst, "_start");
        Value e = instance_get_field(inst, "_end");
        *out_s = IS_INT(s) ? AS_INT(s) : -1;
        *out_e = IS_INT(e) ? AS_INT(e) : -1;
        return 1;
    }
    Value spans_v = instance_get_field(inst, "_spans");
    if (!IS_LIST(spans_v)) return 0;
    ObjList *spans = (ObjList *)AS_OBJ(spans_v);
    Value s = list_get(spans, (idx - 1) * 2);
    Value e = list_get(spans, (idx - 1) * 2 + 1);
    *out_s = IS_INT(s) ? AS_INT(s) : -1;
    *out_e = IS_INT(e) ? AS_INT(e) : -1;
    return 1;
}

static Value match_group_fn(VM *vm, Value *args, int n) {
    ObjInstance *inst = match_self(vm, args[0]);
    if (!inst) return make_null();
    int idx = (n >= 2 && IS_INT(args[1])) ? AS_INT(args[1]) : 0;
    int s, e;
    if (!match_get_span(vm, inst, idx, &s, &e)) {
        luna_throw(vm, py_fe(vm)->index_error_class, "no such group");
        return make_null();
    }
    if (s < 0 || e < 0) return make_null();
    Value str_v = instance_get_field(inst, "_string");
    const char *str = get_cstr(str_v);
    return make_obj((Object *)new_string(str + s, e - s));
}

static Value match_start_fn(VM *vm, Value *args, int n) {
    ObjInstance *inst = match_self(vm, args[0]);
    if (!inst) return make_null();
    int idx = (n >= 2 && IS_INT(args[1])) ? AS_INT(args[1]) : 0;
    int s, e;
    if (!match_get_span(vm, inst, idx, &s, &e)) {
        luna_throw(vm, py_fe(vm)->index_error_class, "no such group");
        return make_null();
    }
    return make_int(s);
}

static Value match_end_fn(VM *vm, Value *args, int n) {
    ObjInstance *inst = match_self(vm, args[0]);
    if (!inst) return make_null();
    int idx = (n >= 2 && IS_INT(args[1])) ? AS_INT(args[1]) : 0;
    int s, e;
    if (!match_get_span(vm, inst, idx, &s, &e)) {
        luna_throw(vm, py_fe(vm)->index_error_class, "no such group");
        return make_null();
    }
    return make_int(e);
}

static Value match_span_fn(VM *vm, Value *args, int n) {
    ObjInstance *inst = match_self(vm, args[0]);
    if (!inst) return make_null();
    int idx = (n >= 2 && IS_INT(args[1])) ? AS_INT(args[1]) : 0;
    int s, e;
    if (!match_get_span(vm, inst, idx, &s, &e)) {
        luna_throw(vm, py_fe(vm)->index_error_class, "no such group");
        return make_null();
    }
    ObjTuple *t = new_tuple(2);
    t->items[0] = make_int(s);
    t->items[1] = make_int(e);
    return make_obj((Object *)t);
}

static Value match_groups_fn(VM *vm, Value *args, int n) {
    (void)args; (void)n;
    ObjInstance *inst = match_self(vm, args[0]);
    if (!inst) return make_null();
    Value gc_v = instance_get_field(inst, "_group_count");
    int gc = IS_INT(gc_v) ? AS_INT(gc_v) : 0;
    ObjTuple *t = new_tuple(gc);
    for (int i = 1; i <= gc; i++) {
        int s, e;
        if (match_get_span(vm, inst, i, &s, &e) && s >= 0 && e >= 0) {
            Value str_v = instance_get_field(inst, "_string");
            const char *str = get_cstr(str_v);
            t->items[i - 1] = make_obj((Object *)new_string(str + s, e - s));
        } else {
            t->items[i - 1] = make_null();
        }
    }
    return make_obj((Object *)t);
}

/* ==================================================================
 * Module registration
 * ================================================================== */

extern void class_add_native_method(void *cls, const char *name, NativeFn fn);

void vm_register_re_module(VM *vm) {
    re_pattern_class = new_class("Pattern", NULL);
    re_match_class = new_class("Match", NULL);

    /* Match methods */
    class_add_native_method(re_match_class, "group",  match_group_fn);
    class_add_native_method(re_match_class, "start",  match_start_fn);
    class_add_native_method(re_match_class, "end",    match_end_fn);
    class_add_native_method(re_match_class, "span",   match_span_fn);
    class_add_native_method(re_match_class, "groups", match_groups_fn);

    /* Pattern methods — same native fns as module functions: args[0] is the
     * Pattern instance in both call shapes, so argument layouts coincide. */
    class_add_native_method(re_pattern_class, "match",     re_match_fn);
    class_add_native_method(re_pattern_class, "search",    re_search_fn);
    class_add_native_method(re_pattern_class, "fullmatch", re_fullmatch_fn);
    class_add_native_method(re_pattern_class, "findall",   re_findall_fn);
    class_add_native_method(re_pattern_class, "sub",       re_sub_fn);
    class_add_native_method(re_pattern_class, "split",     re_split_fn);

    ObjModule *mod = new_module("re");

    #define REG(name, fn) do { \
        ObjFunction *f = new_native_function(name, fn); \
        dict_set(mod->exports, make_obj((Object*)new_string(name, (int)strlen(name))), make_obj((Object*)f)); \
    } while(0)

    REG("compile",   re_compile_fn);
    REG("match",     re_match_fn);
    REG("search",    re_search_fn);
    REG("fullmatch", re_fullmatch_fn);
    REG("findall",   re_findall_fn);
    REG("sub",       re_sub_fn);
    REG("split",     re_split_fn);
    REG("escape",    re_escape_fn);
    #undef REG

    /* Flag constants */
    dict_set(mod->exports, make_obj((Object*)new_string("I", 1)), make_int(2));
    dict_set(mod->exports, make_obj((Object*)new_string("M", 1)), make_int(8));
    dict_set(mod->exports, make_obj((Object*)new_string("S", 1)), make_int(16));

    dict_set(py_fe(vm)->module_cache,
             make_obj((Object *)new_string("re", 2)),
             make_obj((Object *)mod));
}
