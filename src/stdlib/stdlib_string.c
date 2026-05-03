/* stdlib_string.c — Built-in string module for Luna. */

#include <stdlib.h>
#include <string.h>
#include "stdlib_string.h"
#include "value.h"

static void module_add_const(ObjModule *mod, const char *name, const char *value) {
    dict_set(mod->exports,
             make_obj((Object*)new_string(name, (int)strlen(name))),
             make_obj((Object*)new_string(value, (int)strlen(value))));
}

void vm_register_string_module(VM *vm) {
    ObjModule *mod = new_module("string");

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
