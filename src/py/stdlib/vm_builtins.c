/* vm_builtins.c — VM global table, native functions, collection method dispatch.
 * Included as a compilation unit alongside vm.c.
 *
 * FFI CONTRACT: Any builtin returning a double MUST use make_double() to
 * sanitize hardware NaNs. Raw NaNs can collide with object type signatures.
 * Use RETURN_EXT_DOUBLE() for external C function results.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "vm.h"
#include "value.h"
#include "py/object.h"
#include "py/frontend_state.h"
#include "py/range.h"
#include "py/slice.h"
#include "chunk.h"

/* True for int32 immediates and heap bigints; excludes doubles. */
static inline bool is_integer_value(Value v) { return IS_INT(v) || IS_BIGINT(v); }

/* ============================================================ */
/* Global variable table                                         */
/* ============================================================ */

static uint32_t hash_cstr(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

void vm_set_global(VM *vm, const char *name, Value value, bool is_const) {
    uint32_t h = hash_cstr(name);
    uint32_t bucket = h & (VM_GLOBAL_BUCKETS - 1);
    for (GlobalEntry *e = vm->globals[bucket]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            if (e->is_const) { fprintf(stderr, "vm: cannot reassign const '%s'\n", name); return; }
            e->value = value;
            /* invalidate possible cache entry */
            int ic_idx = h & (IC_CACHE_SIZE - 1);
            if (vm->global_ic[ic_idx].entry == e) vm->global_ic[ic_idx].key = NULL;
            return;
        }
    }
    GlobalEntry *ne = malloc(sizeof(GlobalEntry));
    ne->name     = strdup(name);
    ne->value    = value;
    ne->is_const = is_const;
    ne->next     = vm->globals[bucket];
    vm->globals[bucket] = ne;
}

bool vm_get_global(VM *vm, const char *name, Value *out) {
    uint32_t bucket = hash_cstr(name) & (VM_GLOBAL_BUCKETS - 1);
    for (GlobalEntry *e = vm->globals[bucket]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) { *out = e->value; return true; }
    }
    /* fall back to system globals */
    for (GlobalEntry *e = vm->system_globals[bucket]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) { *out = e->value; return true; }
    }
    return false;
}

bool vm_get_global_fast(VM *vm, Value name, Value *out) {
    ObjString *s = (ObjString*)AS_OBJ(name);
    uint32_t h = s->hash;
    int idx = h & (IC_CACHE_SIZE - 1);
    IC_GlobalEntry *ic = &vm->global_ic[idx];
    if (ic->key == (void*)s) {
        *out = ic->entry->value;
        return true;
    }
    uint32_t bucket = h & (VM_GLOBAL_BUCKETS - 1);
    for (GlobalEntry *e = vm->globals[bucket]; e; e = e->next) {
        if (e->name == s->chars || strcmp(e->name, s->chars) == 0) {
            *out = e->value;
            ic->key = (void*)s;
            ic->entry = e;
            return true;
        }
    }
    /* fall back to system globals (no inline cache for system globals) */
    for (GlobalEntry *e = vm->system_globals[bucket]; e; e = e->next) {
        if (e->name == s->chars || strcmp(e->name, s->chars) == 0) {
            *out = e->value;
            return true;
        }
    }
    return false;
}

GlobalEntry *vm_resolve_global(VM *vm, const char *name) {
    uint32_t bucket = hash_cstr(name) & (VM_GLOBAL_BUCKETS - 1);
    for (GlobalEntry *e = vm->globals[bucket]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

void vm_set_system_global(VM *vm, const char *name, Value value) {
    uint32_t h = hash_cstr(name);
    uint32_t bucket = h & (VM_GLOBAL_BUCKETS - 1);
    for (GlobalEntry *e = vm->system_globals[bucket]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            e->value = value;
            return;
        }
    }
    GlobalEntry *ne = malloc(sizeof(GlobalEntry));
    ne->name     = strdup(name);
    ne->value    = value;
    ne->is_const = false;
    ne->next     = vm->system_globals[bucket];
    vm->system_globals[bucket] = ne;
}

void vm_define_native(VM *vm, const char *name, NativeFn fn) {
    ObjFunction *f = new_native_function(name, fn);
    Value v = make_obj((Object *)f);
    vm_set_global(vm, name, v, true);
}

/* ============================================================ */
/* Built-in native functions                                     */
/* ============================================================ */

static Value bn_print(VM *vm, Value *args, int n) {
    (void)vm;
    for (int i = 0; i < n; i++) {
        char *s = value_to_string(args[i]); printf("%s", s); free(s);
        if (i < n-1) printf(" ");
    }
    printf("\n"); fflush(stdout); return make_null();
}

static Value bn_input(VM *vm, Value *args, int n) {
    (void)vm;
    if (n > 0) { char *p = value_to_string(args[0]); printf("%s", p); free(p); }
    char buf[1024];
    if (fgets(buf, sizeof(buf), stdin)) {
        size_t l = strlen(buf);
        if (l && buf[l-1]=='\n') buf[--l]='\0';
        return make_obj((Object *)new_string(buf,(int)l));
    }
    return make_null();
}

static Value bn_range(VM *vm, Value *args, int n) {
    /* CPython semantics: bounds/step must be integers (int32 or bigint);
     * floats raise TypeError; step zero raises ValueError. Construction is
     * O(1): nothing is materialized. */
    if (n < 1) {
        luna_throw(vm, py_fe(vm)->type_error_class, "range expected at least 1 argument, got 0");
        return make_null();
    }
    /* Reject non-integer args (e.g. floats) with a CPython-style message. */
    bool args_ok = false;
    if (n == 1) {
        args_ok = is_integer_value(args[0]);
    } else if (n >= 2) {
        args_ok = is_integer_value(args[0]) && is_integer_value(args[1]);
        if (n >= 3) args_ok = args_ok && is_integer_value(args[2]);
    }
    if (!args_ok) {
        luna_throw(vm, py_fe(vm)->type_error_class, "'float' object cannot be interpreted as an integer");
        return make_null();
    }

    Value vstart = make_int(0), vstop = make_int(0), vstep = make_int(1);
    if (n == 1) {
        vstop = args[0];
    } else {
        vstart = args[0]; vstop = args[1];
        if (n >= 3) vstep = args[2];
    }

    /* step must be non-zero. */
    bool step_zero = (IS_BIGINT(vstep) && ((ObjBigInt *)AS_OBJ(vstep))->sign == 0)
                  || (IS_INT(vstep) && AS_INT(vstep) == 0);
    if (step_zero) {
        luna_throw(vm, py_fe(vm)->value_error_class, "range() arg 3 must not be zero");
        return make_null();
    }

    return make_obj((Object *)new_range(vstart, vstop, vstep));
}

static Value bn_build_tuple(VM *vm, Value *args, int n) {
    (void)vm;
    ObjTuple *t = new_tuple(n);
    for (int i = 0; i < n; i++) t->items[i] = args[i];
    return make_obj((Object *)t);
}

static Value bn_isinstance(VM *vm, Value *args, int n) {
    (void)vm;
    if (n < 2) return make_bool(false);
    Value obj = args[0];
    Value cls = args[1];
    if (IS_TUPLE(cls)) {
        ObjTuple *t = (ObjTuple *)AS_OBJ(cls);
        for (int i = 0; i < t->count; i++) {
            Value item = t->items[i];
            if (IS_OBJ(item) && AS_OBJ(item) && AS_OBJ(item)->type->kind == OBJ_CLASS) {
                bool r = false;
                if (vm_instance_of(vm, obj, item, &r) && r) return make_bool(true);
            }
        }
        return make_bool(false);
    }
    if (IS_OBJ(cls) && AS_OBJ(cls) && AS_OBJ(cls)->type->kind == OBJ_CLASS) {
        bool r = false;
        vm_instance_of(vm, obj, cls, &r);
        return make_bool(r);
    }
    return make_bool(false);
}

static Value bn_repr(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_obj((Object *)new_string("", 0));
    if (IS_STRING(args[0])) {
        ObjString *s = (ObjString *)AS_OBJ(args[0]);
        char *out = malloc((size_t)s->length * 4 + 3);
        if (!out) { fprintf(stderr, "OOM\n"); exit(1); }
        int pos = 0;
        out[pos++] = '\'';
        for (int i = 0; i < s->length; i++) {
            char c = s->chars[i];
            switch (c) {
                case '\'': out[pos++] = '\\'; out[pos++] = '\''; break;
                case '\\': out[pos++] = '\\'; out[pos++] = '\\'; break;
                case '\n': out[pos++] = '\\'; out[pos++] = 'n'; break;
                case '\t': out[pos++] = '\\'; out[pos++] = 't'; break;
                case '\r': out[pos++] = '\\'; out[pos++] = 'r'; break;
                default: out[pos++] = c; break;
            }
        }
        out[pos++] = '\'';
        out[pos] = '\0';
        Value v = make_obj((Object *)new_string(out, pos));
        free(out);
        return v;
    }
    char *s = value_to_string(args[0]);
    Value v = make_obj((Object *)new_string(s, (int)strlen(s)));
    free(s);
    return v;
}

static Value bn_len(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n || !IS_OBJ(args[0])) return make_int(0);
    switch (AS_OBJ(args[0])->type->kind) {
        case OBJ_STRING: {
            ObjString *s = (ObjString*)AS_OBJ(args[0]);
            return make_int(utf8_code_point_count(s->chars, s->length));
        }
        case OBJ_LIST:    return make_int(((ObjList*)  AS_OBJ(args[0]))->count);
        case OBJ_TUPLE:   return make_int(((ObjTuple*) AS_OBJ(args[0]))->count);
        case OBJ_DICT:    return make_int(((ObjDict*)  AS_OBJ(args[0]))->entry_count);
        case OBJ_RANGE:   return range_length_value((ObjRange *)AS_OBJ(args[0]));
        default:          return make_int(0);
    }
}

static Value bn_type(VM *vm, Value *args, int n) {
    (void)vm;
    const char *t = "null";
    if (n) {
        if (IS_NIL(args[0])) t = "null";
        else if (IS_BOOL(args[0])) t = "bool";
        else if (IS_INT(args[0])) t = "int";
        else if (IS_BIGINT(args[0])) t = "int";
        else if (IS_POS_INF(args[0])) t = "inf";
        else if (IS_NEG_INF(args[0])) t = "-inf";
        else if (IS_NAN(args[0])) t = "nan";
        else if (IS_DOUBLE(args[0])) t = "float";
        else if (IS_OBJ(args[0])) {
            switch (AS_OBJ(args[0])->type->kind) {
                case OBJ_STRING:     t = "string";     break;
                case OBJ_LIST:       t = "list";       break;
                case OBJ_DICT:       t = "dict";       break;
                case OBJ_INSTANCE: {
                    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
                    if (inst->klass) {
                        return make_obj((Object*)inst->klass);
                    }
                    t = "instance";
                    break;
                }
                case OBJ_FUNCTION:   t = "function";   break;
                case OBJ_CLOSURE:    t = "closure";    break;
                case OBJ_CLASS:      t = "class";      break;
                case OBJ_RANGE:      t = "range";      break;
                default:             t = "object";     break;
            }
        }
    }
    return make_obj((Object *)new_string(t,(int)strlen(t)));
}

static Value bn_clock(VM *vm, Value *args, int n) {
    (void)vm; (void)args; (void)n;
    return make_double((double)clock() / CLOCKS_PER_SEC);
}

extern Object *all_objects;
extern int allocated_objects;

static Value bn_gc_info(VM *vm, Value *args, int n) {
    (void)vm; (void)args; (void)n;
    int count = 0;
    Object *obj = all_objects;
    while(obj) {
        count++;
        obj = obj->next;
    }
    printf("Allocated objects via counter: %d, via list: %d\n", allocated_objects, count);
    return make_int(count);
}

void mark_and_sweep(VM *vm);

static Value bn_gc(VM *vm, Value *args, int n) {
    (void)args; (void)n;
    mark_and_sweep(vm);
    return make_null();
}

static Value bn_isnan(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_bool(false);
    return make_bool(IS_NAN(args[0]));
}

static Value bn_isinf(VM *vm, Value *args, int n) {
    (void)vm;
    if (!n) return make_bool(false);
    return make_bool(IS_INF(args[0]));
}

static Value bn_abs(VM *vm, Value *args, int n) {
    (void)vm;
    if (n != 1) {
        luna_throw(vm, py_fe(vm)->argument_error_class, "abs() expects exactly 1 argument");
    }
    Value v = args[0];
    if (IS_INT(v)) {
        int32_t i = AS_INT(v);
        return bigint_from_i64_value(i < 0 ? -(int64_t)i : (int64_t)i);
    }
    if (IS_BIGINT(v)) return bigint_to_value(bigint_abs((ObjBigInt *)AS_OBJ(v)));
    if (IS_DOUBLE(v)) return make_double(fabs(AS_DOUBLE(v)));
    if (IS_BOOL(v)) return v;
    luna_throw(vm, py_fe(vm)->type_error_class, "abs() argument must be a number");
    return make_null();
}

static Value bn_chr(VM *vm, Value *args, int n) {
    (void)vm;
    if (n != 1) {
        luna_throw(vm, py_fe(vm)->argument_error_class, "chr() expects exactly 1 argument");
    }
    if (!IS_INT(args[0]) && !IS_BIGINT(args[0])) {
        luna_throw(vm, py_fe(vm)->type_error_class, "chr() argument must be an integer");
    }
    int64_t cp64;
    if (!int64_exact(args[0], &cp64)) {
        luna_throw(vm, py_fe(vm)->overflow_error_class, "chr() argument too large");
    }
    int32_t cp = (int32_t)cp64;
    if (cp < 0 || cp > 0x10FFFF) {
        luna_throw(vm, py_fe(vm)->value_error_class, "chr() argument must be in range 0..1114111");
    }
    char buf[4];
    int len;
    if (cp < 0x80) {
        buf[0] = (char)cp; len = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        len = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        len = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        len = 4;
    }
    return make_obj((Object*)new_string(buf, len));
}

static Value bn_ord(VM *vm, Value *args, int n) {
    (void)vm;
    if (n != 1) {
        luna_throw(vm, py_fe(vm)->argument_error_class, "ord() expects exactly 1 argument");
    }
    if (!IS_STRING(args[0])) {
        luna_throw(vm, py_fe(vm)->type_error_class, "ord() argument must be a string");
    }
    ObjString *str = (ObjString*)AS_OBJ(args[0]);
    if (str->length == 0) {
        luna_throw(vm, py_fe(vm)->value_error_class, "ord() argument must not be empty");
    }
    uint8_t c = (uint8_t)str->chars[0];
    int32_t cp;
    if ((c & 0x80) == 0) {
        cp = c;
    } else if ((c & 0xE0) == 0xC0 && str->length >= 2) {
        cp = ((c & 0x1F) << 6) | ((uint8_t)str->chars[1] & 0x3F);
    } else if ((c & 0xF0) == 0xE0 && str->length >= 3) {
        cp = ((c & 0x0F) << 12) | (((uint8_t)str->chars[1] & 0x3F) << 6) | ((uint8_t)str->chars[2] & 0x3F);
    } else if ((c & 0xF8) == 0xF0 && str->length >= 4) {
        cp = ((c & 0x07) << 18) | (((uint8_t)str->chars[1] & 0x3F) << 12) | (((uint8_t)str->chars[2] & 0x3F) << 6) | ((uint8_t)str->chars[3] & 0x3F);
    } else {
        luna_throw(vm, py_fe(vm)->value_error_class, "ord(): invalid UTF-8");
    }
    return bigint_from_i64_value(cp);   /* codepoints fit int32; normalizes */
}

/* ============================================================ */
/* Dict method native functions — args[0] = dict (self)         */
/* ============================================================ */

static Value dict_method_keys(VM *vm, Value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || !IS_DICT(args[0])) return make_null();
    return dict_keys((ObjDict*)AS_OBJ(args[0]));
}

static Value dict_method_has(VM *vm, Value *args, int nargs) {
    (void)vm;
    if (nargs < 2 || !IS_DICT(args[0])) return make_null();
    return make_bool(dict_has((ObjDict*)AS_OBJ(args[0]), args[1]));
}

static Value dict_method_remove(VM *vm, Value *args, int nargs) {
    (void)vm;
    if (nargs < 2 || !IS_DICT(args[0])) return make_null();
    return dict_remove((ObjDict*)AS_OBJ(args[0]), args[1]);
}

static Value dict_method_values(VM *vm, Value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || !IS_DICT(args[0])) return make_null();
    return dict_values((ObjDict*)AS_OBJ(args[0]));
}

static Value dict_method_clear(VM *vm, Value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || !IS_DICT(args[0])) return make_null();
    dict_clear((ObjDict*)AS_OBJ(args[0]));
    return make_null();
}

static Value dict_method_length(VM *vm, Value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || !IS_DICT(args[0])) return make_null();
    return make_int(dict_length((ObjDict*)AS_OBJ(args[0])));
}

/* ============================================================ */
/* List method native functions — args[0] = list (self)        */
/* ============================================================ */

static Value list_method_add(VM *vm, Value *args, int nargs) {
    (void)vm; if (nargs < 2 || !IS_LIST(args[0])) return make_null();
    list_add((ObjList*)AS_OBJ(args[0]), args[1]); return make_null();
}
static Value list_method_insert(VM *vm, Value *args, int nargs) {
    (void)vm; if (nargs < 3 || !IS_LIST(args[0]) || !IS_INT(args[1])) return make_null();
    list_insert((ObjList*)AS_OBJ(args[0]), AS_INT(args[1]), args[2]); return make_null();
}
static Value list_method_remove(VM *vm, Value *args, int nargs) {
    (void)vm; if (nargs < 2 || !IS_LIST(args[0]) || !IS_INT(args[1])) return make_null();
    return list_remove((ObjList*)AS_OBJ(args[0]), AS_INT(args[1]));
}
static Value list_method_pop(VM *vm, Value *args, int nargs) {
    (void)vm; if (nargs < 1 || !IS_LIST(args[0])) return make_null();
    return list_pop((ObjList*)AS_OBJ(args[0]));
}
static Value list_method_clear(VM *vm, Value *args, int nargs) {
    (void)vm; if (nargs < 1 || !IS_LIST(args[0])) return make_null();
    list_clear((ObjList*)AS_OBJ(args[0])); return make_null();
}
static Value list_method_length(VM *vm, Value *args, int nargs) {
    (void)vm; if (nargs < 1 || !IS_LIST(args[0])) return make_null();
    return make_int(list_length((ObjList*)AS_OBJ(args[0])));
}

/* ============================================================ */
/* String method native functions — args[0] = string (self)    */
/* ============================================================ */

static Value string_method_to_buffer(VM *vm, Value *args, int nargs) {
    if (nargs < 1 || !IS_STRING(args[0])) return make_null();
    if (nargs != 1) { luna_throw(vm, py_fe(vm)->argument_error_class, "string.to_buffer() takes no arguments"); return make_null(); }
    ObjString *str = (ObjString*)AS_OBJ(args[0]);
    ObjBuffer *buf = new_buffer((size_t)str->length);
    buffer_append_data(buf, (const uint8_t*)str->chars, (size_t)str->length);
    return make_obj((Object*)buf);
}
static Value string_method_byte(VM *vm, Value *args, int nargs) {
    if (nargs < 1 || !IS_STRING(args[0])) return make_null();
    int64_t idx64 = 0;
    if (nargs >= 2) {
        if (!IS_INT(args[1]) && !IS_BIGINT(args[1])) {
            luna_throw(vm, py_fe(vm)->type_error_class, "string.byte() expects an integer index"); return make_null();
        }
        if (!int64_exact(args[1], &idx64)) {
            luna_throw(vm, py_fe(vm)->overflow_error_class, "string.byte(): index too large"); return make_null();
        }
    }
    ObjString *str = (ObjString*)AS_OBJ(args[0]);
    if (idx64 < 0 || idx64 >= str->length) {
        luna_throw(vm, py_fe(vm)->index_error_class, "string.byte() index out of range"); return make_null();
    }
    return make_int((uint8_t)str->chars[idx64]);
}

static Value string_method_length(VM *vm, Value *args, int nargs) {
    (void)vm; (void)nargs; if (!IS_STRING(args[0])) return make_null();
    ObjString *str = (ObjString*)AS_OBJ(args[0]);
    return make_int(utf8_code_point_count(str->chars, str->length));
}

static Value string_method_size(VM *vm, Value *args, int nargs) {
    (void)vm; (void)nargs; if (!IS_STRING(args[0])) return make_null();
    return make_int(((ObjString*)AS_OBJ(args[0]))->length);
}

/* UTF-8 case mapping for a single code point; -1 when no mapping exists. */
static int utf8_case_map(int cp, bool upper) {
    if (upper) {
        if (cp >= 'a' && cp <= 'z') return cp - 32;
        if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 32;
    } else {
        if (cp >= 'A' && cp <= 'Z') return cp + 32;
        if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 32;
    }
    return -1;
}

/* Encode a code point as UTF-8 into buf (up to 4 bytes); returns byte count. */
static int utf8_encode(int cp, char *buf) {
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

/* Decode the code point starting at chars[i] (i advanced past it). */
static int utf8_decode(const char *chars, int *i) {
    unsigned char c = (unsigned char)chars[*i];
    int cp;
    int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (len == 1) { cp = c; (*i)++; return cp; }
    cp = c & (0x7F >> len);
    for (int k = 1; k < len && (unsigned char)chars[*i + k]; k++) {
        cp = (cp << 6) | ((unsigned char)chars[*i + k] & 0x3F);
    }
    *i += len;
    return cp;
}

/* Shared implementation for upper/lower: preserves byte length only when the
 * case mapping does not change any code point's UTF-8 width. */
static Value string_case_method(VM *vm, Value *args, bool upper) {
    if (!IS_STRING(args[0])) return make_null();
    ObjString *str = (ObjString*)AS_OBJ(args[0]);
    int len = str->length;
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    int pos = 0, i = 0, out_len = 0;
    while (i < len) {
        int start = i;
        int cp = utf8_decode(str->chars, &i);
        int mapped = utf8_case_map(cp, upper);
        if (mapped < 0) {
            memcpy(buf + pos, str->chars + start, i - start);
            pos += i - start;
            out_len += i - start;
        } else {
            char tmp[4];
            int n = utf8_encode(mapped, tmp);
            memcpy(buf + pos, tmp, (size_t)n);
            pos += n;
            out_len += n;
        }
    }
    buf[pos] = '\0';
    ObjString *result = new_string(buf, out_len);
    free(buf);
    (void)vm;
    return make_obj((Object*)result);
}

static Value string_method_upper(VM *vm, Value *args, int nargs) {
    (void)vm; (void)nargs;
    return string_case_method(vm, args, true);
}

static Value string_method_lower(VM *vm, Value *args, int nargs) {
    (void)vm; (void)nargs;
    return string_case_method(vm, args, false);
}

static Value string_method_reverse(VM *vm, Value *args, int nargs) {
    (void)vm; (void)nargs;
    if (!IS_STRING(args[0])) return make_null();
    ObjString *str = (ObjString*)AS_OBJ(args[0]);
    int len = str->length;
    if (len <= 1) return make_obj((Object*)new_string(str->chars, len));
    int cp_count = utf8_code_point_count(str->chars, len);
    int *starts = malloc(cp_count * sizeof(int));
    if (!starts) { fprintf(stderr, "OOM\n"); exit(1); }
    int idx = 0;
    for (int i = 0; i < len; i++) {
        if ((str->chars[i] & 0xC0) != 0x80) starts[idx++] = i;
    }
    char *buf = malloc(len + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    int pos = 0;
    for (int i = cp_count - 1; i >= 0; i--) {
        int cp_start = starts[i];
        int cp_len = (i < cp_count - 1) ? starts[i + 1] - cp_start : len - cp_start;
        memcpy(buf + pos, str->chars + cp_start, cp_len);
        pos += cp_len;
    }
    buf[pos] = '\0';
    ObjString *result = new_string(buf, len);
    free(buf);
    free(starts);
    return make_obj((Object*)result);
}

void vm_register_builtins(VM *vm) {
    vm_define_native(vm, "print", bn_print);
    vm_define_native(vm, "input", bn_input);
    vm_define_native(vm, "range", bn_range);
    vm_define_native(vm, "repr",  bn_repr);
    vm_define_native(vm, "len",   bn_len);
    vm_define_native(vm, "type",  bn_type);
    vm_define_native(vm, "clock", bn_clock);
    vm_define_native(vm, "gc_info", bn_gc_info);
    vm_define_native(vm, "gc", bn_gc);
    vm_define_native(vm, "isnan", bn_isnan);
    vm_define_native(vm, "isinf", bn_isinf);
    vm_define_native(vm, "abs",   bn_abs);
    vm_define_native(vm, "ord",   bn_ord);
    vm_define_native(vm, "chr",   bn_chr);
    vm_define_native(vm, "_build_tuple", bn_build_tuple);
    vm_define_native(vm, "isinstance", bn_isinstance);
    vm_define_native(vm, "slice", py_builtin_slice);
}

/* ============================================================ */
/* Canonical class registration                                 */
/* ============================================================ */

void vm_register_canonical_classes(VM *vm) {
    extern void vm_register_string_methods(VM *vm);
    /* Python-style builtin type classes (exposed as globals).
     * int/float are created and registered by py_register_number_methods. */
    py_fe(vm)->tuple_class = new_class("tuple", NULL);
    vm_set_global(vm, "tuple", make_obj((Object*)py_fe(vm)->tuple_class), false);

    /* String canonical class */
    py_fe(vm)->string_class = new_class("String", NULL);
    class_add_native_method(py_fe(vm)->string_class, "to_buffer", string_method_to_buffer);
    class_add_native_method(py_fe(vm)->string_class, "byte", string_method_byte);
    class_add_native_method(py_fe(vm)->string_class, "length", string_method_length);
    class_add_native_method(py_fe(vm)->string_class, "size", string_method_size);
    class_add_native_method(py_fe(vm)->string_class, "reverse", string_method_reverse);
    class_add_native_method(py_fe(vm)->string_class, "upper", string_method_upper);
    class_add_native_method(py_fe(vm)->string_class, "lower", string_method_lower);
    vm_register_string_methods(vm);
    vm_set_global(vm, "str", make_obj((Object*)py_fe(vm)->string_class), false);

    /* List canonical class */
    py_fe(vm)->list_class = new_class("List", NULL);
    class_add_native_method(py_fe(vm)->list_class, "add", list_method_add);
    class_add_native_method(py_fe(vm)->list_class, "append", list_method_add);
    class_add_native_method(py_fe(vm)->list_class, "insert", list_method_insert);
    class_add_native_method(py_fe(vm)->list_class, "remove", list_method_remove);
    class_add_native_method(py_fe(vm)->list_class, "pop", list_method_pop);
    class_add_native_method(py_fe(vm)->list_class, "clear", list_method_clear);
    class_add_native_method(py_fe(vm)->list_class, "length", list_method_length);
    class_add_native_method(py_fe(vm)->list_class, "size", list_method_length);
    vm_set_global(vm, "list", make_obj((Object*)py_fe(vm)->list_class), false);

    /* Dict canonical class */
    py_fe(vm)->dict_class = new_class("Dict", NULL);
    class_add_native_method(py_fe(vm)->dict_class, "keys", dict_method_keys);
    class_add_native_method(py_fe(vm)->dict_class, "has", dict_method_has);
    class_add_native_method(py_fe(vm)->dict_class, "remove", dict_method_remove);
    class_add_native_method(py_fe(vm)->dict_class, "values", dict_method_values);
    class_add_native_method(py_fe(vm)->dict_class, "clear", dict_method_clear);
    class_add_native_method(py_fe(vm)->dict_class, "length", dict_method_length);
    vm_set_global(vm, "dict", make_obj((Object*)py_fe(vm)->dict_class), false);

    py_fe(vm)->function_class = new_class("Function", NULL);
    py_fe(vm)->closure_class = new_class("Closure", NULL);
    py_fe(vm)->bound_method_class = new_class("BoundMethod", NULL);
    py_fe(vm)->class_class = new_class("Class", NULL);
    py_fe(vm)->module_class = new_class("Module", NULL);
}


