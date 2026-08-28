/* value.c — language-agnostic VM value helpers + GC bookkeeping.
 *
 * The Luna object runtime (constructors, per-type ops, string interning,
 * value_to_string / values_equal / hash_value, free/mark) lives in
 * luna/object.c so the core stays decoupled from the Luna frontend.
 * See docs/architecture.md Part 6. */

#include "value.h"

/* GC bookkeeping (frontend-independent). Declared extern in value.h. */
Object *all_objects = NULL;
int allocated_objects = 0;
size_t bytes_allocated = 0;
size_t next_gc_threshold = 64 * 1024 * 1024;
int gc_state = GC_STATE_IDLE;
Object *userdata_objects = NULL;
Object *sweep_cursor = NULL;

bool is_null(Value v) { return IS_NIL(v); }
