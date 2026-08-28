/* luna/object.c — Luna MOP Type instances + vtable implementations.
 *
 * Part 1: Type definitions + luna_types[] table.
 * Part 2 (this file): arithmetic / comparison / index vtables wired.
 * The core reaches these through Object.type (Type*) so vm.c never
 * names a concrete Luna kind in dispatch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "value.h"
#include "luna/object.h"
#include "chunk.h"

/* ---- helpers available to vtable functions ---- */
static inline bool is_int64_type(Value v) { return IS_OBJ(v) && AS_OBJ(v) && AS_OBJ(v)->type == luna_types[OBJ_INT64]; }

/* ============================================================
 * String operations (vtable)
 * ============================================================ */
static Value luna_string_add(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_STRING(a) || !IS_OBJ(b)) { /* string concat with anything -> convert b */
        /* This replicates the current do_arith behavior: L is string,
         * R is converted to string and concatenated. */
        if (!IS_STRING(a)) { return make_null(); }
    }
    /* Current inline logic: L=string, R=any -> concat.
     * We assume a is always string (called from vtable). */
    ObjString *ls = (ObjString*)AS_OBJ(a);
    const char *rs; int rs_len; char *rs_tmp = NULL;
    if (IS_STRING(b)) {
        ObjString *rs_str = (ObjString*)AS_OBJ(b);
        rs = rs_str->chars; rs_len = rs_str->length;
    } else {
        rs_tmp = value_to_string(b);
        rs = rs_tmp; rs_len = (int)strlen(rs_tmp);
    }
    int len = ls->length + rs_len;
    char *buf = malloc(len + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    memcpy(buf, ls->chars, ls->length);
    memcpy(buf + ls->length, rs, rs_len + 1);
    ObjString *s = new_string(buf, len);
    free(rs_tmp); free(buf);
    return make_obj((Object*)s);
}

static Value luna_string_mul(struct VM *vm, Value a, Value b) {
    (void)vm;
    /* Replication: string * int OR int * string */
    if (IS_STRING(a) && IS_INT(b)) {
        ObjString *str = (ObjString*)AS_OBJ(a);
        int64_t times = AS_INT(b);
        if (times < 0) { return make_obj((Object*)new_string("", 0)); }
        int len = str->length * (int)times;
        char *buf = malloc(len + 1);
        if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
        buf[0] = '\0';
        for (int t = 0; t < times; t++) memcpy(buf + t * str->length, str->chars, str->length);
        buf[len] = '\0';
        ObjString *res = new_string(buf, len);
        free(buf);
        return make_obj((Object*)res);
    }
    if (IS_STRING(b) && IS_INT(a)) {
        return luna_string_mul(vm, b, a);
    }
    return make_null();
}

static int luna_string_cmp(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_STRING(a) || !IS_STRING(b)) return 2; /* unsupported -> do_cmp returns null */
    ObjString *as = (ObjString*)AS_OBJ(a);
    ObjString *bs = (ObjString*)AS_OBJ(b);
    if (as == bs) return 0;
    if (as->length != bs->length) return (as->length < bs->length) ? -1 : 1;
    int cmp = memcmp(as->chars, bs->chars, (size_t)as->length);
    return (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
}

static Value luna_string_neg(struct VM *vm, Value a) { (void)vm; return make_null(); }
static Value luna_string_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_string_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_string_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }

static Value luna_string_getitem(struct VM *vm, Value self, Value key) {
    (void)vm;
    if (!IS_STRING(self) || !IS_INT(key)) return make_null();
    ObjString *s = (ObjString*)AS_OBJ(self);
    int idx = (int)AS_INT(key);
    if (idx < 0) idx += s->length;
    if (idx < 0 || idx >= s->length) return make_null();
    return make_obj((Object*)new_string(&s->chars[idx], 1));
}

static void luna_string_setitem(struct VM *vm, Value self, Value key, Value val) {
    (void)vm; (void)self; (void)key; (void)val; /* unsupported */
}

static Value luna_string_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int luna_string_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value luna_string_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_string_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_string_hash(Value self) { return ((ObjString*)AS_OBJ(self))->hash; }
static int luna_string_len(struct VM *vm, Value self) { (void)vm; return utf8_code_point_count(((ObjString*)AS_OBJ(self))->chars, ((ObjString*)AS_OBJ(self))->length); }

/* ============================================================
 * List operations (vtable)
 * ============================================================ */
static Value luna_list_add(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_LIST(a) || !IS_LIST(b)) return make_null();
    ObjList *ls = (ObjList*)AS_OBJ(a);
    ObjList *rs = (ObjList*)AS_OBJ(b);
    ObjList *res = new_list(ls->count + rs->count);
    for (int i = 0; i < ls->count; i++) list_add(res, ls->items ? ls->items[i] : ls->inline_items[i]);
    for (int i = 0; i < rs->count; i++) list_add(res, rs->items ? rs->items[i] : rs->inline_items[i]);
    return make_obj((Object*)res);
}

static Value luna_list_mul(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (IS_LIST(a) && IS_INT(b)) {
        ObjList *lst = (ObjList*)AS_OBJ(a);
        int64_t times = AS_INT(b);
        if (times < 0) { return make_obj((Object*)new_list(0)); }
        ObjList *res = new_list(lst->count * (int)times);
        for (int t = 0; t < times; t++)
            for (int i = 0; i < lst->count; i++)
                list_add(res, lst->items ? lst->items[i] : lst->inline_items[i]);
        return make_obj((Object*)res);
    }
    if (IS_LIST(b) && IS_INT(a)) {
        return luna_list_mul(vm, b, a);
    }
    return make_null();
}

static int luna_list_cmp(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_LIST(a) || !IS_LIST(b)) return 2; /* unsupported */
    /* Only exact equality is supported for list comparison (pointer-based in current code) */
    return (a == b) ? 0 : 1;  /* approximate; current behavior uses values_equal for EQ/NE */
    /* Actually current values_equal compares pointers for objects; let's replicate: */
    /* This is handled by the caller (do_cmp) for EQ/NE; cmp vtable only called when needed. */
}

static Value luna_list_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_list_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_list_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_list_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }

static Value luna_list_getitem(struct VM *vm, Value self, Value key) {
    (void)vm;
    if (!IS_LIST(self) || !IS_INT(key)) return make_null();
    ObjList *lst = (ObjList*)AS_OBJ(self);
    int idx = (int)AS_INT(key);
    int len = lst->count;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return make_null();
    return lst->items ? lst->items[idx] : lst->inline_items[idx];
}

static void luna_list_setitem(struct VM *vm, Value self, Value key, Value val) {
    (void)vm;
    if (!IS_LIST(self) || !IS_INT(key)) return;
    ObjList *lst = (ObjList*)AS_OBJ(self);
    int idx = (int)AS_INT(key);
    int len = lst->count;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return; /* out of bounds -> no-op for this vtable */
    if (lst->items) lst->items[idx] = val; else lst->inline_items[idx] = val;
}

static Value luna_list_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int luna_list_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value luna_list_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_list_tostring(struct VM *vm, Value self) { (void)vm; return self; }  /* approximate */
static uint32_t luna_list_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_list_len(struct VM *vm, Value self) { (void)vm; return ((ObjList*)AS_OBJ(self))->count; }

/* ============================================================
 * Dict operations (vtable)
 * ============================================================ */
static Value luna_dict_add(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_dict_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_dict_mul(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_dict_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_dict_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_dict_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }
static int luna_dict_cmp(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return 2; }

static Value luna_dict_getitem(struct VM *vm, Value self, Value key) {
    (void)vm;
    if (!IS_DICT(self)) return make_null();
    return dict_get((ObjDict*)AS_OBJ(self), key);
}

static void luna_dict_setitem(struct VM *vm, Value self, Value key, Value val) {
    (void)vm;
    if (!IS_DICT(self)) return;
    dict_set((ObjDict*)AS_OBJ(self), key, val);
}

static Value luna_dict_getattr(struct VM *vm, Value self, const char *name) {
    (void)vm;
    if (!IS_DICT(self)) return make_null();
    return dict_get((ObjDict*)AS_OBJ(self), make_obj((Object*)new_string(name, (int)strlen(name))));
}
static int luna_dict_setattr(struct VM *vm, Value self, const char *name, Value val) {
    (void)vm;
    if (!IS_DICT(self)) return 0;
    dict_set((ObjDict*)AS_OBJ(self), make_obj((Object*)new_string(name, (int)strlen(name))), val);
    return 1;
}
static Value luna_dict_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_dict_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_dict_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_dict_len(struct VM *vm, Value self) { (void)vm; return ((ObjDict*)AS_OBJ(self))->entry_count; }

/* ============================================================
 * Instance operations (vtable)
 * ============================================================ */
static Value luna_instance_add(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_instance_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_instance_mul(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_instance_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_instance_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_instance_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }
static int luna_instance_cmp(struct VM *vm, Value a, Value b) { (void)vm; if (!IS_INSTANCE(a) || !IS_INSTANCE(b)) return 2; return (a == b) ? 0 : 1; }

static Value luna_instance_getitem(struct VM *vm, Value self, Value key) {
    (void)vm;
    if (!IS_INSTANCE(self) || !IS_STRING(key)) return make_null();
    return instance_get_field((ObjInstance*)AS_OBJ(self), ((ObjString*)AS_OBJ(key))->chars);
}

static void luna_instance_setitem(struct VM *vm, Value self, Value key, Value val) {
    (void)vm;
    if (!IS_INSTANCE(self) || !IS_STRING(key)) return;
    instance_set_field((ObjInstance*)AS_OBJ(self), ((ObjString*)AS_OBJ(key))->chars, val);
}

static Value luna_instance_getattr(struct VM *vm, Value self, const char *name) {
    (void)vm;
    if (!IS_INSTANCE(self)) return make_null();
    return instance_get_field((ObjInstance*)AS_OBJ(self), name);
}

static int luna_instance_setattr(struct VM *vm, Value self, const char *name, Value val) {
    (void)vm;
    if (!IS_INSTANCE(self)) return 0;
    instance_set_field((ObjInstance*)AS_OBJ(self), name, val);
    return 1;
}

static Value luna_instance_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_instance_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_instance_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_instance_len(struct VM *vm, Value self) { (void)vm; (void)self; return 0; }

/* ============================================================
 * Vector operations (vtable)
 * ============================================================ */
static Value luna_vector_add(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_VECTOR(a) || !IS_VECTOR(b)) return make_null();
    ObjVector *av = (ObjVector*)AS_OBJ(a), *bv = (ObjVector*)AS_OBJ(b);
    return make_obj((Object*)new_vector(av->data[0]+bv->data[0], av->data[1]+bv->data[1], av->data[2]+bv->data[2], av->data[3]+bv->data[3]));
}
static Value luna_vector_sub(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_VECTOR(a) || !IS_VECTOR(b)) return make_null();
    ObjVector *av = (ObjVector*)AS_OBJ(a), *bv = (ObjVector*)AS_OBJ(b);
    return make_obj((Object*)new_vector(av->data[0]-bv->data[0], av->data[1]-bv->data[1], av->data[2]-bv->data[2], av->data[3]-bv->data[3]));
}
static Value luna_vector_mul(struct VM *vm, Value a, Value b) {
    (void)vm;
    /* Component-wise or scalar mul (current behavior in do_arith) */
    if (IS_VECTOR(a) && IS_VECTOR(b)) {
        ObjVector *av = (ObjVector*)AS_OBJ(a), *bv = (ObjVector*)AS_OBJ(b);
        return make_obj((Object*)new_vector(av->data[0]*bv->data[0], av->data[1]*bv->data[1], av->data[2]*bv->data[2], av->data[3]*bv->data[3]));
    }
    if (IS_VECTOR(a) && IS_NUMBER(b)) {
        ObjVector *av = (ObjVector*)AS_OBJ(a);
        float s = (float)value_to_double(b);
        return make_obj((Object*)new_vector(av->data[0]*s, av->data[1]*s, av->data[2]*s, av->data[3]*s));
    }
    if (IS_NUMBER(a) && IS_VECTOR(b)) {
        return luna_vector_mul(vm, b, a);
    }
    return make_null();
}
static Value luna_vector_div(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (IS_VECTOR(a) && IS_NUMBER(b)) {
        ObjVector *av = (ObjVector*)AS_OBJ(a);
        float s = (float)value_to_double(b);
        return make_obj((Object*)new_vector(av->data[0]/s, av->data[1]/s, av->data[2]/s, av->data[3]/s));
    }
    return make_null();
}
static Value luna_vector_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_vector_neg(struct VM *vm, Value a) {
    (void)vm;
    if (!IS_VECTOR(a)) return make_null();
    ObjVector *v = (ObjVector*)AS_OBJ(a);
    return make_obj((Object*)new_vector(-v->data[0], -v->data[1], -v->data[2], -v->data[3]));
}
static int luna_vector_cmp(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (!IS_VECTOR(a) || !IS_VECTOR(b)) return 2;
    return a == b ? 0 : 1;
}

static Value luna_vector_getitem(struct VM *vm, Value self, Value key) { (void)vm; (void)self; (void)key; return make_null(); }
static void luna_vector_setitem(struct VM *vm, Value self, Value key, Value val) { (void)vm; (void)self; (void)key; (void)val; }
static Value luna_vector_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int luna_vector_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value luna_vector_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_vector_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_vector_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_vector_len(struct VM *vm, Value self) { (void)vm; return 4; }

/* ============================================================
 * Matrix operations (vtable)
 * ============================================================ */
static Value luna_matrix_add(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_matrix_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_matrix_mul(struct VM *vm, Value a, Value b) {
    (void)vm;
    if (IS_MATRIX(a) && IS_MATRIX(b)) {
        ObjMatrix *ma = (ObjMatrix*)AS_OBJ(a), *mb = (ObjMatrix*)AS_OBJ(b);
        ObjMatrix *res = new_matrix();
        for (int col = 0; col < 4; col++) for (int row = 0; row < 4; row++) {
            float sum = 0.0f; for (int k = 0; k < 4; k++) sum += ma->m[row + k*4] * mb->m[k + col*4];
            res->m[row + col*4] = sum; }
        return make_obj((Object*)res);
    }
    if (IS_MATRIX(a) && IS_VECTOR(b)) {
        ObjMatrix *m = (ObjMatrix*)AS_OBJ(a); ObjVector *v = (ObjVector*)AS_OBJ(b);
        float rx = m->m[0]*v->data[0] + m->m[4]*v->data[1] + m->m[8]*v->data[2] + m->m[12]*v->data[3];
        float ry = m->m[1]*v->data[0] + m->m[5]*v->data[1] + m->m[9]*v->data[2] + m->m[13]*v->data[3];
        float rz = m->m[2]*v->data[0] + m->m[6]*v->data[1] + m->m[10]*v->data[2] + m->m[14]*v->data[3];
        float rw = m->m[3]*v->data[0] + m->m[7]*v->data[1] + m->m[11]*v->data[2] + m->m[15]*v->data[3];
        return make_obj((Object*)new_vector(rx, ry, rz, rw));
    }
    if (IS_MATRIX(a) && IS_NUMBER(b)) {
        ObjMatrix *m = (ObjMatrix*)AS_OBJ(a); float s = (float)value_to_double(b); ObjMatrix *res = new_matrix();
        for (int i = 0; i < 16; i++) res->m[i] = m->m[i] * s; return make_obj((Object*)res);
    }
    return make_null();
}
static Value luna_matrix_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_matrix_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_matrix_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }
static int luna_matrix_cmp(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return 2; }

static Value luna_matrix_getitem(struct VM *vm, Value self, Value key) { (void)vm; (void)self; (void)key; return make_null(); }
static void luna_matrix_setitem(struct VM *vm, Value self, Value key, Value val) { (void)vm; (void)self; (void)key; (void)val; }
static Value luna_matrix_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int luna_matrix_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value luna_matrix_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_matrix_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_matrix_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_matrix_len(struct VM *vm, Value self) { (void)vm; return 16; }

/* ============================================================
 * Int64 (no arithmetic vtable needed — core handles natively,
 * but we provide stubs so Object.type->add etc. is never NULL)
 * ============================================================ */
static Value luna_int64_add(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_int64_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_int64_mul(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_int64_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_int64_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_int64_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }
static int luna_int64_cmp(struct VM *vm, Value a, Value b) { (void)vm; return (a == b) ? 0 : 1; }
static Value luna_int64_getitem(struct VM *vm, Value self, Value key) { (void)vm; (void)self; (void)key; return make_null(); }
static void luna_int64_setitem(struct VM *vm, Value self, Value key, Value val) { (void)vm; (void)self; (void)key; (void)val; }
static Value luna_int64_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int luna_int64_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value luna_int64_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_int64_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_int64_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_int64_len(struct VM *vm, Value self) { (void)vm; return 1; }

/* ============================================================
 * Default stubs for kinds without special arithmetic/index rules
 * ============================================================ */
#define DEFAULT_BIN  (struct VM *vm, Value a, Value b) -> return make_null()
static Value luna_default_add(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_default_sub(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_default_mul(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_default_div(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_default_mod(struct VM *vm, Value a, Value b) { (void)vm; (void)a; (void)b; return make_null(); }
static Value luna_default_neg(struct VM *vm, Value a) { (void)vm; (void)a; return make_null(); }
static int luna_default_cmp(struct VM *vm, Value a, Value b) { (void)vm; return (a == b) ? 0 : 1; }
static Value luna_default_getitem(struct VM *vm, Value self, Value key) { (void)vm; (void)self; (void)key; return make_null(); }
static void luna_default_setitem(struct VM *vm, Value self, Value key, Value val) { (void)vm; (void)self; (void)key; (void)val; }
static Value luna_default_getattr(struct VM *vm, Value self, const char *name) { (void)vm; (void)self; (void)name; return make_null(); }
static int luna_default_setattr(struct VM *vm, Value self, const char *name, Value val) { (void)vm; (void)self; (void)name; (void)val; return 0; }
static Value luna_default_call(struct VM *vm, Value self, Value *args, int argc) { (void)vm; (void)self; (void)args; (void)argc; return make_null(); }
static Value luna_default_tostring(struct VM *vm, Value self) { (void)vm; return self; }
static uint32_t luna_default_hash(Value self) { return (uint32_t)(uintptr_t)AS_OBJ(self); }
static int luna_default_len(struct VM *vm, Value self) { (void)vm; return 0; }

/* ============================================================
 * Type instance definitions (vtable filled)
 * ============================================================ */
Type luna_string_type  = {
    .name = "string", .kind = OBJ_STRING,
    .add = luna_string_add, .sub = luna_string_sub, .mul = luna_string_mul, .div = luna_string_div, .mod = luna_string_mod,
    .neg = luna_string_neg, .cmp = luna_string_cmp,
    .getitem = luna_string_getitem, .setitem = luna_string_setitem,
    .getattr = luna_string_getattr, .setattr = luna_string_setattr,
    .call = luna_string_call, .tostring = luna_string_tostring, .hash = luna_string_hash, .len = luna_string_len
};

Type luna_list_type = {
    .name = "list", .kind = OBJ_LIST,
    .add = luna_list_add, .sub = luna_list_sub, .mul = luna_list_mul, .div = luna_list_div, .mod = luna_list_mod,
    .neg = luna_list_neg, .cmp = luna_list_cmp,
    .getitem = luna_list_getitem, .setitem = luna_list_setitem,
    .getattr = luna_list_getattr, .setattr = luna_list_setattr,
    .call = luna_list_call, .tostring = luna_list_tostring, .hash = luna_list_hash, .len = luna_list_len
};

Type luna_dict_type = {
    .name = "dict", .kind = OBJ_DICT,
    .add = luna_dict_add, .sub = luna_dict_sub, .mul = luna_dict_mul, .div = luna_dict_div, .mod = luna_dict_mod,
    .neg = luna_dict_neg, .cmp = luna_dict_cmp,
    .getitem = luna_dict_getitem, .setitem = luna_dict_setitem,
    .getattr = luna_dict_getattr, .setattr = luna_dict_setattr,
    .call = luna_dict_call, .tostring = luna_dict_tostring, .hash = luna_dict_hash, .len = luna_dict_len
};

Type luna_instance_type = {
    .name = "instance", .kind = OBJ_INSTANCE,
    .add = luna_instance_add, .sub = luna_instance_sub, .mul = luna_instance_mul, .div = luna_instance_div, .mod = luna_instance_mod,
    .neg = luna_instance_neg, .cmp = luna_instance_cmp,
    .getitem = luna_instance_getitem, .setitem = luna_instance_setitem,
    .getattr = luna_instance_getattr, .setattr = luna_instance_setattr,
    .call = luna_instance_call, .tostring = luna_instance_tostring, .hash = luna_instance_hash, .len = luna_instance_len
};

Type luna_function_type = {
    .name = "function", .kind = OBJ_FUNCTION,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_closure_type = {
    .name = "closure", .kind = OBJ_CLOSURE,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_upvalue_type = {
    .name = "upvalue", .kind = OBJ_UPVALUE,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_enum_type = {
    .name = "enum", .kind = OBJ_ENUM,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_class_type = {
    .name = "class", .kind = OBJ_CLASS,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_bound_method_type = {
    .name = "bound_method", .kind = OBJ_BOUND_METHOD,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_module_type = {
    .name = "module", .kind = OBJ_MODULE,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_buffer_type = {
    .name = "buffer", .kind = OBJ_BUFFER,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_int64_type = {
    .name = "int64", .kind = OBJ_INT64,
    .add = luna_int64_add, .sub = luna_int64_sub, .mul = luna_int64_mul, .div = luna_int64_div, .mod = luna_int64_mod,
    .neg = luna_int64_neg, .cmp = luna_int64_cmp,
    .getitem = luna_int64_getitem, .setitem = luna_int64_setitem,
    .getattr = luna_int64_getattr, .setattr = luna_int64_setattr,
    .call = luna_int64_call, .tostring = luna_int64_tostring, .hash = luna_int64_hash, .len = luna_int64_len
};

Type luna_userdata_type = {
    .name = "userdata", .kind = OBJ_USERDATA,
    .add = luna_default_add, .sub = luna_default_sub, .mul = luna_default_mul, .div = luna_default_div, .mod = luna_default_mod,
    .neg = luna_default_neg, .cmp = luna_default_cmp,
    .getitem = luna_default_getitem, .setitem = luna_default_setitem,
    .getattr = luna_default_getattr, .setattr = luna_default_setattr,
    .call = luna_default_call, .tostring = luna_default_tostring, .hash = luna_default_hash, .len = luna_default_len
};

Type luna_vector_type = {
    .name = "vector", .kind = OBJ_VECTOR,
    .add = luna_vector_add, .sub = luna_vector_sub, .mul = luna_vector_mul, .div = luna_vector_div, .mod = luna_vector_mod,
    .neg = luna_vector_neg, .cmp = luna_vector_cmp,
    .getitem = luna_vector_getitem, .setitem = luna_vector_setitem,
    .getattr = luna_vector_getattr, .setattr = luna_vector_setattr,
    .call = luna_vector_call, .tostring = luna_vector_tostring, .hash = luna_vector_hash, .len = luna_vector_len
};

Type luna_matrix_type = {
    .name = "matrix", .kind = OBJ_MATRIX,
    .add = luna_matrix_add, .sub = luna_matrix_sub, .mul = luna_matrix_mul, .div = luna_matrix_div, .mod = luna_matrix_mod,
    .neg = luna_matrix_neg, .cmp = luna_matrix_cmp,
    .getitem = luna_matrix_getitem, .setitem = luna_matrix_setitem,
    .getattr = luna_matrix_getattr, .setattr = luna_matrix_setattr,
    .call = luna_matrix_call, .tostring = luna_matrix_tostring, .hash = luna_matrix_hash, .len = luna_matrix_len
};

/* Indexed by ObjType. */
Type *luna_types[] = {
    [OBJ_STRING]       = &luna_string_type,
    [OBJ_LIST]         = &luna_list_type,
    [OBJ_DICT]         = &luna_dict_type,
    [OBJ_INSTANCE]     = &luna_instance_type,
    [OBJ_FUNCTION]     = &luna_function_type,
    [OBJ_UPVALUE]      = &luna_upvalue_type,
    [OBJ_CLOSURE]      = &luna_closure_type,
    [OBJ_ENUM]         = &luna_enum_type,
    [OBJ_CLASS]        = &luna_class_type,
    [OBJ_BOUND_METHOD] = &luna_bound_method_type,
    [OBJ_MODULE]       = &luna_module_type,
    [OBJ_BUFFER]       = &luna_buffer_type,
    [OBJ_INT64]        = &luna_int64_type,
    [OBJ_USERDATA]     = &luna_userdata_type,
    [OBJ_VECTOR]       = &luna_vector_type,
    [OBJ_MATRIX]       = &luna_matrix_type,
};
