#ifndef LUNA_LUNA_FRONTEND_STATE_H
#define LUNA_LUNA_FRONTEND_STATE_H

#include "vm.h"
#include "luna/object.h"

/* Frontend-owned runtime state for LunaScript. Stored as an opaque void* in
 * the VM (vm->frontend_data): the core never names a luna type. Allocated in
 * luna_init_vm (calloc) and marked as roots by luna_mark_roots. */
typedef struct LunaFrontend {
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

    /* Canonical classes for built-in types */
    ObjClass *string_class;
    ObjClass *list_class;
    ObjClass *dict_class;
    ObjClass *enum_class;
    ObjClass *buffer_class;
    ObjClass *vector_class;
    ObjClass *matrix_class;
    ObjClass *function_class;
    ObjClass *closure_class;
    ObjClass *bound_method_class;
    ObjClass *class_class;
    ObjClass *module_class;
    ObjClass *userdata_class;
} LunaFrontend;

static inline LunaFrontend *luna_fe(VM *vm) {
    return (LunaFrontend *)vm->frontend_data;
}

#endif /* LUNA_LUNA_FRONTEND_STATE_H */