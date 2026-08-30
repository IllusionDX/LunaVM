/* luna.c — Embeddable C API implementation for the Luna interpreter. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "luna.h"
#include "vm.h"
#include "value.h"
#include "luna/object.h"
#include "chunk.h"
#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include "ast.h"
#include "module.h"

/* ============================================================ */
/* Internal state                                                */
/* ============================================================ */

#define LUNA_API_STACK_INIT 64

struct luna_State {
    VM    *vm;
    Value *stack;
    int    cap;
    int    top;
};

/* ============================================================ */
/* GC hook — called by vm.c's mark_and_sweep                    */
/* ============================================================ */

void luna_mark_roots(VM *vm) {
    luna_State *L = (luna_State *)vm->api_state;
    if (L) {
        for (int i = 0; i < L->top; i++) {
            vm_mark_value(vm, L->stack[i]);
        }
    }
    /* All language-owned roots live behind the frontend hook.  The core must
     * not know which canonical classes or module cache a language uses. */
    void *roots[] = {
        vm->module_cache, vm->exception_class, vm->type_error_class,
        vm->key_error_class, vm->index_error_class, vm->attribute_error_class,
        vm->value_error_class, vm->runtime_error_class, vm->argument_error_class,
        vm->string_class, vm->list_class, vm->dict_class, vm->enum_class,
        vm->buffer_class, vm->vector_class, vm->matrix_class, vm->function_class,
        vm->closure_class, vm->bound_method_class, vm->class_class,
        vm->module_class, vm->userdata_class
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

static double luna_to_f64(Value v) {
    if (IS_INT(v)) return (double)AS_INT(v);
    if (IS_INT64(v)) return (double)((ObjInt64 *)AS_OBJ(v))->value;
    return IS_DOUBLE(v) ? AS_DOUBLE(v) : 0.0;
}

static int64_t luna_to_i64(Value v) {
    if (IS_INT(v)) return (int64_t)AS_INT(v);
    if (IS_INT64(v)) return ((ObjInt64 *)AS_OBJ(v))->value;
    return IS_DOUBLE(v) ? (int64_t)AS_DOUBLE(v) : 0;
}

static bool luna_value_is_integer(Value v) { return IS_INT(v) || IS_INT64(v); }

static Value luna_integer_result(int64_t value) {
    return value >= INT32_MIN && value <= INT32_MAX
        ? make_int((int32_t)value) : make_int64(value);
}

static MOP_Bin luna_binary_method(Value self, VMOperation op) {
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

static bool luna_unary_operation(VM *vm, VMOperation op, Value operand, Value *out) {
    if (op == VM_OP_NEG && IS_OBJ(operand) && AS_OBJ(operand)->type &&
        AS_OBJ(operand)->type->neg) {
        *out = AS_OBJ(operand)->type->neg(vm, operand);
        return true;
    }
    if (op == VM_OP_NEG && luna_value_is_integer(operand)) {
        *out = luna_integer_result(-luna_to_i64(operand));
        return true;
    }
    if (op == VM_OP_NEG && IS_DOUBLE(operand)) {
        *out = make_double(-AS_DOUBLE(operand));
        return true;
    }
    if (op == VM_OP_BNOT && luna_value_is_integer(operand)) {
        *out = luna_integer_result(~luna_to_i64(operand));
        return true;
    }
    return false;
}

static bool luna_binary_operation(VM *vm, VMOperation op, Value left, Value right, Value *out) {
    MOP_Bin method = luna_binary_method(left, op);
    if (method) { *out = method(vm, left, right); return !IS_NIL(*out); }
    if ((op == VM_OP_ADD || op == VM_OP_MUL) && (method = luna_binary_method(right, op))) {
        *out = method(vm, right, left);
        return !IS_NIL(*out);
    }
    if (!IS_NUMBER(left) || !IS_NUMBER(right)) return false;
    if (luna_value_is_integer(left) && luna_value_is_integer(right)) {
        int64_t a = luna_to_i64(left), b = luna_to_i64(right);
        switch (op) {
            case VM_OP_ADD: *out = luna_integer_result(a + b); return true;
            case VM_OP_SUB: *out = luna_integer_result(a - b); return true;
            case VM_OP_MUL: *out = luna_integer_result(a * b); return true;
            case VM_OP_DIV:
                if (b) { *out = luna_integer_result(a / b); return true; }
                vm->last_exception = make_exception_instance(vm, vm->runtime_error_class, "division by zero");
                return false;
            case VM_OP_MOD:
                if (b) { *out = luna_integer_result(a % b); return true; }
                vm->last_exception = make_exception_instance(vm, vm->exception_class, "mod/0");
                return false;
            case VM_OP_BAND: *out = luna_integer_result(a & b); return true;
            case VM_OP_BOR:  *out = luna_integer_result(a | b); return true;
            case VM_OP_BXOR: *out = luna_integer_result(a ^ b); return true;
            case VM_OP_SHL:  *out = luna_integer_result(a << b); return true;
            case VM_OP_SHR:  *out = luna_integer_result(a >> b); return true;
            default: break;
        }
    }
    double a = luna_to_f64(left), b = luna_to_f64(right);
    switch (op) {
        case VM_OP_ADD: *out = make_double(a + b); return true;
        case VM_OP_SUB: *out = make_double(a - b); return true;
        case VM_OP_MUL: *out = make_double(a * b); return true;
        case VM_OP_DIV: *out = b == 0.0 ? (a == 0.0 ? make_double(0.0 / 0.0) :
            (a > 0.0 ? make_pos_inf() : make_neg_inf())) : make_double(a / b); return true;
        case VM_OP_MOD: *out = b == 0.0 ? make_double(0.0 / 0.0) : make_double(fmod(a, b)); return true;
        default: return false;
    }
}

static bool luna_compare_operation(VM *vm, VMOperation op, Value left, Value right, Value *out) {
    (void)vm;
    if (luna_value_is_integer(left) && luna_value_is_integer(right)) {
        int64_t a = luna_to_i64(left), b = luna_to_i64(right);
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
        double a = luna_to_f64(left), b = luna_to_f64(right);
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

static bool luna_index_get(VM *vm, Value object, Value key, bool safe, Value *out) {
    if (!IS_OBJ(object) || !AS_OBJ(object)) {
        if (safe) { *out = make_null(); return true; }
        vm->last_exception = make_exception_instance(vm, vm->type_error_class, "cannot index null value");
        return false;
    }
    Type *type = AS_OBJ(object)->type;
    if (!type || !type->getitem) {
        if (safe) { *out = make_null(); return true; }
        vm->last_exception = make_exception_instance(vm, vm->type_error_class, "value is not indexable");
        return false;
    }
    if (IS_LIST(object) && !IS_INT(key)) {
        if (safe) { *out = make_null(); return true; }
        vm->last_exception = make_exception_instance(vm, vm->type_error_class, "list index must be integer");
        return false;
    }
    if (IS_STRING(object) && !IS_INT(key)) {
        if (safe) { *out = make_null(); return true; }
        vm->last_exception = make_exception_instance(vm, vm->type_error_class, "string index must be integer");
        return false;
    }
    *out = type->getitem(vm, object, key);
    if (!safe && IS_DICT(object) && !dict_has((ObjDict *)AS_OBJ(object), key)) {
        char *ks = value_to_string(key);
        char message[256];
        snprintf(message, sizeof(message), "%s not found in dict", ks);
        free(ks);
        vm->last_exception = make_exception_instance(vm, vm->key_error_class, message);
        return false;
    }
    if (!safe && IS_LIST(object)) {
        ObjList *list = (ObjList *)AS_OBJ(object);
        int index = AS_INT(key); if (index < 0) index += list->count;
        if (index < 0 || index >= list->count) {
            vm->last_exception = make_exception_instance(vm, vm->index_error_class, "list index out of bounds");
            return false;
        }
    }
    if (!safe && IS_STRING(object)) {
        ObjString *string = (ObjString *)AS_OBJ(object);
        int index = AS_INT(key); if (index < 0) index += string->length;
        if (index < 0 || index >= string->length) {
            vm->last_exception = make_exception_instance(vm, vm->index_error_class, "string index out of bounds");
            return false;
        }
    }
    return true;
}

static bool luna_index_set(VM *vm, Value object, Value key, Value value) {
    if (!IS_OBJ(object) || !AS_OBJ(object) || !AS_OBJ(object)->type ||
        !AS_OBJ(object)->type->setitem) {
        vm->last_exception = make_exception_instance(vm, vm->type_error_class, "invalid index assignment");
        return false;
    }
    if (IS_LIST(object) && !IS_INT(key)) {
        vm->last_exception = make_exception_instance(vm, vm->type_error_class, "list index must be integer");
        return false;
    }
    if (IS_LIST(object)) {
        ObjList *list = (ObjList *)AS_OBJ(object);
        int index = AS_INT(key);
        if (index < 0) index += list->count;
        if (index < 0 || index >= list->count) {
            char message[128];
            snprintf(message, sizeof(message), "list index out of bounds (index: %d, size: %d)", index, list->count);
            vm->last_exception = make_exception_instance(vm, vm->index_error_class, message);
            return false;
        }
    }
    AS_OBJ(object)->type->setitem(vm, object, key, value);
    return true;
}

/* Slicing semantics live in the frontend: the core only knows the neutral
 * OP_SLICE opcode. Non-indexable objects (including null)
 * yield null, matching the previous core behaviour. */
static bool luna_slice(VM *vm, Value object, Value start_val, Value stop_val,
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
static bool luna_iterate(VM *vm, Value object, Value *iter, Value *state) {
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
        vm->last_exception = make_exception_instance(vm, vm->type_error_class, "object is not iterable");
        return false;
    } else {
        vm->last_exception = make_exception_instance(vm, vm->type_error_class, "cannot iterate null value");
        return false;
    }
    *state = make_int(0);
    return true;
}

static bool luna_iter_next(VM *vm, Value iter, Value *state, Value *elem) {
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

static bool luna_new_list_op(VM *vm, int capacity, Value *out) {
    (void)vm;
    *out = make_obj((Object *)new_list(capacity));
    return true;
}

static bool luna_new_dict_op(VM *vm, Value *out) {
    (void)vm;
    *out = make_obj((Object *)new_dict());
    return true;
}

/* The GC write barrier stays in the core (op_listappend calls it after this
 * hook), so this only performs the language-level append + type check. */
static bool luna_list_append_op(VM *vm, Value list, Value value) {
    if (!IS_LIST(list)) {
        vm->last_exception = make_exception_instance(vm, vm->exception_class, "listappend on non-list");
        return false;
    }
    list_add((ObjList *)AS_OBJ(list), value);
    return true;
}

/* Resolve a class name (optionally module.Class) and allocate an instance.
 * Mirrors the removed core OP_NEW class resolution + new_instance. */
static bool luna_construct(VM *vm, Value class_name_value, Value *out) {
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
            vm->last_exception = make_exception_instance(vm, vm->exception_class, buf);
            return false;
        }
        ObjModule *mod = (ObjModule *)AS_OBJ(mod_val);
        ObjString *field = new_string(dot + 1, (int)strlen(dot + 1));
        cls_val = dict_get(mod->exports, make_obj((Object *)field));
        if (!IS_CLASS(cls_val)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "class '%s' not found in module '%s'", dot + 1, mod_name);
            vm->last_exception = make_exception_instance(vm, vm->exception_class, buf);
            return false;
        }
    } else {
        if (!vm_get_global(vm, cls_name, &cls_val) || !IS_CLASS(cls_val)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "class '%s' not found for new", cls_name);
            vm->last_exception = make_exception_instance(vm, vm->exception_class, buf);
            return false;
        }
    }
    ObjClass *cls = (ObjClass *)AS_OBJ(cls_val);
    *out = make_obj((Object *)new_instance(cls, 4));
    return true;
}

static bool luna_instance_of(VM *vm, Value obj, Value cls, bool *result) {
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

static bool luna_get_field_slot(VM *vm, Value obj, int slot, Value *out) {
    (void)vm;
    if (IS_INSTANCE(obj)) {
        ObjInstance *inst = (ObjInstance *)AS_OBJ(obj);
        if (slot >= 0 && slot < inst->field_count) { *out = inst->fields[slot]; return true; }
    }
    *out = make_null();
    return true;
}

static bool luna_set_field_slot(VM *vm, Value obj, int slot, Value value) {
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
 * static/module function. *callable = null signals an optional-_init no-op. */
static bool luna_invoke(VM *vm, Value obj, Value name, Value *self_arg, Value *callable) {
    *callable = make_null();
    if (!IS_OBJ(obj) || !AS_OBJ(obj) || !IS_STRING(name)) {
        vm->last_exception = make_exception_instance(vm, vm->exception_class, "unknown method");
        return false;
    }
    ObjString *name_obj = (ObjString *)AS_OBJ(name);
    const char *mname = name_obj->chars;
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
            vm->last_exception = make_exception_instance(vm, vm->attribute_error_class, buf);
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
            vm->last_exception = make_exception_instance(vm, vm->exception_class, buf);
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
            if (k && strcmp(mname, "_init") == 0) return true;
            char buf[256];
            snprintf(buf, sizeof(buf), "unknown method '%s'", mname);
            vm->last_exception = make_exception_instance(vm, vm->exception_class, buf);
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
            if (klass && strcmp(mname, "_init") == 0) return true;
            char buf[256];
            snprintf(buf, sizeof(buf), "unknown method '%s'", mname);
            vm->last_exception = make_exception_instance(vm, vm->exception_class, buf);
            return false;
        }
    }
}

/* 6d.5: resolve a super-call over the direct base class (always method layout). */
static bool luna_super(VM *vm, Value self, Value name, Value *self_arg, Value *callable) {
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
    vm->last_exception = make_exception_instance(vm, vm->exception_class, buf);
    return false;
}

static bool luna_contains(VM *vm, Value needle, Value haystack, bool *found) {
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
static bool luna_member_get(VM *vm, Value object, Value name, bool safe, Value *out) {
    if (!IS_OBJ(object) || !AS_OBJ(object) || !IS_STRING(name)) {
        if (safe) { *out = make_null(); return true; }
        vm->last_exception = make_exception_instance(vm, vm->attribute_error_class, "cannot access member of null value");
        return false;
    }
    char *chars = ((ObjString *)AS_OBJ(name))->chars;
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
            return member_miss(vm, safe, vm->attribute_error_class, buf, out);
        }
        case OBJ_MODULE: {
            ObjModule *mod = (ObjModule *)AS_OBJ(object);
            Value export_val = dict_get(mod->exports, make_obj((Object *)name));
            if (!IS_NIL(export_val)) { *out = export_val; return true; }
            char buf[256];
            snprintf(buf, sizeof(buf), "module '%s' has no attribute '%s'", mod->name->chars, chars);
            return member_miss(vm, safe, vm->attribute_error_class, buf, out);
        }
        case OBJ_DICT: {
            ObjClass *dklass = vm->dict_class;
            ObjFunction *mfn = dklass ? class_find_method(dklass, chars) : NULL;
            if (mfn) {
                *out = make_obj((Object *)new_bound_method(object, mfn));
                return true;
            }
            Value val = dict_get((ObjDict *)AS_OBJ(object), make_obj((Object *)name));
            if (!IS_NIL(val)) { *out = val; return true; }
            char buf[256];
            snprintf(buf, sizeof(buf), "'%s' not found in dict", chars);
            return member_miss(vm, safe, vm->key_error_class, buf, out);
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
                        return member_miss(vm, safe, vm->attribute_error_class, type->name, out);
                    *out = result;
                    return true;
                }
                *out = make_obj((Object *)new_bound_method(object, mfn));
                return true;
            }
            char buf[256];
            snprintf(buf, sizeof(buf), "%s has no field '%s'", type->name, chars);
            return member_miss(vm, safe, vm->attribute_error_class, buf, out);
        }
        case OBJ_ENUM: {
            ObjEnum *e = (ObjEnum *)AS_OBJ(object);
            int fi = -1;
            for (int i = 0; i < e->count; i++)
                if (strcmp(e->names[i], chars) == 0) { fi = i; break; }
            if (fi >= 0) { *out = make_int(e->values[fi]); return true; }
            char buf[256];
            snprintf(buf, sizeof(buf), "enum has no variant '%s'", chars);
            return member_miss(vm, safe, vm->attribute_error_class, buf, out);
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
            return member_miss(vm, safe, vm->attribute_error_class, buf, out);
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
                else return member_miss(vm, safe, vm->attribute_error_class, "vector has no field", out);
                *out = make_double((double)v);
                return true;
            }
            return member_miss(vm, safe, vm->attribute_error_class, "vector has no field", out);
        }
        default:
            return member_miss(vm, safe, vm->attribute_error_class, "value has no fields", out);
    }
}

/* 6d.5: authoritative member set. Mirrors the legacy OP_MEMBERSET switch. */
static bool luna_member_set(VM *vm, Value object, Value name, Value value) {
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
                vm->last_exception = make_exception_instance(vm, vm->attribute_error_class, "vector has no field");
                return false;
            }
            return true;
        }
        vm->last_exception = make_exception_instance(vm, vm->attribute_error_class, "vector has no field");
        return false;
    }
    vm->last_exception = make_exception_instance(vm, vm->attribute_error_class, "cannot set field on this type");
    return false;
}

static bool luna_import_module(VM *vm, Value module_name, const char *from_path, Value *out) {
    if (!IS_STRING(module_name)) {
        vm->last_exception = make_exception_instance(vm, vm->exception_class,
            "import: module name must be a string");
        return false;
    }
    const char *name = ((ObjString *)AS_OBJ(module_name))->chars;
    Value cached = dict_get(vm->module_cache, module_name);
    if (!IS_NIL(cached) && IS_MODULE(cached)) { *out = cached; return true; }

    char *path = module_resolve_path(name, from_path && from_path[0] ? from_path : NULL);
    if (!path) {
        char message[256]; snprintf(message, sizeof(message), "module '%s' not found", name);
        vm->last_exception = make_exception_instance(vm, vm->exception_class, message);
        return false;
    }
    char *source = module_read_source(path);
    if (!source) {
        free(path);
        char message[256]; snprintf(message, sizeof(message), "cannot read module '%s'", name);
        vm->last_exception = make_exception_instance(vm, vm->exception_class, message);
        return false;
    }
    Lexer *lexer = lexer_new(source);
    TokenList *tokens = lexer_tokenize(lexer);
    Parser *parser = parser_new(tokens, source, path);
    Program *program = parser_parse(parser);
    if (parser->had_error) {
        parser_free(parser); token_list_free(tokens); lexer_free(lexer); free(source); free(path);
        vm->last_exception = make_exception_instance(vm, vm->exception_class, "import: parser errors");
        return false;
    }
    GlobalEntry **saved = vm_globals_save(vm);
    vm_globals_fresh(vm);
    Chunk chunk;
    bool compiled = compile_program(program, &chunk, vm, false, true);
    parser_free(parser); token_list_free(tokens); lexer_free(lexer); free(source);
    if (!compiled) {
        vm_globals_restore(vm, saved); free(path);
        vm->last_exception = make_exception_instance(vm, vm->exception_class, "import: compilation errors");
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
    dict_set(vm->module_cache, make_obj((Object *)new_string(name, (int)strlen(name))), *out);
    return true;
}

static Value luna_make_exception(VM *vm, VMExceptionKind kind, const char *message) {
    void *klass = vm->exception_class;
    switch (kind) {
        case VM_EXCEPTION_TYPE: klass = vm->type_error_class; break;
        case VM_EXCEPTION_KEY: klass = vm->key_error_class; break;
        case VM_EXCEPTION_INDEX: klass = vm->index_error_class; break;
        case VM_EXCEPTION_ATTRIBUTE: klass = vm->attribute_error_class; break;
        case VM_EXCEPTION_VALUE: klass = vm->value_error_class; break;
        case VM_EXCEPTION_RUNTIME: klass = vm->runtime_error_class; break;
        case VM_EXCEPTION_ARGUMENT: klass = vm->argument_error_class; break;
        default: break;
    }
    return make_exception_instance(vm, klass, message);
}

/* Build an exception from an explicit frontend class object (used by
 * luna_throw, which native functions call with the class they were given). */
static Value luna_make_exception_for_class(VM *vm, void *cls, const char *message) {
    return make_exception_instance(vm, cls, message);
}

/* Build a string constant object for the chunk constant pool. */
static Value luna_new_string(VM *vm, const char *chars, int length) {
    (void)vm;
    return make_obj((Object *)new_string(chars, length));
}

/* Open-upvalue list management (the core owns the void* head, we own the
 * ObjUpvalue objects and their linkage). */
static Object *luna_capture_upvalue(VM *vm, int stack_idx) {
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

static void luna_close_upvalues(VM *vm, int frame_depth) {
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

static bool luna_fe_is_string(VM *vm, Value v) {
    (void)vm;
    return IS_STRING(v);
}

static bool luna_fe_is_instance(VM *vm, Value v) {
    (void)vm;
    return IS_INSTANCE(v);
}

/* OP_CLOSURE delegates the whole closure construction here.  The frontend owns
 * the closure layout and upvalue capture; the core only supplies the function
 * constant and the current frame.  Mirrors the old inline OP_CLOSURE body. */
static Object *luna_fe_new_closure(VM *vm, Value fn_val) {
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

static const VMFrontendHooks luna_frontend_hooks = {
    .mark_roots = luna_mark_roots,
    .capture_upvalue = luna_capture_upvalue,
    .close_upvalues = luna_close_upvalues,
    .unary = luna_unary_operation,
    .binary = luna_binary_operation,
    .compare = luna_compare_operation,
    .getitem = luna_index_get,
    .setitem = luna_index_set,
    .getattr = luna_member_get,
    .setattr = luna_member_set,
    .slice = luna_slice,
    .iterate = luna_iterate,
    .iter_next = luna_iter_next,
    .new_list = luna_new_list_op,
    .new_dict = luna_new_dict_op,
    .new_string = luna_new_string,
    .list_append = luna_list_append_op,
    .construct = luna_construct,
    .instance_of = luna_instance_of,
    .get_field_slot = luna_get_field_slot,
    .set_field_slot = luna_set_field_slot,
    .invoke = luna_invoke,
    .super_fn = luna_super,
    .contains = luna_contains,
    .import_module = luna_import_module,
    .make_exception = luna_make_exception,
    .make_exception_for_class = luna_make_exception_for_class,
    .is_string = luna_fe_is_string,
    .is_instance = luna_fe_is_instance,
    .new_closure = luna_fe_new_closure
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
extern uint64_t luna_time_monotonic_us(void);

static ObjClass *luna_register_exception(VM *vm, const char *name, ObjClass *base) {
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

void luna_init_vm(VM *vm) {
    vm->time_start_us = luna_time_monotonic_us();
    vm_register_builtins(vm);
    vm_register_canonical_classes(vm);

    vm->exception_class = luna_register_exception(vm, "Exception", NULL);
    vm->type_error_class = luna_register_exception(vm, "TypeError", vm->exception_class);
    vm->key_error_class = luna_register_exception(vm, "KeyError", vm->exception_class);
    vm->index_error_class = luna_register_exception(vm, "IndexError", vm->exception_class);
    vm->attribute_error_class = luna_register_exception(vm, "AttributeError", vm->exception_class);
    vm->value_error_class = luna_register_exception(vm, "ValueError", vm->exception_class);
    vm->runtime_error_class = luna_register_exception(vm, "RuntimeError", vm->exception_class);
    vm->argument_error_class = luna_register_exception(vm, "ArgumentError", vm->exception_class);

    vm->module_cache = new_dict();
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
    vm_set_frontend(vm, &luna_frontend_hooks);
}

/* ============================================================ */
/* State management                                              */
/* ============================================================ */

luna_State *luna_new_state(void) {
    luna_State *L = calloc(1, sizeof(luna_State));
    if (!L) return NULL;

    L->vm = calloc(1, sizeof(VM));
    if (!L->vm) { free(L); return NULL; }

    vm_init(L->vm);
    luna_init_vm(L->vm);
    luna_wire_lifecycle();
    vm_set_frontend(L->vm, &luna_frontend_hooks);
    L->vm->api_state = L;

    L->cap = LUNA_API_STACK_INIT;
    L->stack = calloc((size_t)L->cap, sizeof(Value));
    if (!L->stack) {
        vm_free(L->vm);
        free(L->vm);
        free(L);
        return NULL;
    }
    L->top = 0;

    return L;
}

void luna_close(luna_State *L) {
    if (!L) return;
    VM *vm = L->vm;
    vm->api_state = NULL;
    free(L->stack);
    vm_free(vm);
    free(vm);
    free(L);
}

/* ============================================================ */
/* Stack manipulation helpers                                    */
/* ============================================================ */

static Value *luna_stack_ptr(luna_State *L, int idx) {
    if (idx < 0) idx = L->top + idx;
    if (idx < 0 || idx >= L->top) return NULL;
    return &L->stack[idx];
}

static bool luna_grow_stack(luna_State *L, int min) {
    if (min <= L->cap) return true;
    int new_cap = L->cap * 2;
    while (new_cap < min) new_cap *= 2;
    Value *new_stack = realloc(L->stack, (size_t)new_cap * sizeof(Value));
    if (!new_stack) return false;
    /* Zero new slots */
    for (int i = L->cap; i < new_cap; i++) new_stack[i] = make_null();
    L->stack = new_stack;
    L->cap = new_cap;
    return true;
}

/* ============================================================ */
/* Stack manipulation API                                        */
/* ============================================================ */

int luna_get_top(luna_State *L) {
    return L->top;
}

void luna_set_top(luna_State *L, int n) {
    if (n < 0) n = L->top + n + 1;
    if (n < 0) n = 0;
    if (n > L->top) {
        if (!luna_grow_stack(L, n)) return;
        for (int i = L->top; i < n; i++) L->stack[i] = make_null();
    }
    L->top = n;
}

void luna_push_value(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v) return;
    if (!luna_grow_stack(L, L->top + 1)) return;
    L->stack[L->top++] = *v;
}

void luna_remove(luna_State *L, int idx) {
    if (idx < 0 || idx >= L->top) return;
    for (int i = idx; i < L->top - 1; i++)
        L->stack[i] = L->stack[i + 1];
    L->top--;
}

void luna_insert(luna_State *L, int idx) {
    if (idx < 0 || idx > L->top) return;
    if (!luna_grow_stack(L, L->top + 1)) return;
    for (int i = L->top; i > idx; i--)
        L->stack[i] = L->stack[i - 1];
    L->stack[idx] = make_null();
    L->top++;
}

void luna_replace(luna_State *L, int idx) {
    if (idx < 0 || idx >= L->top) return;
    L->stack[idx] = L->stack[L->top - 1];
    L->top--;
}

int luna_check_stack(luna_State *L, int n) {
    return luna_grow_stack(L, L->top + n) ? 1 : 0;
}

/* ============================================================ */
/* Type checking                                                 */
/* ============================================================ */

int luna_type(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v) return LUNA_TNIL;
    Value val = *v;

    if (IS_NIL(val))     return LUNA_TNIL;
    if (IS_BOOL(val))    return LUNA_TBOOLEAN;
    if (IS_INT(val))     return LUNA_TINTEGER;
    if (IS_INT64(val))   return LUNA_TINTEGER;
    if (IS_DOUBLE(val))  return LUNA_TNUMBER;
    if (IS_STRING(val))  return LUNA_TSTRING;

    if (IS_OBJ(val)) {
        Object *obj = AS_OBJ(val);
        switch (obj->type->kind) {
            case OBJ_STRING:     return LUNA_TSTRING;
            case OBJ_FUNCTION:   return LUNA_TFUNCTION;
            case OBJ_CLOSURE:    return LUNA_TFUNCTION;
            case OBJ_CLASS:      return LUNA_TCLASS;
            case OBJ_INSTANCE:   return LUNA_TINSTANCE;
            case OBJ_DICT:       return LUNA_TTABLE;
            case OBJ_LIST:       return LUNA_TTABLE;
            case OBJ_USERDATA:   return LUNA_TUSERDATA;
            case OBJ_VECTOR:     return LUNA_TVECTOR;
            case OBJ_MATRIX:     return LUNA_TMATRIX;
            case OBJ_BUFFER:     return LUNA_TBUFFER;
            case OBJ_INT64:      return LUNA_TINTEGER;
            default:             return LUNA_TNIL;
        }
    }
    return LUNA_TNIL;
}

bool luna_is_nil(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    return v && IS_NIL(*v);
}

bool luna_is_boolean(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    return v && IS_BOOL(*v);
}

bool luna_is_number(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    return v && (IS_DOUBLE(*v) || IS_INT(*v) || IS_INT64(*v));
}

bool luna_is_integer(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    return v && (IS_INT(*v) || IS_INT64(*v));
}

bool luna_is_string(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    return v && IS_STRING(*v);
}

bool luna_is_function(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v) return false;
    if (!IS_OBJ(*v)) return false;
    Object *obj = AS_OBJ(*v);
    return obj->type->kind == OBJ_FUNCTION || obj->type->kind == OBJ_CLOSURE;
}

bool luna_is_cfunction(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v || !IS_OBJ(*v)) return false;
    Object *obj = AS_OBJ(*v);
    if (obj->type->kind != OBJ_FUNCTION) return false;
    return ((ObjFunction *)obj)->cfunc != NULL;
}

bool luna_is_userdata(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    return v && IS_USERDATA(*v);
}

/* ============================================================ */
/* Access functions                                              */
/* ============================================================ */

bool luna_to_boolean(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v) return false;
    if (IS_BOOL(*v)) return AS_BOOL(*v);
    return is_truthy(*v);
}

double luna_to_number(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v) return 0.0;
    if (IS_DOUBLE(*v))  return AS_DOUBLE(*v);
    if (IS_INT(*v))     return (double)AS_INT(*v);
    if (IS_INT64(*v))   return (double)((ObjInt64 *)AS_OBJ(*v))->value;
    return 0.0;
}

int64_t luna_to_integer(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v) return 0;
    if (IS_INT(*v))     return AS_INT(*v);
    if (IS_INT64(*v))   return ((ObjInt64 *)AS_OBJ(*v))->value;
    if (IS_DOUBLE(*v))  return (int64_t)AS_DOUBLE(*v);
    return 0;
}

const char *luna_to_string(luna_State *L, int idx, size_t *len) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v || !IS_STRING(*v)) {
        if (len) *len = 0;
        return NULL;
    }
    ObjString *s = (ObjString *)AS_OBJ(*v);
    if (len) *len = (size_t)s->length;
    return s->chars;
}

/* ============================================================ */
/* Push functions                                                */
/* ============================================================ */

void luna_push_nil(luna_State *L) {
    if (luna_grow_stack(L, L->top + 1))
        L->stack[L->top++] = make_null();
}

void luna_push_boolean(luna_State *L, bool b) {
    if (luna_grow_stack(L, L->top + 1))
        L->stack[L->top++] = make_bool(b);
}

void luna_push_number(luna_State *L, double n) {
    if (luna_grow_stack(L, L->top + 1))
        L->stack[L->top++] = make_double(n);
}

void luna_push_integer(luna_State *L, int64_t n) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    if (n >= INT32_MIN && n <= INT32_MAX)
        L->stack[L->top++] = make_int((int32_t)n);
    else
        L->stack[L->top++] = make_int64(n);
}

void luna_push_string(luna_State *L, const char *s) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    L->stack[L->top++] = make_obj((Object *)new_string(s, (int)strlen(s)));
}

void luna_push_lstring(luna_State *L, const char *s, size_t len) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    L->stack[L->top++] = make_obj((Object *)new_string(s, (int)len));
}

void luna_push_cfunction(luna_State *L, luna_CFunction fn) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    ObjFunction *f = new_native_function("<cfunc>", NULL);
    f->cfunc = (int (*)(struct luna_State *L))fn;
    L->stack[L->top++] = make_obj((Object *)f);
}

/* ============================================================ */
/* Table (dict) operations                                       */
/* ============================================================ */

void luna_new_dict(luna_State *L) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    L->stack[L->top++] = make_obj((Object *)new_dict());
}

void luna_set_field(luna_State *L, int idx, const char *key) {
    Value *dv = luna_stack_ptr(L, idx);
    if (!dv || L->top < 1) return;
    if (!IS_OBJ(*dv) || !AS_OBJ(*dv)->type || !AS_OBJ(*dv)->type->setattr) return;
    AS_OBJ(*dv)->type->setattr(L->vm, *dv, key, L->stack[L->top - 1]);
    L->top--;
}

int luna_get_field(luna_State *L, int idx, const char *key) {
    Value *dv = luna_stack_ptr(L, idx);
    if (!dv || !IS_OBJ(*dv) || !AS_OBJ(*dv)->type || !AS_OBJ(*dv)->type->getattr) return LUNA_TNIL;
    Value val = AS_OBJ(*dv)->type->getattr(L->vm, *dv, key);
    if (!luna_grow_stack(L, L->top + 1)) return LUNA_TNIL;
    L->stack[L->top++] = val;
    return luna_type(L, L->top - 1);
}

/* ============================================================ */
/* List (array) operations                                       */
/* ============================================================ */

void luna_new_list(luna_State *L) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    L->stack[L->top++] = make_obj((Object *)new_list(0));
}

void luna_list_append(luna_State *L, int idx) {
    Value *lv = luna_stack_ptr(L, idx);
    if (!lv || L->top < 1) return;
    if (!IS_OBJ(*lv) || ((Object *)AS_OBJ(*lv))->type->kind != OBJ_LIST) return;
    ObjList *lst = (ObjList *)AS_OBJ(*lv);
    Value val = L->stack[L->top - 1];
    list_add(lst, val);
    L->top--;
}

void luna_get_index(luna_State *L, int idx, int n) {
    Value *lv = luna_stack_ptr(L, idx);
    if (!lv || !IS_OBJ(*lv) || ((Object *)AS_OBJ(*lv))->type->kind != OBJ_LIST) {
        luna_push_nil(L);
        return;
    }
    ObjList *lst = (ObjList *)AS_OBJ(*lv);
    int len = list_length(lst);
    if (n < 0 || n >= len) {
        luna_push_nil(L);
    } else {
        Value val = lst->items ? lst->items[n] : lst->inline_items[n];
        if (luna_grow_stack(L, L->top + 1)) L->stack[L->top++] = val;
    }
}

void luna_set_index(luna_State *L, int idx, int n) {
    Value *lv = luna_stack_ptr(L, idx);
    if (!lv || L->top < 1 || !IS_OBJ(*lv) || ((Object *)AS_OBJ(*lv))->type->kind != OBJ_LIST) return;
    ObjList *lst = (ObjList *)AS_OBJ(*lv);
    Value val = L->stack[L->top - 1];
    list_set(lst, n, val);
    L->top--;
}

/* ============================================================ */
/* Userdata                                                      */
/* ============================================================ */

void luna_new_userdata(luna_State *L, void *data, const char *tag, void (*finalizer)(void *)) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    ObjUserdata *ud = new_userdata_tagged(tag, data, finalizer);
    L->stack[L->top++] = make_obj((Object *)ud);
}

void *luna_to_userdata(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v || !IS_USERDATA(*v)) return NULL;
    return ((ObjUserdata *)AS_OBJ(*v))->data;
}

bool luna_is_userdata_tag(luna_State *L, int idx, const char *tag) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v || !IS_USERDATA(*v)) return false;
    ObjUserdata *ud = (ObjUserdata *)AS_OBJ(*v);
    return ud->tag && tag && strcmp(ud->tag, tag) == 0;
}

const char *luna_get_userdata_tag(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v || !IS_USERDATA(*v)) return NULL;
    return ((ObjUserdata *)AS_OBJ(*v))->tag;
}

void luna_push_lightuserdata(luna_State *L, void *ptr) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    ObjUserdata *ud = new_userdata_tagged("lightuserdata", ptr, NULL);
    L->stack[L->top++] = make_obj((Object *)ud);
}

void *luna_to_lightuserdata(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v || !IS_USERDATA(*v)) return NULL;
    ObjUserdata *ud = (ObjUserdata *)AS_OBJ(*v);
    return ud->data;
}

/* ============================================================ */
/* Global access                                                 */
/* ============================================================ */

int luna_get_global(luna_State *L, const char *name) {
    Value val;
    if (!vm_get_global(L->vm, name, &val)) {
        luna_push_nil(L);
        return LUNA_TNIL;
    }
    if (!luna_grow_stack(L, L->top + 1)) return LUNA_TNIL;
    L->stack[L->top++] = val;
    return luna_type(L, L->top - 1);
}

void luna_set_global(luna_State *L, const char *name) {
    if (L->top < 1) return;
    Value val = L->stack[L->top - 1];
    vm_set_global(L->vm, name, val, false);
    L->top--;
}

/* ============================================================ */
/* System globals (persist across modules)                       */
/* ============================================================ */

void luna_set_system_global(luna_State *L, const char *name) {
    if (L->top < 1) return;
    Value val = L->stack[L->top - 1];
    vm_set_system_global(L->vm, name, val);
    L->top--;
}

int luna_get_system_global(luna_State *L, const char *name) {
    Value val;
    if (!vm_get_global(L->vm, name, &val)) {
        luna_push_nil(L);
        return LUNA_TNIL;
    }
    if (!luna_grow_stack(L, L->top + 1)) return LUNA_TNIL;
    L->stack[L->top++] = val;
    return luna_type(L, L->top - 1);
}

/* ============================================================ */
/* C function dispatch — called from vm_opcodes.inc              */
/* ============================================================ */

Value luna_cfunc_dispatch(VM *vm, ObjFunction *fn, Value *args, int arg_count) {
    luna_State *L = (luna_State *)vm->api_state;
    if (!L || !fn->cfunc) return make_null();

    /* Ensure API stack has room */
    luna_grow_stack(L, L->top + arg_count);

    /* Push args onto the API stack */
    for (int i = 0; i < arg_count; i++) {
        L->stack[L->top++] = args[i];
    }

    /* Call the C function */
    int nresults = fn->cfunc(L);

    /* Clamp nresults to available stack */
    if (nresults < 0) nresults = 0;
    if (nresults > L->top) nresults = L->top;

    /* If there are results, return the first one */
    Value result = make_null();
    if (nresults > 0) {
        result = L->stack[L->top - nresults];
    }

    /* Pop all args + results from API stack */
    L->top -= (arg_count + nresults);

    return result;
}

/* ============================================================ */
/* pcall                                                         */
/* ============================================================ */

luna_Status luna_pcall(luna_State *L, int nargs, int nresults) {
    VM *vm = L->vm;
    int func_idx = L->top - nargs - 1;

    if (func_idx < 0) return LUNA_ERRRUN;

    Value fn_val = L->stack[func_idx];

    /* Collect args */
    Value *args = nargs > 0 ? &L->stack[func_idx + 1] : NULL;

    /* Remove function + args from API stack */
    L->top = func_idx;

    /* Call through vm_call_value */
    Value result;
    VMResult vm_result = vm_call_value(vm, fn_val, args, nargs, &result);

    if (vm_result == VM_EXCEPTION) {
        /* Push error message */
        char *msg = value_to_string(vm->last_exception);
        if (msg) {
            luna_push_string(L, msg);
            free(msg);
        } else {
            luna_push_string(L, "unknown error");
        }
        return LUNA_ERRRUN;
    }

    /* Push results */
    if (nresults != 0) {
        luna_grow_stack(L, L->top + 1);
        L->stack[L->top++] = result;
    }

    return LUNA_OK;
}

/* ============================================================ */
/* Load and run LunaScript                                       */
/* ============================================================ */

luna_Status lunaL_load_string(luna_State *L, const char *str) {
    VM *vm = L->vm;

    /* Lex */
    Lexer *lexer = lexer_new(str);
    if (!lexer) return LUNA_ERRMEM;

    TokenList *tokens = lexer_tokenize(lexer);
    if (!tokens) {
        lexer_free(lexer);
        return LUNA_ERRMEM;
    }

    /* Check for lex errors */
    Token *t = tokens->head;
    while (t) {
        if (t->type == TOK_ERROR) {
            luna_push_string(L, t->value ? t->value : "lexer error");
            token_list_free(tokens);
            lexer_free(lexer);
            return LUNA_ERRSYNTAX;
        }
        t = t->next;
    }

    /* Parse */
    Parser *parser = parser_new(tokens, str, "<string>");
    if (!parser) {
        token_list_free(tokens);
        lexer_free(lexer);
        return LUNA_ERRMEM;
    }

    Program *program = parser_parse(parser);
    if (!program) {
        luna_push_string(L, "parse error");
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return LUNA_ERRSYNTAX;
    }

    /* Compile */
    Chunk *chunk = malloc(sizeof(Chunk));
    if (!chunk) {
        luna_push_string(L, "compile error");
        free_program(program);
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return LUNA_ERRMEM;
    }
    memset(chunk, 0, sizeof(Chunk));
    if (!compile_program(program, chunk, vm, false, false)) {
        luna_push_string(L, "compile error");
        chunk_free(chunk);
        free(chunk);
        free_program(program);
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return LUNA_ERRSYNTAX;
    }
    chunk->source_path = NULL;

    /* Create a function from the chunk */
    ObjFunction *fn = new_function("<load>");
    fn->chunk = chunk; /* Transfer ownership */
    fn->param_count = 0;

    /* Push as a closure */
    ObjClosure *cl = new_closure(fn);
    luna_grow_stack(L, L->top + 1);
    L->stack[L->top++] = make_obj((Object *)cl);

    /* The chunk is owned by the function now; don't free it */
    free_program(program);
    parser_free(parser);
    token_list_free(tokens);
    lexer_free(lexer);

    return LUNA_OK;
}

luna_Status lunaL_load_file(luna_State *L, const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        luna_push_string(L, "cannot open file");
        return LUNA_ERRRUN;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len < 0) {
        fclose(f);
        luna_push_string(L, "cannot read file");
        return LUNA_ERRRUN;
    }

    char *buf = malloc((size_t)(len + 1));
    if (!buf) {
        fclose(f);
        return LUNA_ERRMEM;
    }

    size_t read = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[read] = '\0';

    luna_Status status = lunaL_load_string(L, buf);
    free(buf);

    /* Replace chunk source path with the actual filename */
    if (status == LUNA_OK && L->top > 0) {
        Value *v = luna_stack_ptr(L, L->top - 1);
        if (v && IS_OBJ(*v)) {
            ObjClosure *cl = (ObjClosure *)AS_OBJ(*v);
            if (cl && cl->function && cl->function->chunk) {
                free(cl->function->chunk->source_path);
                cl->function->chunk->source_path = strdup(filename);
            }
        }
    }

    return status;
}

luna_Status lunaL_dostring(luna_State *L, const char *str) {
    luna_Status s = lunaL_load_string(L, str);
    if (s != LUNA_OK) return s;
    /* The function is on the stack; call with 0 args */
    return luna_pcall(L, 0, 1);
}

luna_Status lunaL_dofile(luna_State *L, const char *filename) {
    luna_Status s = lunaL_load_file(L, filename);
    if (s != LUNA_OK) return s;
    return luna_pcall(L, 0, 1);
}

/* ============================================================ */
/* GC                                                           */
/* ============================================================ */

int luna_gc(luna_State *L, int what) {
    (void)what;
    /* Trigger full GC */
    mark_and_sweep(L->vm);
    return 0;
}

/* ============================================================ */
/* Error handling and Arg checking                              */
/* ============================================================ */

void luna_error(luna_State *L, const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    L->vm->last_exception = make_exception_instance(L->vm, L->vm->exception_class, buf);
    if (L->vm->native_jump) {
        longjmp(L->vm->native_jump->env, 1);
    }
}

double lunaL_checknumber(luna_State *L, int arg) {
    if (!luna_is_number(L, arg)) {
        luna_error(L, "bad argument #%d (number expected)", arg + (arg < 0 ? L->top + 1 : 1));
    }
    return luna_to_number(L, arg);
}

int64_t lunaL_checkinteger(luna_State *L, int arg) {
    if (!luna_is_integer(L, arg)) {
        luna_error(L, "bad argument #%d (integer expected)", arg + (arg < 0 ? L->top + 1 : 1));
    }
    return luna_to_integer(L, arg);
}

const char* lunaL_checkstring(luna_State *L, int arg) {
    if (!luna_is_string(L, arg)) {
        luna_error(L, "bad argument #%d (string expected)", arg + (arg < 0 ? L->top + 1 : 1));
    }
    return luna_to_string(L, arg, NULL);
}

void* lunaL_checkuserdata(luna_State *L, int arg, const char *tag) {
    if (!luna_is_userdata_tag(L, arg, tag)) {
        luna_error(L, "bad argument #%d (%s expected)", arg + (arg < 0 ? L->top + 1 : 1), tag);
    }
    return luna_to_userdata(L, arg);
}

