#ifndef LUNA_PY_RANGE_H
#define LUNA_PY_RANGE_H

#include "value.h"
#include "py/object.h"

struct VM;

/* Lazy range object (start/stop/step only, no materialized elements). */
ObjRange *new_range(Value start, Value stop, Value step);
ObjRangeIter *new_range_iter(Value start, Value stop, Value step);

/* Advance a lazy range iterator by one element. Returns false at exhaustion
 * (or when step is zero); on success writes the next element to *elem. */
bool range_iter_next(struct VM *vm, ObjRangeIter *it, Value *elem);

/* Length of a range as a Value (int32 immediate or bigint). Returns
 * make_int(0) when step is zero. O(1) in time and memory. */
Value range_length_value(ObjRange *r);

/* Element at index `key` (start + key*step), O(1). Returns make_int(0) when
 * the index is out of range or step is zero (callers must pre-check). */
Value range_item(ObjRange *r, int64_t i64, Value ikey, bool is_big_index);

/* repr rendering, e.g. "range(0, 10, 2)". Caller frees the returned string. */
char *py_range_to_cstr(Value self);
char *py_range_iter_to_cstr(Value self);

/* MOP entry points. py_range_getitem raises IndexError out of range. */
Value py_range_getitem(struct VM *vm, Value self, Value key);
Value py_range_tostring(struct VM *vm, Value self);

/* Materialize a range into a fresh list (used by list()/tuple()). */
ObjList *list_from_range(ObjRange *r);

#endif /* LUNA_PY_RANGE_H */
