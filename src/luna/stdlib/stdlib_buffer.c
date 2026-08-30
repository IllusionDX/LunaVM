/* stdlib_buffer.c — Built-in buffer module for Luna.
 *
 * buffer.new(capacity?)     -> empty mutable buffer
 * buffer.from_string(text)  -> copy bytes from a string
 */

#include <stdlib.h>
#include <string.h>
#include "stdlib_buffer.h"
#include "value.h"
#include "luna/object.h"

static Value buffer_new(VM *vm, Value *args, int n) {
    if (n > 1) {
        luna_throw(vm, vm->argument_error_class, "buffer.new() expects at most 1 argument");
    }
    size_t capacity = 0;
    if (n == 1) {
        if (!IS_INT(args[0]) && !IS_INT64(args[0])) {
            luna_throw(vm, vm->type_error_class, "buffer.new() capacity must be an integer");
        }
        int64_t cap = as_int64(args[0]);
        if (cap < 0) {
            luna_throw(vm, vm->value_error_class, "buffer.new() capacity must be non-negative");
        }
        capacity = (size_t)cap;
    }
    return make_obj((Object*)new_buffer(capacity));
}

static Value buffer_from_string(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "buffer.from_string() expects exactly 1 argument");
    }
    if (!IS_STRING(args[0])) {
        luna_throw(vm, vm->type_error_class, "buffer.from_string() argument must be a string");
    }
    ObjString *str = (ObjString*)AS_OBJ(args[0]);
    ObjBuffer *buf = new_buffer((size_t)str->length);
    buffer_append_data(buf, (const uint8_t*)str->chars, (size_t)str->length);
    return make_obj((Object*)buf);
}

void vm_register_buffer_module(VM *vm) {
    ObjModule *mod = new_module("buffer");

    /* Export new/from_string directly on the module */
    ObjFunction *fn = new_native_function("new", buffer_new);
    dict_set(mod->exports,
             make_obj((Object*)new_string("new", 3)),
             make_obj((Object*)fn));

    fn = new_native_function("from_string", buffer_from_string);
    dict_set(mod->exports,
             make_obj((Object*)new_string("from_string", 11)),
             make_obj((Object*)fn));

    Value mod_val = make_obj((Object *)mod);
    ObjString *mod_key = new_string("buffer", 6);
    dict_set(vm->module_cache, make_obj((Object *)mod_key), mod_val);
}
