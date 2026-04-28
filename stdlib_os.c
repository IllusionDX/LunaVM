/* stdlib_os.c — Built-in os module for Luna.
 *
 * os.args()     -> Full argv list from the host process
 * os.env(key)   -> Read an environment variable
 * os.exit(code) -> Exit the host process
 * os.platform() -> "win32" or "posix"
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "stdlib_os.h"
#include "value.h"

static inline double checked_number(VM *vm, Value v, const char *fn) {
    if (!IS_NUMBER(v)) {
        luna_throw(vm, vm->type_error_class, "%s() argument must be numeric", fn);
    }
    return value_to_double(v);
}

static void module_add_native(ObjModule *mod, const char *name, NativeFn fn) {
    ObjFunction *f = new_native_function(name, fn);
    Value key = make_obj((Object*)new_string(name, (int)strlen(name)));
    dict_set(mod->exports, key, make_obj((Object*)f));
}

static Value os_args(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, vm->argument_error_class, "os.args() takes no arguments");
    }

    ObjList *list = new_list(vm->process_argc);
    for (int i = 0; i < vm->process_argc; i++) {
        const char *s = vm->process_argv && vm->process_argv[i] ? vm->process_argv[i] : "";
        list_add(list, make_obj((Object*)new_string(s, (int)strlen(s))));
    }
    return make_obj((Object*)list);
}

static Value os_env(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "os.env() expects exactly 1 argument");
    }
    if (!IS_STRING(args[0])) {
        luna_throw(vm, vm->type_error_class, "os.env() argument must be a string");
    }
    const char *key = ((ObjString*)AS_OBJ(args[0]))->chars;
    const char *val = getenv(key);
    if (!val) return make_null();
    return make_obj((Object*)new_string(val, (int)strlen(val)));
}

static Value os_exit(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "os.exit() expects exactly 1 argument");
    }
    int code = (int)checked_number(vm, args[0], "os.exit");
    fflush(stdout);
    fflush(stderr);
    exit(code);
    return make_null();
}

static Value os_platform(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, vm->argument_error_class, "os.platform() takes no arguments");
    }
#ifdef _WIN32
    return make_obj((Object*)new_string("win32", 5));
#else
    return make_obj((Object*)new_string("posix", 5));
#endif
}

void vm_register_os_module(VM *vm) {
    ObjModule *mod = new_module("os");
    module_add_native(mod, "args", os_args);
    module_add_native(mod, "env", os_env);
    module_add_native(mod, "exit", os_exit);
    module_add_native(mod, "platform", os_platform);

    dict_set(vm->module_cache,
             make_obj((Object*)new_string("os", 2)),
             make_obj((Object*)mod));
}
