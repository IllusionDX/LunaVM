/* stdlib_string.c — Built-in string module for Luna.
 *
 * string.from_byte(n) -> 1-byte string
 */

#include <stdlib.h>
#include <string.h>
#include "stdlib_string.h"
#include "value.h"

static void module_add_native(ObjModule *mod, const char *name, NativeFn fn) {
    ObjFunction *f = new_native_function(name, fn);
    ObjString *key = new_string(name, (int)strlen(name));
    dict_set(mod->exports, make_obj((Object *)key), make_obj((Object *)f));
}

static Value string_from_byte(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "string.from_byte() expects exactly 1 argument");
    }
    if (!IS_INT(args[0]) && !IS_INT64(args[0])) {
        luna_throw(vm, vm->type_error_class, "string.from_byte() argument must be an integer");
    }
    int64_t v = as_int64(args[0]);
    if (v < 0 || v > 255) {
        luna_throw(vm, vm->value_error_class, "string.from_byte() argument must be in range 0..255");
    }
    char c = (char)(uint8_t)v;
    return make_obj((Object*)new_string(&c, 1));
}

static void module_add_const(ObjModule *mod, const char *name, const char *value) {
    dict_set(mod->exports,
             make_obj((Object*)new_string(name, (int)strlen(name))),
             make_obj((Object*)new_string(value, (int)strlen(value))));
}

void vm_register_string_module(VM *vm) {
    ObjModule *mod = new_module("string");

    module_add_native(mod, "from_byte", string_from_byte);

    module_add_const(mod, "ascii_letters",
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
    module_add_const(mod, "digits",
        "0123456789");
    module_add_const(mod, "punctuation",
        "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~");
    module_add_const(mod, "whitespace",
        " \t\n\r\x0b\x0c");
    module_add_const(mod, "hexdigits",
        "0123456789abcdefABCDEF");

    Value mod_val = make_obj((Object *)mod);
    ObjString *key = new_string("string", 6);
    dict_set(vm->module_cache, make_obj((Object *)key), mod_val);
}
