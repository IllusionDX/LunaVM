/* stdlib_time.h — Built-in time module for Luna. */
#ifndef LUNA_STDLIB_TIME_H
#define LUNA_STDLIB_TIME_H

#include "vm.h"

uint64_t luna_time_monotonic_us(void);
void vm_register_time_module(VM *vm);

#endif /* LUNA_STDLIB_TIME_H */
