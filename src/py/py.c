/* py.c — Python-subset frontend: MOP hooks, builtins, object bridge. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "api.h"
#include "vm.h"
#include "value.h"
#include "py/object.h"
#include "py/number_methods.h"
#include "py/frontend_state.h"
#include "chunk.h"
#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include "ast.h"
#include "module.h"

/* ============================================================ */
/* GC hook — called by vm.c's mark_and_sweep                    */
/* ============================================================ */

void py_mark_roots(VM *vm) {
    api_mark_roots(vm);
    /* All language-owned roots live in the frontend state. The core must not
     * know which canonical classes or module cache a language uses. */
    PyFrontend *fe = py_fe(vm);
    if (!fe) return;
    void *roots[] = {
        fe->module_cache, fe->exception_class, fe->type_error_class,
        fe->key_error_class, fe->index_error_class, fe->attribute_error_class,
        fe->value_error_class, fe->runtime_error_class, fe->argument_error_class,
        fe->string_class, fe->list_class, fe->dict_class, fe->enum_class,
        fe->buffer_class, fe->vector_class, fe->matrix_class, fe->function_class,
        fe->closure_class, fe->bound_method_class, fe->class_class,
        fe->module_class, fe->userdata_class, fe->int_class, fe->float_class
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (roots[i]) vm_mark_value(vm, make_obj(roots[i]));
    }
    /* Open upvalues are frontend-owned; mark each (its mark MOP covers the
     * captured value once closed). */
    void *uv = vm->open_upvalues;
    while (uv) {
        vm_mark_value(vm, make_obj(uv));
        uv = ((ObjUpvalue*)uv)->next;
    }
}

static double py_to_f64(Value v) {
    if (IS_INT(v)) return (double)AS_INT(v);
    if (IS_INT64(v)) return (double)((ObjInt64 *)AS_OBJ(v))->value;
    return IS_DOUBLE(v) ? AS_DOUBLE(v) : 0.0;
}

static int64_t py_to_i64(Value v) {
    if (IS_INT(v)) return (int64_t)AS_INT(v);
    if (IS_INT64(v)) return ((ObjInt64 *)AS_OBJ(v))->value;
    return IS_DOUBLE(v) ? (int64_t)AS_DOUBLE(v) : 0;
}

static bool py_value_is_integer(Value v) { return IS_INT(v) || IS_INT64(v); }

/* Exact int64 overflow check for `a * b` (avoids signed-overflow UB). */
static bool py_i64_mul_overflows(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return false;
    if (a > 0) {
        if (b > 0) return a > INT64_MAX / b;
        return b < INT64_MIN / a;
    }
    if (b > 0) return a < INT64_MIN / b;
    return a < INT64_MAX / b;
}

static Value py_integer_result(int64_t value) {
    return value >= INT32_MIN && value <= INT32_MAX
        ? make_int((int32_t)value) : make_int64(value);
}

static MOP_Bin py_binary_method(Value self, VMOperation op) {
    if (!IS_OBJ(self) || !AS_OBJ(self) || !AS_OBJ(self)->type) return NULL;
    Type *type = AS_OBJ(self)->type;
    switch (op) {
        case VM_OP_ADD: return type->add;
        case VM_OP_SUB: return type->sub;
        case VM_OP_MUL: return type->mul;
        case VM_OP_DIV: return type->div;
        case VM_OP_MOD: return type->mod;
        default: return NULL;
    }
}

static bool py_unary_operation(VM *vm, VMOperation op, Value operand, Value *out) {
    if (op == VM_OP_NEG && IS_OBJ(operand) && AS_OBJ(operand)->type &&
        AS_OBJ(operand)->type->neg) {
        *out = AS_OBJ(operand)->type->neg(vm, operand);
        return true;
    }
    if (op == VM_OP_NEG && py_value_is_integer(operand)) {
        *out = py_integer_result(-py_to_i64(operand));
        return true;
    }
    if (op == VM_OP_NEG && IS_DOUBLE(operand)) {
        *out = make_double(-AS_DOUBLE(operand));
        return true;
    }
    if (op == VM_OP_BNOT && py_value_is_integer(operand)) {
        *out = py_integer_result(~py_to_i64(operand));
        return true;
    }
    return false;
}

static bool py_binary_operation(VM *vm, VMOperation op, Value left, Value right, Value *out) {
    MOP_Bin method = py_binary_method(left, op);
    if (method) { *out = method(vm, left, right); return !IS_NIL(*out); }
    if ((op == VM_OP_ADD || op == VM_OP_MUL) && (method = py_binary_method(right, op))) {
        *out = method(vm, right, left);
        return !IS_NIL(*out);
    }
    if (!IS_NUMBER(left) || !IS_NUMBER(right)) return false;
    if (py_value_is_integer(left) && py_value_is_integer(right)) {
        int64_t a = py_to_i64(left), b = py_to_i64(right);
        switch (op) {
            case VM_OP_ADD: *out = py_integer_result(a + b); return true;
            case VM_OP_SUB: *out = py_integer_result(a - b); return true;
            case VM_OP_MUL: *out = py_integer_result(a * b); return true;
            case VM_OP_DIV:
                /* Python 3: `/` is always true division — int / int → float */
                if (b) { *out = make_double((double)a / (double)b); return true; }
                vm->last_exception = make_exception_instance(vm, py_fe(vm)->runtime_error_class, "division by zero");
                return false;
            case VM_OP_MOD:
                if (b) { *out = py_integer_result(a % b); return true; }
                vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, "mod/0");
                return false;
            case VM_OP_IDIV: {
                if (b) {
                    if (a == INT64_MIN && b == -1) { *out = make_double(9223372036854775808.0); return true; }
                    int64_t q = a / b;
                    int64_t r = a % b;
                    if (r != 0 && ((r < 0) != (b < 0))) q -= 1;  /* floor, not truncate */
                    *out = py_integer_result(q);
                    return true;
                }
                vm->last_exception = make_exception_instance(vm, py_fe(vm)->runtime_error_class, "division by zero");
                return false;
            }
            case VM_OP_POW: {
                if (b < 0) { *out = make_double(pow(py_to_f64(left), py_to_f64(right))); return true; }
                int64_t base = a, res = 1, exp = b;
                bool overflow = false;
                while (exp > 0 && !overflow) {
                    if (exp & 1) { if (py_i64_mul_overflows(res, base)) overflow = true; else res *= base; }
                    exp >>= 1;
                    if (exp && !overflow) { if (py_i64_mul_overflows(base, base)) overflow = true; else base *= base; }
                }
                if (overflow) { *out = make_double(pow(py_to_f64(left), py_to_f64(right))); return true; }
                *out = py_integer_result(res);
                return true;
            }
            case VM_OP_BAND: *out = py_integer_result(a & b); return true;
            case VM_OP_BOR:  *out = py_integer_result(a | b); return true;
            case VM_OP_BXOR: *out = py_integer_result(a ^ b); return true;
            case VM_OP_SHL:  *out = py_integer_result(a << b); return true;
            case VM_OP_SHR:  *out = py_integer_result(a >> b); return true;
            default: break;
        }
    }
    double a = py_to_f64(left), b = py_to_f64(right);
    switch (op) {
        case VM_OP_ADD: *out = make_double(a + b); return true;
        case VM_OP_SUB: *out = make_double(a - b); return true;
        case VM_OP_MUL: *out = make_double(a * b); return true;
        case VM_OP_DIV: *out = b == 0.0 ? (a == 0.0 ? make_double(0.0 / 0.0) :
            (a > 0.0 ? make_pos_inf() : make_neg_inf())) : make_double(a / b); return true;
        case VM_OP_MOD: *out = b == 0.0 ? make_double(0.0 / 0.0) : make_double(fmod(a, b)); return true;
        case VM_OP_IDIV: *out = b == 0.0 ? (a == 0.0 ? make_double(0.0 / 0.0) :
            (a > 0.0 ? make_pos_inf() : make_neg_inf())) : make_double(floor(a / b)); return true;
        case VM_OP_POW: *out = make_double(pow(a, b)); return true;
        default: return false;
    }
}

static bool py_compare_operation(VM *vm, VMOperation op, Value left, Value right, Value *out) {
    (void)vm;
    if (py_value_is_integer(left) && py_value_is_integer(right)) {
        int64_t a = py_to_i64(left), b = py_to_i64(right);
        switch (op) {
            case VM_OP_EQ: *out = make_bool(a == b); return true;
            case VM_OP_NE: *out = make_bool(a != b); return true;
            case VM_OP_LT: *out = make_bool(a < b); return true;
            case VM_OP_LE: *out = make_bool(a <= b); return true;
            case VM_OP_GT: *out = make_bool(a > b); return true;
            case VM_OP_GE: *out = make_bool(a >= b); return true;
            default: break;
        }
    }
    if (IS_NUMBER(left) && IS_NUMBER(right)) {
        double a = py_to_f64(left), b = py_to_f64(right);
        switch (op) {
            case VM_OP_LT: *out = make_bool(a < b); return true;
            case VM_OP_LE: *out = make_bool(a <= b); return true;
            case VM_OP_GT: *out = make_bool(a > b); return true;
            case VM_OP_GE: *out = make_bool(a >= b); return true;
            default: break;
        }
    }
    if (op == VM_OP_EQ || op == VM_OP_NE) {
        *out = make_bool(op == VM_OP_EQ ? values_equal(left, right) : !values_equal(left, right));
        return true;
    }
    if (IS_OBJ(left) && AS_OBJ(left)->type && AS_OBJ(left)->type->cmp) {
        int cmp = AS_OBJ(left)->type->cmp(vm, left, right);
        if (cmp != 2) {
            *out = make_bool(op == VM_OP_LT ? cmp < 0 : op == VM_OP_LE ? cmp <= 0 :
                             op == VM_OP_GT ? cmp > 0 : cmp >= 0);
            return true;
        }
    }
    return false;
}

static bool py_index_get(VM *vm, Value object, Value key, bool safe, Value *out) {
    if (!IS_OBJ(object) || !AS_OBJ(object)) {
        if (safe) { *out = make_null(); return true; }
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->type_error_class, "cannot index null value");
        return false;
    }
    Type *type = AS_OBJ(object)->type;
    if (!type || !type->getitem) {
        if (safe) { *out = make_null(); return true; }
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->type_error_class, "value is not indexable");
        return false;
    }
    if (IS_LIST(object) && !IS_INT(key)) {
        if (safe) { *out = make_null(); return true; }
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->type_error_class, "list index must be integer");
        return false;
    }
    if (IS_STRING(object) && !IS_INT(key)) {
        if (safe) { *out = make_null(); return true; }
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->type_error_class, "string index must be integer");
        return false;
    }
    *out = type->getitem(vm, object, key);
    if (!safe && IS_DICT(object) && !dict_has((ObjDict *)AS_OBJ(object), key)) {
        char *ks = value_to_string(key);
        char message[256];
        snprintf(message, sizeof(message), "%s not found in dict", ks);
        free(ks);
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->key_error_class, message);
        return false;
    }
    if (!safe && IS_LIST(object)) {
        ObjList *list = (ObjList *)AS_OBJ(object);
        int index = AS_INT(key); if (index < 0) index += list->count;
        if (index < 0 || index >= list->count) {
            vm->last_exception = make_exception_instance(vm, py_fe(vm)->index_error_class, "list index out of bounds");
            return false;
        }
    }
    if (!safe && IS_STRING(object)) {
        ObjString *string = (ObjString *)AS_OBJ(object);
        int index = AS_INT(key); if (index < 0) index += string->length;
        if (index < 0 || index >= string->length) {
            vm->last_exception = make_exception_instance(vm, py_fe(vm)->index_error_class, "string index out of bounds");
            return false;
        }
    }
    return true;
}

static bool py_index_set(VM *vm, Value object, Value key, Value value) {
    if (!IS_OBJ(object) || !AS_OBJ(object) || !AS_OBJ(object)->type ||
        !AS_OBJ(object)->type->setitem) {
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->type_error_class, "invalid index assignment");
        return false;
    }
    if (IS_LIST(object) && !IS_INT(key)) {
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->type_error_class, "list index must be integer");
        return false;
    }
    if (IS_LIST(object)) {
        ObjList *list = (ObjList *)AS_OBJ(object);
        int index = AS_INT(key);
        if (index < 0) index += list->count;
        if (index < 0 || index >= list->count) {
            char message[128];
            snprintf(message, sizeof(message), "list index out of bounds (index: %d, size: %d)", index, list->count);
            vm->last_exception = make_exception_instance(vm, py_fe(vm)->index_error_class, message);
            return false;
        }
    }
    AS_OBJ(object)->type->setitem(vm, object, key, value);
    return true;
}

/* Slicing semantics live in the frontend: the core only knows the neutral
 * OP_SLICE opcode. Non-indexable objects (including null)
 * yield null, matching the previous core behaviour. */
static bool py_slice(VM *vm, Value object, Value start_val, Value stop_val,
                       Value step_val, bool safe, Value *out) {
    (void)vm; (void)safe;
    if (!IS_LIST(object) && !IS_STRING(object)) {
        *out = make_null();
        return true;
    }
    int step = IS_INT(step_val) ? AS_INT(step_val) : 1;
    if (IS_LIST(object)) {
        ObjList *lst = (ObjList *)AS_OBJ(object);
        int len = list_length(lst);
        int start, stop;
        if (IS_NIL(start_val)) start = (step < 0) ? len - 1 : 0;
        else if (IS_INT(start_val)) {
            start = AS_INT(start_val);
            if (start < 0) start += len;
            if (start < 0) start = (step < 0) ? -1 : 0;
            if (start > len) start = len;
        } else start = (step < 0) ? len - 1 : 0;
        if (IS_NIL(stop_val)) stop = (step < 0) ? -1 : len;
        else if (IS_INT(stop_val)) {
            stop = AS_INT(stop_val);
            if (stop < 0) stop += len;
            if (stop < 0) stop = -1;
            if (stop > len) stop = len;
        } else stop = (step < 0) ? -1 : len;
        ObjList *result = new_list(0);
        if (step > 0) {
            for (int i = start; i < stop; i += step) {
                Value v = lst->items ? lst->items[i] : lst->inline_items[i];
                list_add(result, v);
            }
        } else if (step < 0) {
            for (int i = start; i > stop; i += step) {
                Value v = lst->items ? lst->items[i] : lst->inline_items[i];
                list_add(result, v);
            }
        }
        *out = make_obj((Object *)result);
        return true;
    }
    /* string */
    ObjString *s = (ObjString *)AS_OBJ(object);
    int byte_len = s->length;
    int cp_count = utf8_code_point_count(s->chars, byte_len);
    int len = cp_count;
    int start, stop;
    if (IS_NIL(start_val)) start = (step < 0) ? len - 1 : 0;
    else if (IS_INT(start_val)) {
        start = AS_INT(start_val);
        if (start < 0) start += len;
        if (start < 0) start = (step < 0) ? -1 : 0;
        if (start > len) start = len;
    } else start = (step < 0) ? len - 1 : 0;
    if (IS_NIL(stop_val)) stop = (step < 0) ? -1 : len;
    else if (IS_INT(stop_val)) {
        stop = AS_INT(stop_val);
        if (stop < 0) stop += len;
        if (stop < 0) stop = -1;
        if (stop > len) stop = len;
    } else stop = (step < 0) ? -1 : len;
    if (step == 0) {
        *out = make_obj((Object *)new_string("", 0));
        return true;
    }
    int *cp_pos = (int *)malloc((size_t)cp_count * sizeof(int));
    if (!cp_pos) { fprintf(stderr, "OOM\n"); exit(1); }
    int ci = 0;
    for (int i = 0; i < byte_len; i++) {
        if ((s->chars[i] & 0xC0) != 0x80) cp_pos[ci++] = i;
    }
    int total_bytes = 0;
    if (step > 0) {
        for (int i = start; i < stop; i += step) {
            int cp_start = cp_pos[i];
            int cp_end = (i < cp_count - 1) ? cp_pos[i + 1] : byte_len;
            total_bytes += cp_end - cp_start;
        }
    } else {
        for (int i = start; i > stop; i += step) {
            int cp_start = cp_pos[i];
            int cp_end = (i < cp_count - 1) ? cp_pos[i + 1] : byte_len;
            total_bytes += cp_end - cp_start;
        }
    }
    char *buf = (char *)malloc((size_t)total_bytes + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    int j = 0;
    if (step > 0) {
        for (int i = start; i < stop; i += step) {
            int cp_start = cp_pos[i];
            int cp_end = (i < cp_count - 1) ? cp_pos[i + 1] : byte_len;
            int cp_len = cp_end - cp_start;
            memcpy(buf + j, s->chars + cp_start, (size_t)cp_len);
            j += cp_len;
        }
    } else {
        for (int i = start; i > stop; i += step) {
            int cp_start = cp_pos[i];
            int cp_end = (i < cp_count - 1) ? cp_pos[i + 1] : byte_len;
            int cp_len = cp_end - cp_start;
            memcpy(buf + j, s->chars + cp_start, (size_t)cp_len);
            j += cp_len;
        }
    }
    buf[j] = '\0';
    ObjString *result = new_string(buf, j);
    free(buf);
    free(cp_pos);
    *out = make_obj((Object *)result);
    return true;
}

/* Iteration (6d.2): the core only issues neutral OP_GETITER/OP_FORITER for
 * generic iterables (lists/dicts/strings). Numeric `for i in range(...)` loops
 * are compiled to the dedicated OP_FORLOOP. The
 * frontend builds an opaque iterator plus an int-index state, then yields
 * each element on demand. list/string iterate in place; a dict iterates over
 * a freshly built list of keys (insertion order, like the removed core path). */
static bool py_iterate(VM *vm, Value object, Value *iter, Value *state) {
    if (IS_DICT(object)) {
        ObjDict *dict = (ObjDict *)AS_OBJ(object);
        ObjList *keys = new_list(dict->entry_count);
        if (dict->entries == NULL) {
            for (int i = 0; i < dict->entry_count; i++)
                list_add(keys, dict->inline_entries[i].key);
        } else {
            for (int i = 0; i < dict->order_count; i++) {
                int idx = dict->order[i];
                if (dict->entries[idx].key != EMPTY_VAL && dict->entries[idx].key != TOMBSTONE_VAL)
                    list_add(keys, dict->entries[idx].key);
            }
        }
        *iter = make_obj((Object *)keys);
    } else if (IS_LIST(object) || IS_STRING(object)) {
        *iter = object;
    } else if (IS_OBJ(object) && AS_OBJ(object)) {
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->type_error_class, "object is not iterable");
        return false;
    } else {
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->type_error_class, "cannot iterate null value");
        return false;
    }
    *state = make_int(0);
    return true;
}

static bool py_iter_next(VM *vm, Value iter, Value *state, Value *elem) {
    vm->last_exception = make_null();
    if (!IS_INT(*state)) return false;
    int idx = AS_INT(*state);
    if (IS_LIST(iter)) {
        ObjList *lst = (ObjList *)AS_OBJ(iter);
        if (idx >= lst->count) return false;
        *elem = lst->items ? lst->items[idx] : lst->inline_items[idx];
        *state = make_int(idx + 1);
        return true;
    }
    if (IS_STRING(iter)) {
        ObjString *str = (ObjString *)AS_OBJ(iter);
        if (idx >= str->length) return false;
        char buf[2] = {str->chars[idx], '\0'};
        *elem = make_obj((Object *)new_string(buf, 1));
        *state = make_int(idx + 1);
        return true;
    }
    return false;
}

static bool py_new_list_op(VM *vm, int capacity, Value *out) {
    (void)vm;
    *out = make_obj((Object *)new_list(capacity));
    return true;
}

static bool py_new_dict_op(VM *vm, Value *out) {
    (void)vm;
    *out = make_obj((Object *)new_dict());
    return true;
}

/* The GC write barrier stays in the core (op_listappend calls it after this
 * hook), so this only performs the language-level append + type check. */
static bool py_list_append_op(VM *vm, Value list, Value value) {
    if (!IS_LIST(list)) {
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, "listappend on non-list");
        return false;
    }
    list_add((ObjList *)AS_OBJ(list), value);
    return true;
}

/* Resolve a class name (optionally module.Class) and allocate an instance.
 * Mirrors the removed core OP_NEW class resolution + new_instance. */
static bool py_construct(VM *vm, Value class_name_value, Value *out) {
    ObjString *cls_str = (ObjString *)AS_OBJ(class_name_value);
    const char *cls_name = cls_str->chars;
    Value cls_val;
    char *dot = strchr(cls_name, '.');
    if (dot) {
        char mod_name[128];
        int len = (int)(dot - cls_name);
        if (len >= 128) len = 127;
        memcpy(mod_name, cls_name, len);
        mod_name[len] = '\0';
        Value mod_val;
        if (!vm_get_global(vm, mod_name, &mod_val) || !IS_MODULE(mod_val)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "module '%s' not found for new", mod_name);
            vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, buf);
            return false;
        }
        ObjModule *mod = (ObjModule *)AS_OBJ(mod_val);
        ObjString *field = new_string(dot + 1, (int)strlen(dot + 1));
        cls_val = dict_get(mod->exports, make_obj((Object *)field));
        if (!IS_CLASS(cls_val)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "class '%s' not found in module '%s'", dot + 1, mod_name);
            vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, buf);
            return false;
        }
    } else {
        if (!vm_get_global(vm, cls_name, &cls_val) || !IS_CLASS(cls_val)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "class '%s' not found for new", cls_name);
            vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, buf);
            return false;
        }
    }
    ObjClass *cls = (ObjClass *)AS_OBJ(cls_val);
    *out = make_obj((Object *)new_instance(cls, 4));
    return true;
}

static bool py_instance_of(VM *vm, Value obj, Value cls, bool *result) {
    (void)vm;
    *result = false;
    if (IS_OBJ(obj) && AS_OBJ(obj) && AS_OBJ(obj)->type->kind == OBJ_INSTANCE) {
        ObjClass *obj_class = ((ObjInstance *)AS_OBJ(obj))->klass;
        if (IS_OBJ(cls) && AS_OBJ(cls) && AS_OBJ(cls)->type->kind == OBJ_CLASS) {
            ObjClass *target = (ObjClass *)AS_OBJ(cls);
            while (obj_class) {
                if (obj_class == target) { *result = true; break; }
                obj_class = obj_class->base;
            }
        }
    }
    return true;
}

static bool py_get_field_slot(VM *vm, Value obj, int slot, Value *out) {
    (void)vm;
    if (IS_INSTANCE(obj)) {
        ObjInstance *inst = (ObjInstance *)AS_OBJ(obj);
        if (slot >= 0 && slot < inst->field_count) { *out = inst->fields[slot]; return true; }
    }
    *out = make_null();
    return true;
}

static bool py_set_field_slot(VM *vm, Value obj, int slot, Value value) {
    (void)vm;
    if (IS_INSTANCE(obj)) {
        ObjInstance *inst = (ObjInstance *)AS_OBJ(obj);
        if (slot >= 0 && slot < inst->field_count) {
            inst->fields[slot] = value;
        } else if (slot >= inst->field_count) {
            while (slot >= inst->field_capacity) {
                inst->field_capacity = inst->field_capacity < 4 ? 4 : inst->field_capacity * 2;
                inst->field_names = realloc(inst->field_names, inst->field_capacity * sizeof(char *));
                inst->fields      = realloc(inst->fields,      inst->field_capacity * sizeof(Value));
            }
            while (inst->field_count <= slot) {
                inst->field_names[inst->field_count] = NULL;
                inst->fields[inst->field_count] = make_null();
                inst->field_count++;
            }
            inst->fields[slot] = value;
        }
    }
    return true;
}

/* 6d.5: resolve an invoke callable + self-binding layout. Returns
 * *self_arg = receiver when the callable is a bound method, else null for a
 * static/module function. *callable = null signals an optional-__init__ no-op. */
static bool py_invoke(VM *vm, Value obj, Value name, Value *self_arg, Value *callable) {
    *callable = make_null();
    if (!IS_STRING(name)) {
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, "unknown method");
        return false;
    }
    const char *mname = ((ObjString *)AS_OBJ(name))->chars;

    if (!IS_OBJ(obj) || !AS_OBJ(obj)) {
        /* Immediates (int/float/bool) are virtual objects: methods resolve
         * through their canonical class without boxing. */
        ObjClass *klass = get_class(vm, obj);
        ObjFunction *method = klass ? class_find_method(klass, mname) : NULL;
        if (method) {
            *self_arg = obj;
            *callable = make_obj((Object *)method);
            return true;
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "int/float has no method '%s'", mname);
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, buf);
        return false;
    }

    ObjString *name_obj = (ObjString *)AS_OBJ(name);
    switch (AS_OBJ(obj)->type->kind) {
        case OBJ_CLASS: {
            ObjClass *cls = (ObjClass *)AS_OBJ(obj);
            Value fn_val = cls->fields ? dict_get(cls->fields, make_obj((Object *)name_obj)) : make_null();
            if (IS_FUNCTION(fn_val) || IS_CLOSURE(fn_val)) {
                *self_arg = make_null();
                *callable = fn_val;
                return true;
            }
            char buf[256];
            snprintf(buf, sizeof(buf), "class '%s' has no method '%s'", cls->name, mname);
            vm->last_exception = make_exception_instance(vm, py_fe(vm)->attribute_error_class, buf);
            return false;
        }
        case OBJ_MODULE: {
            ObjModule *mod = (ObjModule *)AS_OBJ(obj);
            Value fn_val = dict_get(mod->exports, make_obj((Object *)name_obj));
            if (IS_FUNCTION(fn_val) || IS_CLOSURE(fn_val)) {
                *self_arg = make_null();
                *callable = fn_val;
                return true;
            }
            char buf[256];
            snprintf(buf, sizeof(buf), "unknown method '%s'", mname);
            vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, buf);
            return false;
        }
        case OBJ_INSTANCE: {
            ObjInstance *inst = (ObjInstance *)AS_OBJ(obj);
            ObjClass *k = inst->klass;
            if (k) {
                for (int i = k->method_count - 1; i >= 0; i--) {
                    ObjFunction *m = k->methods[i];
                    if (m && strcmp(k->method_names[i], mname) == 0) {
                        *self_arg = obj;
                        *callable = make_obj((Object *)m);
                        return true;
                    }
                }
            }
            if (k && strcmp(mname, "__init__") == 0) return true;
            char buf[256];
            snprintf(buf, sizeof(buf), "unknown method '%s'", mname);
            vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, buf);
            return false;
        }
        default: {
            ObjClass *klass = get_class(vm, obj);
            ObjFunction *method = klass ? class_find_method(klass, mname) : NULL;
            if (method) {
                *self_arg = obj;
                *callable = make_obj((Object *)method);
                return true;
            }
            if (klass && strcmp(mname, "__init__") == 0) return true;
            char buf[256];
            snprintf(buf, sizeof(buf), "unknown method '%s'", mname);
            vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, buf);
            return false;
        }
    }
}

/* 6d.5: resolve a super-call over the direct base class (always method layout). */
static bool py_super(VM *vm, Value self, Value name, Value *self_arg, Value *callable) {
    *self_arg = self;
    *callable = make_null();
    if (!IS_STRING(name) || !IS_INSTANCE(self)) return true; /* no-op, like legacy */
    const char *mname = ((ObjString *)AS_OBJ(name))->chars;
    ObjInstance *inst = (ObjInstance *)AS_OBJ(self);
    if (inst->klass && inst->klass->base) {
        ObjClass *base = inst->klass->base;
        for (int i = 0; i < base->method_count; i++) {
            ObjFunction *m = base->methods[i];
            if (m && strcmp(base->method_names[i], mname) == 0) {
                *callable = make_obj((Object *)m);
                return true;
            }
        }
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "unknown super method '%s'", mname);
    vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, buf);
    return false;
}

static bool py_contains(VM *vm, Value needle, Value haystack, bool *found) {
    (void)vm;
    if (IS_LIST(haystack)) *found = list_contains((ObjList *)AS_OBJ(haystack), needle);
    else if (IS_DICT(haystack)) *found = dict_has((ObjDict *)AS_OBJ(haystack), needle);
    else if (IS_STRING(haystack) && IS_STRING(needle))
        *found = strstr(((ObjString *)AS_OBJ(haystack))->chars,
                        ((ObjString *)AS_OBJ(needle))->chars) != NULL;
    else *found = false;
    return true;
}

static bool member_miss(VM *vm, bool safe, void *err_class, const char *msg, Value *out) {
    if (safe) { *out = make_null(); return true; }
    vm->last_exception = make_exception_instance(vm, err_class, msg);
    return false;
}

/* 6d.5: authoritative member lookup. The core op_memberget/op_memberget_safe
 * delegate entirely here; the legacy switch previously lived in the opcode. */
/* Resolve a native method of a canonical class for any receiver (heap or
 * immediate). Zero-arg natives compute eagerly (property-style access, as
 * str/list already do); the rest bind as a method. */
static bool py_resolve_class_method(VM *vm, Value object, ObjClass *klass,
                                    const char *name, bool safe, Value *out) {
    ObjFunction *mfn = klass ? class_find_method(klass, name) : NULL;
    if (!mfn) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s has no attribute '%s'",
                 klass && klass->name ? klass->name : "object", name);
        return member_miss(vm, safe, py_fe(vm)->attribute_error_class, buf, out);
    }
    if (mfn->is_native && mfn->param_count == 0) {
        Value result;
        Value native_args[1] = { object };
        if (!vm_call_native(vm, mfn->native_fn, native_args, 1, &result))
            return member_miss(vm, safe, py_fe(vm)->attribute_error_class,
                               "native method failed", out);
        *out = result;
        return true;
    }
    *out = make_obj((Object *)new_bound_method(object, mfn));
    return true;
}

static bool py_member_get(VM *vm, Value object, Value name, bool safe, Value *out) {
    if (!IS_STRING(name)) {
        if (safe) { *out = make_null(); return true; }
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->attribute_error_class, "cannot access member of null value");
        return false;
    }
    char *chars = ((ObjString *)AS_OBJ(name))->chars;

    if (!IS_OBJ(object) || !AS_OBJ(object)) {
        /* Immediates (int/float/bool) are virtual objects: attributes resolve
         * through their canonical class without boxing. */
        ObjClass *klass = get_class(vm, object);
        if (strcmp(chars, "__class__") == 0) {
            if (!klass) return member_miss(vm, safe, py_fe(vm)->attribute_error_class,
                                           "value has no class", out);
            *out = make_obj((Object *)klass);
            return true;
        }
        if (!klass) {
            if (safe) { *out = make_null(); return true; }
            vm->last_exception = make_exception_instance(vm, py_fe(vm)->attribute_error_class, "cannot access member of null value");
            return false;
        }
        return py_resolve_class_method(vm, object, klass, chars, safe, out);
    }

    Type *type = AS_OBJ(object)->type;
    if (type && type->getattr) {
        Value v = type->getattr(vm, object, chars);
        if (!IS_NIL(v)) { *out = v; return true; }
    }
    switch (type->kind) {
        case OBJ_INSTANCE: {
            ObjInstance *inst = (ObjInstance *)AS_OBJ(object);
            int fi = -1;
            if (inst->klass && inst->klass->field_slot_map) {
                Value slot_v = dict_get(inst->klass->field_slot_map, make_obj((Object *)name));
                if (IS_INT(slot_v)) fi = AS_INT(slot_v);
            }
            if (fi >= 0) { *out = inst->fields[fi]; return true; }
            int mi = -1;
            if (inst->klass) {
                for (int i = 0; i < inst->klass->method_count; i++)
                    if (strcmp(inst->klass->method_names[i], chars) == 0) { mi = i; break; }
            }
            if (mi >= 0) {
                *out = make_obj((Object *)new_bound_method(object, inst->klass->methods[mi]));
                return true;
            }
            char buf[256];
            snprintf(buf, sizeof(buf), "instance has no field or method '%s'", chars);
            return member_miss(vm, safe, py_fe(vm)->attribute_error_class, buf, out);
        }
        case OBJ_MODULE: {
            ObjModule *mod = (ObjModule *)AS_OBJ(object);
            Value export_val = dict_get(mod->exports, make_obj((Object *)name));
            if (!IS_NIL(export_val)) { *out = export_val; return true; }
            char buf[256];
            snprintf(buf, sizeof(buf), "module '%s' has no attribute '%s'", mod->name->chars, chars);
            return member_miss(vm, safe, py_fe(vm)->attribute_error_class, buf, out);
        }
        case OBJ_DICT: {
            ObjClass *dklass = py_fe(vm)->dict_class;
            ObjFunction *mfn = dklass ? class_find_method(dklass, chars) : NULL;
            if (mfn) {
                *out = make_obj((Object *)new_bound_method(object, mfn));
                return true;
            }
            Value val = dict_get((ObjDict *)AS_OBJ(object), make_obj((Object *)name));
            if (!IS_NIL(val)) { *out = val; return true; }
            char buf[256];
            snprintf(buf, sizeof(buf), "'%s' not found in dict", chars);
            return member_miss(vm, safe, py_fe(vm)->key_error_class, buf, out);
        }
        case OBJ_STRING:
        case OBJ_LIST: {
            ObjClass *klass = get_class(vm, object);
            ObjFunction *mfn = klass ? class_find_method(klass, chars) : NULL;
            if (mfn) {
                if (mfn->is_native && mfn->param_count == 0) {
                    Value result;
                    Value native_args[1] = { object };
                    if (!vm_call_native(vm, mfn->native_fn, native_args, 1, &result))
                        return member_miss(vm, safe, py_fe(vm)->attribute_error_class, type->name, out);
                    *out = result;
                    return true;
                }
                *out = make_obj((Object *)new_bound_method(object, mfn));
                return true;
            }
            char buf[256];
            snprintf(buf, sizeof(buf), "%s has no field '%s'", type->name, chars);
            return member_miss(vm, safe, py_fe(vm)->attribute_error_class, buf, out);
        }
        case OBJ_ENUM: {
            ObjEnum *e = (ObjEnum *)AS_OBJ(object);
            int fi = -1;
            for (int i = 0; i < e->count; i++)
                if (strcmp(e->names[i], chars) == 0) { fi = i; break; }
            if (fi >= 0) { *out = make_int(e->values[fi]); return true; }
            char buf[256];
            snprintf(buf, sizeof(buf), "enum has no variant '%s'", chars);
            return member_miss(vm, safe, py_fe(vm)->attribute_error_class, buf, out);
        }
        case OBJ_CLASS: {
            ObjClass *cls = (ObjClass *)AS_OBJ(object);
            if (strcmp(chars, "name") == 0) {
                *out = make_obj((Object *)new_string(cls->name, (int)strlen(cls->name)));
                return true;
            }
            if (strcmp(chars, "base") == 0) {
                *out = cls->base ? make_obj((Object *)cls->base) : make_null();
                return true;
            }
            if (cls->fields) {
                Value val = dict_get(cls->fields, make_obj((Object *)name));
                if (!IS_NIL(val)) { *out = val; return true; }
            }
            char buf[256];
            snprintf(buf, sizeof(buf), "class '%s' has no field '%s'", cls->name, chars);
            return member_miss(vm, safe, py_fe(vm)->attribute_error_class, buf, out);
        }
        case OBJ_VECTOR: {
            ObjVector *vec = (ObjVector *)AS_OBJ(object);
            if (((ObjString *)AS_OBJ(name))->length == 1) {
                float v = 0.0f;
                char c = chars[0];
                if (c == 'x') v = vec->data[0];
                else if (c == 'y') v = vec->data[1];
                else if (c == 'z') v = vec->data[2];
                else if (c == 'w') v = vec->data[3];
                else return member_miss(vm, safe, py_fe(vm)->attribute_error_class, "vector has no field", out);
                *out = make_double((double)v);
                return true;
            }
            return member_miss(vm, safe, py_fe(vm)->attribute_error_class, "vector has no field", out);
        }
        default:
            return member_miss(vm, safe, py_fe(vm)->attribute_error_class, "value has no fields", out);
    }
}

/* 6d.5: authoritative member set. Mirrors the legacy OP_MEMBERSET switch. */
static bool py_member_set(VM *vm, Value object, Value name, Value value) {
    if (!IS_STRING(name)) return false;
    const char *chars = ((ObjString *)AS_OBJ(name))->chars;
    if (IS_INSTANCE(object)) {
        ObjInstance *inst = (ObjInstance *)AS_OBJ(object);
        int fi = -1;
        if (inst->klass && inst->klass->field_slot_map) {
            Value slot_v = dict_get(inst->klass->field_slot_map, make_obj((Object *)name));
            if (IS_INT(slot_v)) fi = AS_INT(slot_v);
        }
        if (fi < 0) {
            for (int i = 0; i < inst->field_count; i++)
                if (strcmp(inst->field_names[i], chars) == 0) { fi = i; break; }
        }
        if (fi >= 0) { inst->fields[fi] = value; return true; }
        instance_set_field(inst, chars, value);
        return true;
    }
    if (IS_VECTOR(object)) {
        ObjVector *vec = (ObjVector *)AS_OBJ(object);
        float v = (float)value_to_double(value);
        if (((ObjString *)AS_OBJ(name))->length == 1) {
            char c = chars[0];
            if (c == 'x') vec->data[0] = v;
            else if (c == 'y') vec->data[1] = v;
            else if (c == 'z') vec->data[2] = v;
            else if (c == 'w') vec->data[3] = v;
            else {
                vm->last_exception = make_exception_instance(vm, py_fe(vm)->attribute_error_class, "vector has no field");
                return false;
            }
            return true;
        }
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->attribute_error_class, "vector has no field");
        return false;
    }
    vm->last_exception = make_exception_instance(vm, py_fe(vm)->attribute_error_class, "cannot set field on this type");
    return false;
}

static bool py_import_module(VM *vm, Value module_name, const char *from_path, Value *out) {
    if (!IS_STRING(module_name)) {
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class,
            "import: module name must be a string");
        return false;
    }
    const char *name = ((ObjString *)AS_OBJ(module_name))->chars;
    Value cached = dict_get(py_fe(vm)->module_cache, module_name);
    if (!IS_NIL(cached) && IS_MODULE(cached)) { *out = cached; return true; }

    char *path = module_resolve_path(name, from_path && from_path[0] ? from_path : NULL);
    if (!path) {
        char message[256]; snprintf(message, sizeof(message), "module '%s' not found", name);
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, message);
        return false;
    }
    char *source = module_read_source(path);
    if (!source) {
        free(path);
        char message[256]; snprintf(message, sizeof(message), "cannot read module '%s'", name);
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, message);
        return false;
    }
    Lexer *lexer = lexer_new(source);
    TokenList *tokens = lexer_tokenize(lexer);
    Parser *parser = parser_new(tokens, source, path);
    Program *program = parser_parse(parser);
    if (parser->had_error) {
        parser_free(parser); token_list_free(tokens); lexer_free(lexer); free(source); free(path);
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, "import: parser errors");
        return false;
    }
    GlobalEntry **saved = vm_globals_save(vm);
    vm_globals_fresh(vm);
    Chunk chunk;
    bool compiled = compile_program(program, &chunk, vm, false, true);
    parser_free(parser); token_list_free(tokens); lexer_free(lexer); free(source);
    if (!compiled) {
        vm_globals_restore(vm, saved); free(path);
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->exception_class, "import: compilation errors");
        return false;
    }
    ObjFunction *function = new_function(name);
    function->chunk = malloc(sizeof(Chunk));
    *function->chunk = chunk;
    function->chunk->source_path = path;
    Value ignored;
    VMResult result = vm_call_value(vm, make_obj((Object *)function), NULL, 0, &ignored);
    if (result != VM_OK) {
        vm_globals_restore(vm, saved);
        return false;
    }
    ObjModule *module = new_module(name);
    module->exports = (ObjDict *)vm_globals_to_dict(vm);
    vm_globals_restore(vm, saved);
    *out = make_obj((Object *)module);
    dict_set(py_fe(vm)->module_cache, make_obj((Object *)new_string(name, (int)strlen(name))), *out);
    return true;
}

static Value py_make_exception(VM *vm, VMExceptionKind kind, const char *message) {
    void *klass = py_fe(vm)->exception_class;
    switch (kind) {
        case VM_EXCEPTION_TYPE: klass = py_fe(vm)->type_error_class; break;
        case VM_EXCEPTION_KEY: klass = py_fe(vm)->key_error_class; break;
        case VM_EXCEPTION_INDEX: klass = py_fe(vm)->index_error_class; break;
        case VM_EXCEPTION_ATTRIBUTE: klass = py_fe(vm)->attribute_error_class; break;
        case VM_EXCEPTION_VALUE: klass = py_fe(vm)->value_error_class; break;
        case VM_EXCEPTION_RUNTIME: klass = py_fe(vm)->runtime_error_class; break;
        case VM_EXCEPTION_ARGUMENT: klass = py_fe(vm)->argument_error_class; break;
        default: break;
    }
    return make_exception_instance(vm, klass, message);
}

/* Build an exception from an explicit frontend class object (used by
 * luna_throw, which native functions call with the class they were given). */
static Value py_make_exception_for_class(VM *vm, void *cls, const char *message) {
    return make_exception_instance(vm, cls, message);
}

/* Build a string constant object for the chunk constant pool. */
static Value py_new_string(VM *vm, const char *chars, int length) {
    (void)vm;
    return make_obj((Object *)new_string(chars, length));
}

/* Open-upvalue list management (the core owns the void* head, we own the
 * ObjUpvalue objects and their linkage). */
static Object *py_capture_upvalue(VM *vm, int stack_idx) {
    ObjUpvalue *prev = NULL;
    ObjUpvalue *uv = (ObjUpvalue*)vm->open_upvalues;
    while (uv && uv->stack_index > stack_idx) {
        prev = uv;
        uv = uv->next;
    }
    if (uv && uv->is_open && uv->stack_index == stack_idx) return (Object*)uv;

    ObjUpvalue *created = new_upvalue(stack_idx);
    created->next = uv;
    created->frame_depth = vm->frame_count;
    if (prev) prev->next = created;
    else vm->open_upvalues = (Object*)created;
    return (Object*)created;
}

static void py_close_upvalues(VM *vm, int frame_depth) {
    while (vm->open_upvalues && ((ObjUpvalue*)vm->open_upvalues)->frame_depth >= frame_depth) {
        ObjUpvalue *uv = (ObjUpvalue*)vm->open_upvalues;
        uv->closed = vm->stack[uv->stack_index];
        uv->is_open = false;
        vm->open_upvalues = uv->next;
    }
}

/* ---- Frontend object-model queries (used by the core via VMFrontendHooks) ----
 * The core must not switch on inline object kinds, so it asks the frontend
 * whether a value is a string / instance through these hooks. */

static bool py_fe_is_string(VM *vm, Value v) {
    (void)vm;
    return IS_STRING(v);
}

static bool py_fe_is_instance(VM *vm, Value v) {
    (void)vm;
    return IS_INSTANCE(v);
}

/* OP_CLOSURE delegates the whole closure construction here.  The frontend owns
 * the closure layout and upvalue capture; the core only supplies the function
 * constant and the current frame.  Mirrors the old inline OP_CLOSURE body. */
static Object *py_fe_new_closure(VM *vm, Value fn_val) {
    if (!IS_FUNCTION(fn_val)) {
        vm->last_exception = vm_make_exception(vm, VM_EXCEPTION_GENERIC,
                                              "CLOSURE needs function constant");
        return NULL;
    }
    ObjFunction *fn = (ObjFunction *)AS_OBJ(fn_val);
    ObjClosure *cl = new_closure(fn);
    CallFrame *frame = &vm->frames[vm->frame_count - 1];
    int base = frame->base;
    for (int i = 0; i < fn->upvalue_count; i++) {
        uint8_t is_local = fn->upvalue_descriptors[i].is_local;
        uint8_t index = fn->upvalue_descriptors[i].index;
        if (is_local) {
            cl->upvalues[i] = (ObjUpvalue *)capture_upvalue(vm, base + index);
        } else {
            Object *closure_obj = frame->closure;
            Value uvref = (closure_obj && closure_obj->type->get_upvalue_ref)
                ? closure_obj->type->get_upvalue_ref(make_obj(closure_obj), index)
                : make_null();
            cl->upvalues[i] = is_null(uvref) ? NULL : (ObjUpvalue *)AS_OBJ(uvref);
        }
    }
    return (Object *)cl;
}

static const VMFrontendHooks py_frontend_hooks = {
    .mark_roots = py_mark_roots,
    .capture_upvalue = py_capture_upvalue,
    .close_upvalues = py_close_upvalues,
    .unary = py_unary_operation,
    .binary = py_binary_operation,
    .compare = py_compare_operation,
    .getitem = py_index_get,
    .setitem = py_index_set,
    .getattr = py_member_get,
    .setattr = py_member_set,
    .slice = py_slice,
    .iterate = py_iterate,
    .iter_next = py_iter_next,
    .new_list = py_new_list_op,
    .new_dict = py_new_dict_op,
    .new_string = py_new_string,
    .list_append = py_list_append_op,
    .construct = py_construct,
    .instance_of = py_instance_of,
    .get_field_slot = py_get_field_slot,
    .set_field_slot = py_set_field_slot,
    .invoke = py_invoke,
    .super_fn = py_super,
    .contains = py_contains,
    .import_module = py_import_module,
    .make_exception = py_make_exception,
    .make_exception_for_class = py_make_exception_for_class,
    .is_string = py_fe_is_string,
    .is_instance = py_fe_is_instance,
    .new_closure = py_fe_new_closure
};

/* Luna's language runtime bootstrap lives in the frontend, not in vm.c. */
extern void vm_register_builtins(VM *vm);
extern void vm_register_canonical_classes(VM *vm);
extern void vm_register_math_module(VM *vm);
extern void vm_register_random_module(VM *vm);
extern void vm_register_noise_module(VM *vm);
extern void vm_register_io_module(VM *vm);
extern void vm_register_time_module(VM *vm);
extern void vm_register_os_module(VM *vm);
extern void vm_register_buffer_module(VM *vm);
extern void vm_register_string_module(VM *vm);
extern void vm_register_net_module(VM *vm);
extern void vm_register_json_module(VM *vm);
extern uint64_t py_time_monotonic_us(void);

static ObjClass *py_register_exception(VM *vm, const char *name, ObjClass *base) {
    ObjClass *cls = new_class(name, NULL);
    if (base) {
        cls->base = base;
        if (base->prototype) {
            ObjInstance *bp = base->prototype;
            cls->prototype = new_instance(cls, bp->field_capacity > 4 ? bp->field_capacity : 4);
            for (int i = 0; i < bp->field_count; i++) {
                instance_set_field(cls->prototype, bp->field_names[i], bp->fields[i]);
            }
        }
    } else {
        cls->prototype = new_instance(cls, 4);
        instance_set_field(cls->prototype, "message", make_null());
        instance_set_field(cls->prototype, "stack_trace", make_null());
        instance_set_field(cls->prototype, "file", make_null());
        instance_set_field(cls->prototype, "line", make_int(0));
    }
    vm_set_global(vm, name, make_obj((Object*)cls), false);
    return cls;
}

/* Native __init__ so `RuntimeError("msg")` (and any subclass) builds a proper
 * exception instance with its message field set from the first argument. */
static Value py_exc_init(VM *vm, Value *args, int n) {
    (void)vm;
    if (n < 1 || !IS_INSTANCE(args[0])) return make_null();
    instance_set_field((ObjInstance*)AS_OBJ(args[0]), "message",
                       (n > 1) ? args[1] : make_null());
    return make_null();
}

void py_init_vm(VM *vm) {
    vm->time_start_us = py_time_monotonic_us();
    vm->frontend_data = calloc(1, sizeof(PyFrontend));
    if (!vm->frontend_data) { fprintf(stderr, "Out of memory\n"); exit(1); }
    PyFrontend *fe = py_fe(vm);
    vm_register_builtins(vm);
    vm_register_canonical_classes(vm);
    py_register_number_methods(vm);

    fe->exception_class = py_register_exception(vm, "Exception", NULL);
    fe->type_error_class = py_register_exception(vm, "TypeError", fe->exception_class);
    fe->key_error_class = py_register_exception(vm, "KeyError", fe->exception_class);
    fe->index_error_class = py_register_exception(vm, "IndexError", fe->exception_class);
    fe->attribute_error_class = py_register_exception(vm, "AttributeError", fe->exception_class);
    fe->value_error_class = py_register_exception(vm, "ValueError", fe->exception_class);
    fe->runtime_error_class = py_register_exception(vm, "RuntimeError", fe->exception_class);
    fe->argument_error_class = py_register_exception(vm, "ArgumentError", fe->exception_class);

    {
        ObjClass *exc_classes[] = {
            fe->exception_class, fe->type_error_class, fe->key_error_class,
            fe->index_error_class, fe->attribute_error_class,
            fe->value_error_class, fe->runtime_error_class,
            fe->argument_error_class
        };
        for (size_t i = 0; i < sizeof(exc_classes) / sizeof(exc_classes[0]); i++)
            class_add_native_method(exc_classes[i], "__init__", py_exc_init);
    }

    fe->module_cache = new_dict();
    vm_register_math_module(vm);
    vm_register_random_module(vm);
    vm_register_noise_module(vm);
    vm_register_io_module(vm);
    vm_register_time_module(vm);
    vm_register_os_module(vm);
    vm_register_buffer_module(vm);
    vm_register_string_module(vm);
    vm_register_net_module(vm);
    vm_register_json_module(vm);
}

/* ============================================================ */
/* Object bridge (FrontendObject) + source compiler              */
/* ============================================================ */

static int py_type_of(Value v) {
    if (IS_NIL(v)) return LUNA_TNIL;
    if (IS_BOOL(v)) return LUNA_TBOOLEAN;
    if (IS_INT(v)) return LUNA_TINTEGER;
    if (IS_INT64(v)) return LUNA_TINTEGER;
    if (IS_DOUBLE(v)) return LUNA_TNUMBER;

    Object *obj = AS_OBJ(v);
    switch (obj->type->kind) {
        case OBJ_STRING:   return LUNA_TSTRING;
        case OBJ_FUNCTION: return LUNA_TFUNCTION;
        case OBJ_CLOSURE:  return LUNA_TFUNCTION;
        case OBJ_CLASS:    return LUNA_TCLASS;
        case OBJ_INSTANCE: return LUNA_TINSTANCE;
        case OBJ_DICT:     return LUNA_TTABLE;
        case OBJ_LIST:     return LUNA_TTABLE;
        case OBJ_USERDATA: return LUNA_TUSERDATA;
        case OBJ_VECTOR:   return LUNA_TVECTOR;
        case OBJ_MATRIX:   return LUNA_TMATRIX;
        case OBJ_BUFFER:   return LUNA_TBUFFER;
        case OBJ_INT64:    return LUNA_TINTEGER;
        default:           return LUNA_TNIL;
    }
}

static bool py_is_cfunction(Value v) {
    if (!IS_OBJ(v)) return false;
    Object *obj = AS_OBJ(v);
    if (obj->type->kind != OBJ_FUNCTION) return false;
    return ((ObjFunction *)obj)->cfunc != NULL;
}

static Value py_new_cfunction(api_CFunction fn) {
    ObjFunction *f = new_native_function("<cfunc>", NULL);
    f->cfunc = fn;
    return make_obj((Object *)f);
}

static const char *py_cstring(Value v, size_t *len) {
    if (!IS_OBJ(v)) { if (len) *len = 0; return NULL; }
    ObjString *s = (ObjString *)AS_OBJ(v);
    if (len) *len = (size_t)s->length;
    return s->chars;
}

static Value py_new_userdata(void *data, const char *tag, void (*finalizer)(void *)) {
    return make_obj((Object *)new_userdata_tagged(tag, data, finalizer));
}

static void *py_userdata_data(Value v) {
    return ((ObjUserdata *)AS_OBJ(v))->data;
}

static const char *py_userdata_tag(Value v) {
    return ((ObjUserdata *)AS_OBJ(v))->tag;
}

static int64_t py_int64_value(Value v) {
    return ((ObjInt64 *)AS_OBJ(v))->value;
}

static Value py_make_int64(int64_t n) {
    return make_int64(n);
}

static const FrontendObject py_frontend_object = {
    .type_of = py_type_of,
    .new_cfunction = py_new_cfunction,
    .is_cfunction = py_is_cfunction,
    .cstring = py_cstring,
    .new_userdata = py_new_userdata,
    .userdata_data = py_userdata_data,
    .userdata_tag = py_userdata_tag,
    .int64_value = py_int64_value,
    .make_int64 = py_make_int64,
};

static const char *py_compile_source(VM *vm, const char *source, const char *path, bool is_repl, Value *out_fn) {
    static char errbuf[256];
    Lexer *lexer = lexer_new(source);
    if (!lexer) return "out of memory";
    TokenList *tokens = lexer_tokenize(lexer);
    if (!tokens) { lexer_free(lexer); return "out of memory"; }

    Token *t = tokens->head;
    while (t) {
        if (t->type == TOK_ERROR) {
            snprintf(errbuf, sizeof(errbuf), "lexer error: %s", t->value ? t->value : "tokenize error");
            token_list_free(tokens); lexer_free(lexer);
            return errbuf;
        }
        t = t->next;
    }

    Parser *parser = parser_new(tokens, source, path ? path : "<string>");
    if (!parser) { token_list_free(tokens); lexer_free(lexer); return "out of memory"; }
    Program *program = parser_parse(parser);
    if (!program || parser->had_error) {
        parser_free(parser); token_list_free(tokens); lexer_free(lexer);
        return "parse error";
    }

    Chunk *chunk = (Chunk *)calloc(1, sizeof(Chunk));
    if (!chunk) { free_program(program); parser_free(parser); token_list_free(tokens); lexer_free(lexer); return "out of memory"; }
    if (!compile_program(program, chunk, vm, is_repl, false)) {
        chunk_free(chunk); free(chunk);
        free_program(program); parser_free(parser); token_list_free(tokens); lexer_free(lexer);
        return "compile error";
    }
    if (path) chunk->source_path = strdup(path);

    ObjFunction *fn = new_function("<load>");
    fn->chunk = chunk;
    fn->param_count = 0;
    ObjClosure *cl = new_closure(fn);
    *out_fn = make_obj((Object *)cl);

    free_program(program);
    parser_free(parser);
    token_list_free(tokens);
    lexer_free(lexer);
    return NULL;
}

const FrontendDef g_frontend = {
    .name = "python",
    .init_vm = py_init_vm,
    .wire_lifecycle = py_wire_lifecycle,
    .hooks = &py_frontend_hooks,
    .object = &py_frontend_object,
    .compile_source = py_compile_source,
};
