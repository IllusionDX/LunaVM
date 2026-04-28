/* stdlib_json.c — JSON parse/encode module for Luna
 *
 * json.parse(text)  -> Luna value
 * json.encode(val)  -> JSON string
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include "stdlib_json.h"
#include "value.h"

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    VM *vm;
} JsonParser;

/* ============================================================ */
/* Forward declarations                                          */
/* ============================================================ */

static Value parse_value(JsonParser *p);

/* ============================================================ */
/* Helpers                                                       */
/* ============================================================ */

static inline char peek(JsonParser *p) {
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos];
}

static inline char advance(JsonParser *p) {
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos++];
}

static inline void skip_ws(JsonParser *p) {
    while (p->pos < p->len && isspace((unsigned char)p->src[p->pos])) {
        p->pos++;
    }
}

static void json_error(JsonParser *p, const char *msg) {
    luna_throw(p->vm, p->vm->runtime_error_class, "JSON parse error at pos %zu: %s", p->pos, msg);
}

/* ============================================================ */
/* String parsing                                                */
/* ============================================================ */

static Value parse_string(JsonParser *p) {
    if (advance(p) != '"') json_error(p, "expected '\"'");

    size_t capacity = 64;
    size_t out_len = 0;
    char *buf = (char*)malloc(capacity);
    if (!buf) json_error(p, "out of memory");

    while (p->pos < p->len && peek(p) != '"') {
        char c = advance(p);
        if (c == '\\') {
            if (p->pos >= p->len) {
                free(buf);
                json_error(p, "unterminated escape");
            }
            char esc = advance(p);
            switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    if (p->pos + 4 > p->len) {
                        free(buf);
                        json_error(p, "incomplete unicode escape");
                    }
                    /* Parse 4 hex digits, skip for now (store as UTF-8 if valid BMP) */
                    unsigned int code = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = advance(p);
                        unsigned int digit = 0;
                        if (h >= '0' && h <= '9') digit = h - '0';
                        else if (h >= 'a' && h <= 'f') digit = 10 + (h - 'a');
                        else if (h >= 'A' && h <= 'F') digit = 10 + (h - 'A');
                        else {
                            free(buf);
                            json_error(p, "invalid hex digit in \\u escape");
                        }
                        code = (code << 4) | digit;
                    }
                    /* Encode code point as UTF-8 */
                    if (code <= 0x7F) {
                        c = (char)code;
                        goto push_char;
                    } else if (code <= 0x7FF) {
                        if (out_len + 2 > capacity) {
                            capacity *= 2;
                            buf = (char*)realloc(buf, capacity);
                        }
                        buf[out_len++] = (char)(0xC0 | (code >> 6));
                        buf[out_len++] = (char)(0x80 | (code & 0x3F));
                        continue;
                    } else {
                        if (out_len + 3 > capacity) {
                            capacity *= 2;
                            buf = (char*)realloc(buf, capacity);
                        }
                        buf[out_len++] = (char)(0xE0 | (code >> 12));
                        buf[out_len++] = (char)(0x80 | ((code >> 6) & 0x3F));
                        buf[out_len++] = (char)(0x80 | (code & 0x3F));
                        continue;
                    }
                }
                default:
                    free(buf);
                    json_error(p, "invalid escape sequence");
            }
        } else if ((unsigned char)c < 0x20) {
            free(buf);
            json_error(p, "unescaped control character");
        }

    push_char:
        if (out_len + 1 >= capacity) {
            capacity *= 2;
            buf = (char*)realloc(buf, capacity);
        }
        buf[out_len++] = c;
    }

    if (advance(p) != '"') {
        free(buf);
        json_error(p, "unterminated string");
    }

    buf[out_len] = '\0';
    ObjString *s = new_string(buf, (int)out_len);
    free(buf);
    return make_obj((Object*)s);
}

/* ============================================================ */
/* Number parsing                                                */
/* ============================================================ */

static Value parse_number(JsonParser *p) {
    size_t start = p->pos;
    if (peek(p) == '-') advance(p);
    if (peek(p) == '0') {
        advance(p);
    } else if (peek(p) >= '1' && peek(p) <= '9') {
        while (p->pos < p->len && isdigit((unsigned char)peek(p))) advance(p);
    } else {
        json_error(p, "invalid number");
    }

    bool is_float = false;
    if (peek(p) == '.') {
        is_float = true;
        advance(p);
        if (!isdigit((unsigned char)peek(p))) json_error(p, "invalid number: no digits after decimal");
        while (p->pos < p->len && isdigit((unsigned char)peek(p))) advance(p);
    }
    if (peek(p) == 'e' || peek(p) == 'E') {
        is_float = true;
        advance(p);
        if (peek(p) == '+' || peek(p) == '-') advance(p);
        if (!isdigit((unsigned char)peek(p))) json_error(p, "invalid number: no digits in exponent");
        while (p->pos < p->len && isdigit((unsigned char)peek(p))) advance(p);
    }

    size_t len = p->pos - start;
    char *tmp = (char*)malloc(len + 1);
    memcpy(tmp, p->src + start, len);
    tmp[len] = '\0';

    Value result;
    if (!is_float) {
        /* Try int64 first, then int32, then fallback to double */
        errno = 0;
        long long ll = strtoll(tmp, NULL, 10);
        if (errno == 0) {
            if (ll >= INT32_MIN && ll <= INT32_MAX) {
                result = make_int((int32_t)ll);
            } else {
                result = make_obj((Object*)new_int64((int64_t)ll));
            }
            free(tmp);
            return result;
        }
    }

    /* Parse as double */
    errno = 0;
    double d = strtod(tmp, NULL);
    if (errno == ERANGE) {
        free(tmp);
        json_error(p, "number out of range");
    }
    free(tmp);
    return make_double(d);
}

/* ============================================================ */
/* Object / Array parsing                                        */
/* ============================================================ */

static Value parse_object(JsonParser *p) {
    if (advance(p) != '{') json_error(p, "expected '{'");
    skip_ws(p);
    ObjDict *dict = new_dict();

    if (peek(p) == '}') {
        advance(p);
        return make_obj((Object*)dict);
    }

    for (;;) {
        skip_ws(p);
        Value key = parse_string(p);
        skip_ws(p);
        if (advance(p) != ':') json_error(p, "expected ':' after object key");
        skip_ws(p);
        Value val = parse_value(p);
        dict_set(dict, key, val);
        skip_ws(p);
        char c = advance(p);
        if (c == '}') break;
        if (c != ',') json_error(p, "expected ',' or '}' in object");
    }

    return make_obj((Object*)dict);
}

static Value parse_array(JsonParser *p) {
    if (advance(p) != '[') json_error(p, "expected '['");
    skip_ws(p);
    ObjList *list = new_list(0);

    if (peek(p) == ']') {
        advance(p);
        return make_obj((Object*)list);
    }

    for (;;) {
        skip_ws(p);
        Value val = parse_value(p);
        list_add(list, val);
        skip_ws(p);
        char c = advance(p);
        if (c == ']') break;
        if (c != ',') json_error(p, "expected ',' or ']' in array");
    }

    return make_obj((Object*)list);
}

/* ============================================================ */
/* Value dispatch                                                */
/* ============================================================ */

static Value parse_value(JsonParser *p) {
    skip_ws(p);
    char c = peek(p);

    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    if (c == '"') return parse_string(p);
    if (c == 't') {
        if (p->pos + 4 <= p->len && memcmp(p->src + p->pos, "true", 4) == 0) {
            p->pos += 4;
            return make_bool(true);
        }
        json_error(p, "expected 'true'");
    }
    if (c == 'f') {
        if (p->pos + 5 <= p->len && memcmp(p->src + p->pos, "false", 5) == 0) {
            p->pos += 5;
            return make_bool(false);
        }
        json_error(p, "expected 'false'");
    }
    if (c == 'n') {
        if (p->pos + 4 <= p->len && memcmp(p->src + p->pos, "null", 4) == 0) {
            p->pos += 4;
            return make_null();
        }
        json_error(p, "expected 'null'");
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p);

    json_error(p, "unexpected character");
    return make_null(); /* unreachable */
}

/* ============================================================ */
/* Encode helpers                                                */
/* ============================================================ */

static void sb_append(char **buf, size_t *len, size_t *cap, const char *s, size_t slen) {
    if (*len + slen >= *cap) {
        while (*len + slen >= *cap) *cap *= 2;
        *buf = (char*)realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, slen);
    *len += slen;
}

static void sb_append_cstr(char **buf, size_t *len, size_t *cap, const char *s) {
    sb_append(buf, len, cap, s, strlen(s));
}

static void sb_append_char(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 1 >= *cap) {
        *cap *= 2;
        *buf = (char*)realloc(*buf, *cap);
    }
    (*buf)[(*len)++] = c;
}

static void encode_string(char **buf, size_t *len, size_t *cap, const char *s, int slen) {
    sb_append_char(buf, len, cap, '"');
    for (int i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': sb_append_cstr(buf, len, cap, "\\\""); break;
            case '\\': sb_append_cstr(buf, len, cap, "\\\\"); break;
            case '\b': sb_append_cstr(buf, len, cap, "\\b"); break;
            case '\f': sb_append_cstr(buf, len, cap, "\\f"); break;
            case '\n': sb_append_cstr(buf, len, cap, "\\n"); break;
            case '\r': sb_append_cstr(buf, len, cap, "\\r"); break;
            case '\t': sb_append_cstr(buf, len, cap, "\\t"); break;
            default:
                if (c < 0x20) {
                    char esc[7];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    sb_append_cstr(buf, len, cap, esc);
                } else {
                    sb_append_char(buf, len, cap, (char)c);
                }
        }
    }
    sb_append_char(buf, len, cap, '"');
}

static void encode_value(VM *vm, Value v, char **buf, size_t *len, size_t *cap, int depth);

static void encode_dict(VM *vm, ObjDict *dict, char **buf, size_t *len, size_t *cap, int depth) {
    sb_append_char(buf, len, cap, '{');
    bool first = true;
    if (dict->indices == NULL) {
        /* SOO mode */
        for (int i = 0; i < dict->entry_count; i++) {
            Value key = dict->inline_entries[i].key;
            Value val = dict->inline_entries[i].value;
            if (!IS_STRING(key)) {
                luna_throw(vm, vm->type_error_class,
                    "json.encode(): dict keys must be strings");
            }
            if (!first) sb_append_char(buf, len, cap, ',');
            first = false;
            ObjString *k = (ObjString*)AS_OBJ(key);
            encode_string(buf, len, cap, k->chars, k->length);
            sb_append_char(buf, len, cap, ':');
            encode_value(vm, val, buf, len, cap, depth + 1);
        }
    } else {
        /* Heap mode */
        for (int i = 0; i < dict->next_entry; i++) {
            Value key = dict->entries[i].key;
            if (key == EMPTY_VAL) continue;
            if (!IS_STRING(key)) {
                luna_throw(vm, vm->type_error_class,
                    "json.encode(): dict keys must be strings");
            }
            if (!first) sb_append_char(buf, len, cap, ',');
            first = false;
            ObjString *k = (ObjString*)AS_OBJ(key);
            encode_string(buf, len, cap, k->chars, k->length);
            sb_append_char(buf, len, cap, ':');
            encode_value(vm, dict->entries[i].value, buf, len, cap, depth + 1);
        }
    }
    sb_append_char(buf, len, cap, '}');
}

static void encode_list(VM *vm, ObjList *list, char **buf, size_t *len, size_t *cap, int depth) {
    sb_append_char(buf, len, cap, '[');
    for (int i = 0; i < list->count; i++) {
        if (i > 0) sb_append_char(buf, len, cap, ',');
        Value v = list->items ? list->items[i] : list->inline_items[i];
        encode_value(vm, v, buf, len, cap, depth + 1);
    }
    sb_append_char(buf, len, cap, ']');
}

static void encode_value(VM *vm, Value v, char **buf, size_t *len, size_t *cap, int depth) {
    if (depth > 100) {
        luna_throw(vm, vm->runtime_error_class, "json.encode(): nesting too deep");
    }

    if (IS_NIL(v)) {
        sb_append_cstr(buf, len, cap, "null");
    } else if (IS_BOOL(v)) {
        sb_append_cstr(buf, len, cap, AS_BOOL(v) ? "true" : "false");
    } else if (IS_INT(v)) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%lld", (long long)AS_INT(v));
        sb_append_cstr(buf, len, cap, tmp);
    } else if (IS_INT64(v)) {
        ObjInt64 *obj = (ObjInt64*)AS_OBJ(v);
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%lld", (long long)obj->value);
        sb_append_cstr(buf, len, cap, tmp);
    } else if (IS_DOUBLE(v)) {
        double d = AS_DOUBLE(v);
        if (isinf(d) || isnan(d)) {
            luna_throw(vm, vm->runtime_error_class,
                "json.encode(): cannot encode infinity or NaN");
        }
        char tmp[64];
        /* Use %.17g for round-trip precision, but avoid trailing .0 for integers */
        snprintf(tmp, sizeof(tmp), "%.17g", d);
        sb_append_cstr(buf, len, cap, tmp);
    } else if (IS_STRING(v)) {
        ObjString *s = (ObjString*)AS_OBJ(v);
        encode_string(buf, len, cap, s->chars, s->length);
    } else if (IS_DICT(v)) {
        encode_dict(vm, (ObjDict*)AS_OBJ(v), buf, len, cap, depth);
    } else if (IS_LIST(v)) {
        encode_list(vm, (ObjList*)AS_OBJ(v), buf, len, cap, depth);
    } else {
        luna_throw(vm, vm->type_error_class,
            "json.encode(): unsupported type for JSON serialization");
    }
}

/* ============================================================ */
/* Native wrappers                                               */
/* ============================================================ */

static Value json_parse_native(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "json.parse() expects exactly 1 argument");
    }
    if (!IS_STRING(args[0])) {
        luna_throw(vm, vm->type_error_class, "json.parse() argument must be a string");
    }
    ObjString *s = (ObjString*)AS_OBJ(args[0]);
    JsonParser p = { s->chars, (size_t)s->length, 0, vm };
    Value result = parse_value(&p);
    skip_ws(&p);
    if (p.pos != p.len) {
        luna_throw(vm, vm->runtime_error_class,
            "json.parse(): trailing data after valid JSON at pos %zu", p.pos);
    }
    return result;
}

static Value json_encode_native(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "json.encode() expects exactly 1 argument");
    }
    size_t cap = 256;
    size_t len = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) {
        luna_throw(vm, vm->runtime_error_class, "json.encode(): out of memory");
    }
    encode_value(vm, args[0], &buf, &len, &cap, 0);
    buf[len] = '\0';
    ObjString *result = new_string(buf, (int)len);
    free(buf);
    return make_obj((Object*)result);
}

/* ============================================================ */
/* Module registration                                           */
/* ============================================================ */

static void module_add_native(ObjModule *mod, const char *name, NativeFn fn) {
    ObjFunction *f = new_native_function(name, fn);
    Value key = make_obj((Object*)new_string(name, (int)strlen(name)));
    dict_set(mod->exports, key, make_obj((Object*)f));
}

void vm_register_json_module(VM *vm) {
    ObjModule *mod = new_module("json");
    module_add_native(mod, "parse", json_parse_native);
    module_add_native(mod, "encode", json_encode_native);
    dict_set(vm->module_cache,
             make_obj((Object*)new_string("json", 4)),
             make_obj((Object*)mod));
}
