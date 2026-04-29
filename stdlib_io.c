/* stdlib_io.c — Stream utilities (Go-inspired).
 *
 * io.read_all(src)  — read all data from any source with read_all() method
 * io.copy(dst, src) — copy all data from src to dst using read()/write()
 * io.pipe()         — in-memory pipe, returns [reader, writer]
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "stdlib_io.h"
#include "value.h"

typedef struct PipeData {
    char *buf;
    size_t cap;
    size_t read_pos;
    size_t write_pos;
    bool closed;
} PipeData;

static char *dup_cstr(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) { fprintf(stderr, "OOM\n"); exit(1); }
    memcpy(out, s, len + 1);
    return out;
}

static void pipe_finalizer(void *data) {
    PipeData *pd = (PipeData*)data;
    if (!pd) return;
    free(pd->buf);
    free(pd);
}

/* ============================================================ */
/* io.read_all(src)                                              */
/* Calls src.read_all() if the instance has that method.         */
/* ============================================================ */

static Value io_read_all(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class,
            "io.read_all() expects exactly 1 argument");
    }
    if (!IS_INSTANCE(args[0])) {
        luna_throw(vm, vm->type_error_class,
            "io.read_all() requires an instance with a read_all method");
    }
    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
    if (!inst->klass) {
        luna_throw(vm, vm->runtime_error_class,
            "io.read_all(): object has no class");
    }
    for (int i = 0; i < inst->klass->method_count; i++) {
        if (strcmp(inst->klass->method_names[i], "read_all") == 0) {
            ObjFunction *m = inst->klass->methods[i];
            if (!m->is_native) {
                luna_throw(vm, vm->runtime_error_class,
                    "io.read_all(): read_all is not a native method");
            }
            Value call_args[2];
            call_args[0] = args[0];
            Value result;
            if (!vm_call_native(vm, m->native_fn, call_args, 1, &result))
                return make_null();
            return result;
        }
    }
    luna_throw(vm, vm->runtime_error_class,
        "io.read_all(): object has no read_all method");
    return make_null();
}

/* ============================================================ */
/* io.copy(dst, src)                                             */
/* Copies all data by calling src.read_all() + dst.write().      */
/* ============================================================ */

static Value io_copy(VM *vm, Value *args, int n) {
    if (n != 2) {
        luna_throw(vm, vm->argument_error_class,
            "io.copy() expects exactly 2 arguments (dst, src)");
    }
    Value data = io_read_all(vm, args + 1, 1);

    if (!IS_INSTANCE(args[0])) {
        luna_throw(vm, vm->type_error_class,
            "io.copy() destination must be an instance with a write method");
    }
    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
    if (!inst->klass) {
        luna_throw(vm, vm->runtime_error_class,
            "io.copy(): destination has no class");
    }
    for (int i = 0; i < inst->klass->method_count; i++) {
        if (strcmp(inst->klass->method_names[i], "write") == 0) {
            ObjFunction *m = inst->klass->methods[i];
            if (!m->is_native) {
                luna_throw(vm, vm->runtime_error_class,
                    "io.copy(): write is not a native method");
            }
            Value call_args[3];
            call_args[0] = args[0];
            call_args[1] = data;
            Value result;
            if (!vm_call_native(vm, m->native_fn, call_args, 2, &result))
                return make_null();
            return result;
        }
    }
    luna_throw(vm, vm->runtime_error_class,
        "io.copy(): destination has no write method");
    return make_null();
}

/* ============================================================ */
/* io.pipe() — in-memory pipe                                    */
/* Returns [reader, writer] — both are tagged userdata objs.     */
/* ============================================================ */

static Value pipe_read(VM *vm, Value *args, int n) {
    (void)vm;
    (void)n;
    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
    Value h = instance_get_field(inst, "_handle");
    if (!IS_USERDATA(h)) return make_null();
    ObjUserdata *ud = (ObjUserdata*)AS_OBJ(h);
    PipeData *pd = (PipeData*)ud->data;
    if (!pd || pd->closed) return make_null();
    if (pd->read_pos >= pd->write_pos) return make_null();
    return make_obj((Object*)new_string(pd->buf + pd->read_pos,
        (int)(pd->write_pos - pd->read_pos)));
}

static Value pipe_read_all(VM *vm, Value *args, int n) {
    (void)vm;
    (void)n;
    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
    Value h = instance_get_field(inst, "_handle");
    if (!IS_USERDATA(h)) return make_null();
    ObjUserdata *ud = (ObjUserdata*)AS_OBJ(h);
    PipeData *pd = (PipeData*)ud->data;
    if (!pd) return make_null();
    if (pd->read_pos >= pd->write_pos) {
        return make_obj((Object*)new_string("", 0));
    }
    size_t len = pd->write_pos - pd->read_pos;
    ObjString *s = new_string(pd->buf + pd->read_pos, (int)len);
    pd->read_pos = pd->write_pos;
    return make_obj((Object*)s);
}

static Value pipe_write(VM *vm, Value *args, int n) {
    if (n < 2) {
        luna_throw(vm, vm->argument_error_class,
            "pipe.write() requires a value argument");
    }
    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
    Value h = instance_get_field(inst, "_handle");
    if (!IS_USERDATA(h)) return make_null();
    ObjUserdata *ud = (ObjUserdata*)AS_OBJ(h);
    PipeData *pd = (PipeData*)ud->data;
    if (!pd) {
        luna_throw(vm, vm->runtime_error_class,
            "pipe.write(): pipe is closed");
    }

    char *text = value_to_string(args[1]);
    size_t len = strlen(text);
    size_t needed = pd->write_pos + len;
    if (needed > pd->cap) {
        size_t new_cap = pd->cap < 4096 ? 4096 : pd->cap * 2;
        while (new_cap < needed) new_cap *= 2;
        pd->buf = realloc(pd->buf, new_cap);
        pd->cap = new_cap;
    }
    memcpy(pd->buf + pd->write_pos, text, len);
    pd->write_pos += len;
    free(text);
    return make_int((int32_t)len);
}

static Value pipe_close(VM *vm, Value *args, int n) {
    (void)vm;
    (void)n;
    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
    Value h = instance_get_field(inst, "_handle");
    if (!IS_USERDATA(h)) return make_bool(false);
    ObjUserdata *ud = (ObjUserdata*)AS_OBJ(h);
    PipeData *pd = (PipeData*)ud->data;
    if (!pd) return make_bool(false);
    pd->closed = true;
    return make_bool(true);
}

static Value io_pipe(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, vm->argument_error_class,
            "io.pipe() takes no arguments");
    }

    PipeData *pd = calloc(1, sizeof(PipeData));
    pd->cap = 4096;
    pd->buf = malloc(pd->cap);
    pd->read_pos = 0;
    pd->write_pos = 0;
    pd->closed = false;

    ObjClass *pipe_reader_class = new_class("PipeReader", NULL);
    ObjClass *pipe_writer_class = new_class("PipeWriter", NULL);

    ObjFunction *fn;

    fn = new_native_function("read", pipe_read);
    pipe_reader_class->methods = malloc(sizeof(ObjFunction*));
    pipe_reader_class->method_names = malloc(sizeof(char*));
    pipe_reader_class->methods[0] = fn;
    pipe_reader_class->method_names[0] = dup_cstr("read");
    pipe_reader_class->method_count = 1;
    pipe_reader_class->method_capacity = 1;

    fn = new_native_function("read_all", pipe_read_all);
    pipe_reader_class->methods = realloc(pipe_reader_class->methods, sizeof(ObjFunction*) * 2);
    pipe_reader_class->method_names = realloc(pipe_reader_class->method_names, sizeof(char*) * 2);
    pipe_reader_class->methods[1] = fn;
    pipe_reader_class->method_names[1] = dup_cstr("read_all");
    pipe_reader_class->method_count = 2;
    pipe_reader_class->method_capacity = 2;

    fn = new_native_function("write", pipe_write);
    pipe_writer_class->methods = malloc(sizeof(ObjFunction*));
    pipe_writer_class->method_names = malloc(sizeof(char*));
    pipe_writer_class->methods[0] = fn;
    pipe_writer_class->method_names[0] = dup_cstr("write");
    pipe_writer_class->method_count = 1;
    pipe_writer_class->method_capacity = 1;

    fn = new_native_function("close", pipe_close);
    pipe_writer_class->methods = realloc(pipe_writer_class->methods, sizeof(ObjFunction*) * 2);
    pipe_writer_class->method_names = realloc(pipe_writer_class->method_names, sizeof(char*) * 2);
    pipe_writer_class->methods[1] = fn;
    pipe_writer_class->method_names[1] = dup_cstr("close");
    pipe_writer_class->method_count = 2;
    pipe_writer_class->method_capacity = 2;

    ObjUserdata *ud = new_userdata_tagged("io.Pipe", pd, pipe_finalizer);

    ObjInstance *reader = new_instance(pipe_reader_class, 4);
    instance_set_field(reader, "_handle", make_obj((Object*)ud));

    ObjInstance *writer = new_instance(pipe_writer_class, 4);
    instance_set_field(writer, "_handle", make_obj((Object*)ud));

    ObjList *pair = new_list(2);
    list_add(pair, make_obj((Object*)reader));
    list_add(pair, make_obj((Object*)writer));
    return make_obj((Object*)pair);
}

/* ============================================================ */
/* Module registration                                           */
/* ============================================================ */

void vm_register_io_module(VM *vm) {
    ObjModule *mod = new_module("io");

    /* Stream utilities */
    {
        ObjFunction *fn = new_native_function("read_all", io_read_all);
        dict_set(mod->exports,
                 make_obj((Object*)new_string("read_all", 8)),
                 make_obj((Object*)fn));
    }
    {
        ObjFunction *fn = new_native_function("copy", io_copy);
        dict_set(mod->exports,
                 make_obj((Object*)new_string("copy", 4)),
                 make_obj((Object*)fn));
    }
    {
        ObjFunction *fn = new_native_function("pipe", io_pipe);
        dict_set(mod->exports,
                 make_obj((Object*)new_string("pipe", 4)),
                 make_obj((Object*)fn));
    }

    dict_set(vm->module_cache,
             make_obj((Object*)new_string("io", 2)),
             make_obj((Object*)mod));
}
