#ifndef LUNA_PY_NUMBER_METHODS_H
#define LUNA_PY_NUMBER_METHODS_H

struct VM;

/* Registers the virtual int/float classes into the py frontend state
 * (py_fe(vm)->int_class / float_class) with their native methods.
 * Called from py_init_vm. */
void py_register_number_methods(struct VM *vm);

#endif /* LUNA_PY_NUMBER_METHODS_H */