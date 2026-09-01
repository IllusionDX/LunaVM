#ifndef LUNA_PY_SLICE_H
#define LUNA_PY_SLICE_H

#include "value.h"
#include "py/object.h"

struct VM;

/* Construct a slice object. Bounds are validated int32/bigint or nil. */
ObjSlice *new_slice(Value start, Value stop, Value step);

/* slice() builtin entry point. Returns a PySlice object or null on error. */
Value py_builtin_slice(struct VM *vm, Value *args, int n);

/* Apply a slice key to a list/tuple/string; writes the sliced object to *out.
 * Returns false (with vm->last_exception set) on a bound overflow error. */
bool py_apply_slice(struct VM *vm, Value object, Value slicekey, Value *out);

/* repr rendering, e.g. "1:10:2". Caller frees the returned string. */
char *py_slice_to_cstr(Value self);

/* Lifecycle helpers wired from py_wire_lifecycle in object.c. */
void py_slice_free(Object *obj);
void py_slice_mark(struct VM *vm, Object *obj);

#endif /* LUNA_PY_SLICE_H */
