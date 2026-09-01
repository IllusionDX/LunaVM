#ifndef LUNA_PY_FRONTEND_STATE_H
#define LUNA_PY_FRONTEND_STATE_H

#include "vm.h"
#include "py/object.h"

/* Frontend-owned runtime state for the Python subset. Stored as an opaque
 * void* in the VM (vm->frontend_data): the core never names a py type.
 * Allocated in py_init_vm (calloc) and marked as roots by py_mark_roots. */
typedef struct PyFrontend {
    void *module_cache;

    /* Built-in exception classes */
    ObjClass *exception_class;
    ObjClass *type_error_class;
    ObjClass *key_error_class;
    ObjClass *index_error_class;
    ObjClass *attribute_error_class;
    ObjClass *value_error_class;
    ObjClass *runtime_error_class;
    ObjClass *argument_error_class;
    ObjClass *overflow_error_class;

    /* Canonical classes for built-in types */
    ObjClass *string_class;
    ObjClass *list_class;
    ObjClass *tuple_class;
    ObjClass *dict_class;
    ObjClass *enum_class;
    ObjClass *buffer_class;
    ObjClass *function_class;
    ObjClass *closure_class;
    ObjClass *bound_method_class;
    ObjClass *class_class;
    ObjClass *module_class;

    /* Virtual classes for immediate numbers (bool maps to int) */
    ObjClass *int_class;
    ObjClass *float_class;
} PyFrontend;

static inline PyFrontend *py_fe(VM *vm) {
    return (PyFrontend *)vm->frontend_data;
}

void vm_set_global(VM *vm, const char *name, Value value, bool is_const);

#endif /* LUNA_PY_FRONTEND_STATE_H */