/* slice.c — PySlice objects for the Python frontend.
 *
 * A slice(start, stop, step) object is used as the single key passed through
 * OP_INDEXGET / __getitem__ for `a[start:stop:step]` (CPython parity). The VM
 * core no longer has an OP_SLICE opcode; the frontend owns slice semantics.
 *
 * start/stop/step are int32/bigint Values, or nil (make_null) when omitted.
 * Slicing itself (py_apply_slice) is reached from the list/tuple/string
 * getitem vtable when the key is a PySlice. */
#include "object.h"
#include "frontend_state.h"
#include "value.h"
#include "bigint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Slice bounds: bigints beyond int64 raise OverflowError (CPython: "Python int
 * too large to convert to C ssize_t"); bigints that fit are clamped into the
 * int32 range so the existing clamp logic reduces them correctly. */
static bool py_slice_bound(VM *vm, Value *v) {
    if (IS_BIGINT(*v)) {
        int64_t x;
        if (!bigint_get_i64((ObjBigInt *)AS_OBJ(*v), &x)) {
            vm->last_exception = make_exception_instance(vm, py_fe(vm)->overflow_error_class,
                "Python int too large to convert to C ssize_t");
            return false;
        }
        if (x > INT32_MAX) x = INT32_MAX;
        else if (x < INT32_MIN) x = INT32_MIN;
        *v = make_int((int32_t)x);
    }
    return true;
}

/* Build a slice object. Bounds must already be validated int32/bigint/nil. */
ObjSlice *new_slice(Value start, Value stop, Value step) {
    ObjSlice *s = (ObjSlice *)malloc(sizeof(ObjSlice));
    if (!s) { fprintf(stderr, "OOM\n"); exit(1); }
    py_init_object((Object *)s, OBJ_SLICE, sizeof(ObjSlice));
    s->start = start; s->stop = stop; s->step = step;
    return s;
}

/* Apply a slice object to a list/tuple/string, producing the sliced result as
 * a new object of the same kind. Out of range -> null. */
static bool apply_slice_list(ObjSlice *sl, Value object, Value *out) {
    ObjList *lst = (ObjList *)AS_OBJ(object);
    int len = list_length(lst);
    int step = IS_INT(sl->step) ? AS_INT(sl->step) : 1;
    int start, stop;
    if (IS_NIL(sl->start)) start = (step < 0) ? len - 1 : 0;
    else if (IS_INT(sl->start)) {
        start = AS_INT(sl->start);
        if (start < 0) start += len;
        if (start < 0) start = (step < 0) ? -1 : 0;
        if (start > len) start = len;
    } else start = (step < 0) ? len - 1 : 0;
    if (IS_NIL(sl->stop)) stop = (step < 0) ? -1 : len;
    else if (IS_INT(sl->stop)) {
        stop = AS_INT(sl->stop);
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

static bool apply_slice_tuple(ObjSlice *sl, Value object, Value *out) {
    ObjTuple *t = (ObjTuple *)AS_OBJ(object);
    int len = t->count;
    int step = IS_INT(sl->step) ? AS_INT(sl->step) : 1;
    int start, stop;
    if (IS_NIL(sl->start)) start = (step < 0) ? len - 1 : 0;
    else if (IS_INT(sl->start)) {
        start = AS_INT(sl->start);
        if (start < 0) start += len;
        if (start < 0) start = (step < 0) ? -1 : 0;
        if (start > len) start = len;
    } else start = (step < 0) ? len - 1 : 0;
    if (IS_NIL(sl->stop)) stop = (step < 0) ? -1 : len;
    else if (IS_INT(sl->stop)) {
        stop = AS_INT(sl->stop);
        if (stop < 0) stop += len;
        if (stop < 0) stop = -1;
        if (stop > len) stop = len;
    } else stop = (step < 0) ? -1 : len;
    int cap = (step > 0) ? (stop > start ? (stop - start + step - 1) / step : 0)
                         : (start > stop ? (start - stop - step - 1) / (-step) : 0);
    ObjTuple *result = new_tuple(cap);
    int ri = 0;
    if (step > 0) {
        for (int i = start; i < stop; i += step) result->items[ri++] = t->items[i];
    } else if (step < 0) {
        for (int i = start; i > stop; i += step) result->items[ri++] = t->items[i];
    }
    *out = make_obj((Object *)result);
    return true;
}

static bool apply_slice_string(ObjSlice *sl, Value object, Value *out) {
    ObjString *s = (ObjString *)AS_OBJ(object);
    int byte_len = s->length;
    int cp_count = utf8_code_point_count(s->chars, byte_len);
    int len = cp_count;
    int step = IS_INT(sl->step) ? AS_INT(sl->step) : 1;
    int start, stop;
    if (IS_NIL(sl->start)) start = (step < 0) ? len - 1 : 0;
    else if (IS_INT(sl->start)) {
        start = AS_INT(sl->start);
        if (start < 0) start += len;
        if (start < 0) start = (step < 0) ? -1 : 0;
        if (start > len) start = len;
    } else start = (step < 0) ? len - 1 : 0;
    if (IS_NIL(sl->stop)) stop = (step < 0) ? -1 : len;
    else if (IS_INT(sl->stop)) {
        stop = AS_INT(sl->stop);
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

/* Entry point for the list/tuple/string getitem vtables: apply a slice key.
 * Returns false (with vm->last_exception set) on a bound overflow error. */
bool py_apply_slice(VM *vm, Value object, Value slicekey, Value *out) {
    ObjSlice *sl = (ObjSlice *)AS_OBJ(slicekey);
    Value start = sl->start, stop = sl->stop, step = sl->step;
    if (!py_slice_bound(vm, &start)) return false;
    if (!py_slice_bound(vm, &stop)) return false;
    if (!py_slice_bound(vm, &step)) return false;
    /* Rebuild a normalized slice with clamped bounds for the inner helpers. */
    ObjSlice norm;
    norm.start = start; norm.stop = stop; norm.step = step;
    if (IS_LIST(object)) return apply_slice_list(&norm, object, out);
    if (IS_TUPLE(object)) return apply_slice_tuple(&norm, object, out);
    if (IS_STRING(object)) return apply_slice_string(&norm, object, out);
    *out = make_null();
    return true;
}

/* --- slice() builtin -------------------------------------------------- */
Value py_builtin_slice(VM *vm, Value *args, int n) {
    if (n < 1 || n > 3) {
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->type_error_class,
            "slice expected at most 3 arguments");
        return make_null();
    }
    Value start = make_null(), stop = make_null(), step = make_null();
    for (int i = 0; i < n; i++) {
        Value a = args[i];
        if (!IS_INT(a) && !IS_BIGINT(a) && !IS_NIL(a)) {
            vm->last_exception = make_exception_instance(vm, py_fe(vm)->type_error_class,
                "slice indices must be integers or None");
            return make_null();
        }
    }
    if (n == 1) {
        stop = args[0];
    } else {
        start = args[0]; stop = args[1];
        if (n == 3) step = args[2];
    }
    if (IS_NIL(step)) step = make_int(1);
    if ((IS_INT(step) && AS_INT(step) == 0) ||
        (IS_BIGINT(step) && ((ObjBigInt *)AS_OBJ(step))->sign == 0)) {
        vm->last_exception = make_exception_instance(vm, py_fe(vm)->value_error_class,
            "slice step cannot be zero");
        return make_null();
    }
    return make_obj((Object *)new_slice(start, stop, step));
}

/* --- repr/lifecycle --------------------------------------------------- */
/* Render a scalar bound as its decimal form; nil -> "None". Caller frees. */
char *py_slice_to_cstr(Value self) {
    ObjSlice *s = (ObjSlice *)AS_OBJ(self);
    char *out = malloc(64);
    if (!out) { fprintf(stderr, "OOM\n"); exit(1); }
    if (IS_NIL(s->start)) strcpy(out, "None");
    else if (IS_BIGINT(s->start)) {
        char *d = bigint_to_decimal((ObjBigInt *)AS_OBJ(s->start));
        strcpy(out, d); free(d);
    } else snprintf(out, 64, "%d", AS_INT(s->start));
    size_t n = strlen(out);
    out[n++] = ':';
    if (IS_NIL(s->stop)) strcpy(out + n, "None");
    else if (IS_BIGINT(s->stop)) {
        char *d = bigint_to_decimal((ObjBigInt *)AS_OBJ(s->stop));
        strcpy(out + n, d); free(d);
    } else snprintf(out + n, 64 - n, "%d", AS_INT(s->stop));
    n = strlen(out);
    bool step_one = !IS_NIL(s->step) && IS_INT(s->step) && AS_INT(s->step) == 1;
    if (!step_one) {
        out[n++] = ':';
        if (IS_NIL(s->step)) strcpy(out + n, "None");
        else if (IS_BIGINT(s->step)) {
            char *d = bigint_to_decimal((ObjBigInt *)AS_OBJ(s->step));
            strcpy(out + n, d); free(d);
        } else snprintf(out + n, 64 - n, "%d", AS_INT(s->step));
    }
    return out;
}

/* Lifecycle: slice holds start/stop/step Values that must be traced/kept. */
void py_slice_free(Object *obj) { free(obj); }
void py_slice_mark(struct VM *vm, Object *obj) {
    ObjSlice *s = (ObjSlice *)obj;
    vm_mark_value(vm, s->start);
    vm_mark_value(vm, s->stop);
    vm_mark_value(vm, s->step);
}
