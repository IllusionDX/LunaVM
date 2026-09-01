/* stdlib_enum.h — Python-style Enum support */

#ifndef STDLIB_ENUM_H
#define STDLIB_ENUM_H

#include "value.h"

void   enum_reset_auto(void);
Value  enum_get_auto_value(void);
void   enum_register_members(struct ObjClass *cls, const char **names, Value *values, int count);
void   vm_register_enum_module(VM *vm);

#endif /* STDLIB_ENUM_H */
