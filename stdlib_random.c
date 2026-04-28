/* stdlib_random.c — Random number generation module for Luna.
 *
 * PCG32 instance-based RNG using ObjInstance + ObjList for state storage.
 * No new ObjType needed — reuses existing class/instance/list machinery.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "vm.h"
#include "value.h"
#include "stdlib_random.h"

/* ============================================================ */
/* PCG32 algorithm (64-bit state split into two 32-bit ints)    */
/* ============================================================ */

static ObjClass *pcg_class = NULL;

static uint32_t pcg32_step(uint64_t *state, uint64_t *inc) {
    uint64_t oldstate = *state;
    *state = oldstate * 6364136223846793005ULL + (*inc | 1);
    uint32_t xorshifted = (uint32_t)(((oldstate >> 18u) ^ oldstate) >> 27u);
    uint32_t rot = (uint32_t)(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

/* Read 64-bit value from two consecutive 32-bit list slots */
static uint64_t read_u64(ObjList *list, int idx) {
    uint32_t low  = (uint32_t)AS_INT(list_get(list, idx));
    uint32_t high = (uint32_t)AS_INT(list_get(list, idx + 1));
    return ((uint64_t)high << 32) | low;
}

/* Write 64-bit value to two consecutive 32-bit list slots */
static void write_u64(ObjList *list, int idx, uint64_t val) {
    list_set(list, idx,     make_int((int32_t)(val & 0xFFFFFFFF)));
    list_set(list, idx + 1, make_int((int32_t)(val >> 32)));
}

/* ============================================================ */
/* Helpers                                                      */
/* ============================================================ */

static void class_add_native_method(ObjClass *cls, const char *name, NativeFn fn) {
    if (cls->method_count >= cls->method_capacity) {
        int new_cap = cls->method_capacity < 4 ? 4 : cls->method_capacity * 2;
        cls->methods = realloc(cls->methods, sizeof(ObjFunction*) * new_cap);
        cls->method_names = realloc(cls->method_names, sizeof(char*) * new_cap);
        cls->method_capacity = new_cap;
    }
    ObjFunction *f = new_native_function(name, fn);
    cls->methods[cls->method_count] = f;
    cls->method_names[cls->method_count] = strdup(name);
    cls->method_count++;
}

static ObjList *get_state_list(Value self) {
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    Value state_val = instance_get_field(inst, "_state");
    if (!IS_OBJ(state_val) || AS_OBJ(state_val)->type != OBJ_LIST) {
        return NULL;
    }
    return (ObjList*)AS_OBJ(state_val);
}

/* ============================================================ */
/* PCG instance methods                                         */
/* ============================================================ */

static Value pcg_int(VM *vm, Value *args, int n) {
    if (n < 3) luna_throw(vm, vm->argument_error_class, "PCG.int() requires 2 arguments: min, max");
    if (!IS_NUMBER(args[1])) luna_throw(vm, vm->type_error_class, "PCG.int(): min must be numeric");
    if (!IS_NUMBER(args[2])) luna_throw(vm, vm->type_error_class, "PCG.int(): max must be numeric");

    ObjList *state_list = get_state_list(args[0]);
    if (!state_list) luna_throw(vm, vm->runtime_error_class, "PCG instance corrupted: missing _state");

    uint64_t state = read_u64(state_list, 0);
    uint64_t inc   = read_u64(state_list, 2);
    uint32_t raw   = pcg32_step(&state, &inc);
    write_u64(state_list, 0, state);
    write_u64(state_list, 2, inc);

    int min = (int)AS_INT(args[1]);
    int max = (int)AS_INT(args[2]);
    if (min > max) { int t = min; min = max; max = t; }

    uint32_t range = (uint32_t)(max - min + 1);
    if (range == 0) {
        /* Full uint32_t range — modulo would divide by zero */
        return make_int((int32_t)raw);
    }
    int result = min + (int)(raw % range);
    return make_int(result);
}

static Value pcg_float(VM *vm, Value *args, int n) {
    if (n < 1) luna_throw(vm, vm->argument_error_class, "PCG.float() called without instance");

    ObjList *state_list = get_state_list(args[0]);
    if (!state_list) luna_throw(vm, vm->runtime_error_class, "PCG instance corrupted: missing _state");

    uint64_t state = read_u64(state_list, 0);
    uint64_t inc   = read_u64(state_list, 2);
    uint32_t raw   = pcg32_step(&state, &inc);
    write_u64(state_list, 0, state);
    write_u64(state_list, 2, inc);

    /* [0.0, 1.0) — never returns exactly 1.0 */
    double normalized = (double)raw / ((double)UINT32_MAX + 1.0);

    if (n == 1) {
        return make_double(normalized);
    }
    if (n >= 3) {
        if (!IS_NUMBER(args[1])) luna_throw(vm, vm->type_error_class, "PCG.float(): min must be numeric");
        if (!IS_NUMBER(args[2])) luna_throw(vm, vm->type_error_class, "PCG.float(): max must be numeric");
        double min = value_to_double(args[1]);
        double max = value_to_double(args[2]);
        return make_double(min + normalized * (max - min));
    }

    luna_throw(vm, vm->argument_error_class, "PCG.float() takes 0 or 2 arguments");
    return make_null(); /* unreachable */
}

static Value pcg_seed(VM *vm, Value *args, int n) {
    if (n < 2) luna_throw(vm, vm->argument_error_class, "PCG.seed() requires 1 argument");
    if (!IS_NUMBER(args[1])) luna_throw(vm, vm->type_error_class, "PCG.seed(): seed must be numeric");

    ObjList *state_list = get_state_list(args[0]);
    if (!state_list) luna_throw(vm, vm->runtime_error_class, "PCG instance corrupted: missing _state");

    uint64_t new_seed = (uint64_t)AS_INT(args[1]);
    /* Standard PCG init: state = 0, inc = seed, then step once */
    write_u64(state_list, 0, 0);
    write_u64(state_list, 2, new_seed);

    uint64_t state = 0;
    uint64_t inc   = new_seed;
    pcg32_step(&state, &inc);
    write_u64(state_list, 0, state);

    return make_null();
}

/* ============================================================ */
/* Constructor                                                  */
/* ============================================================ */

static Value random_PCG(VM *vm, Value *args, int n) {
    (void)vm;
    uint64_t seed = 0;
    if (n >= 1) {
        if (!IS_NUMBER(args[0])) luna_throw(vm, vm->type_error_class, "PCG(): seed must be numeric");
        seed = (uint64_t)AS_INT(args[0]);
    } else {
        seed = (uint64_t)time(NULL);
    }

    ObjInstance *inst = new_instance(pcg_class, 4);

    /* State layout: [state_low, state_high, inc_low, inc_high] */
    ObjList *state_list = new_list(4);
    retain_obj((Object*)state_list);

    /* Pre-fill 4 slots so list_set works (list count starts at 0) */
    for (int i = 0; i < 4; i++) list_add(state_list, make_int(0));

    /* PCG standard initialization */
    write_u64(state_list, 0, 0);      /* state = 0 */
    write_u64(state_list, 2, seed);   /* inc = seed */

    uint64_t state = 0;
    uint64_t inc   = seed;
    pcg32_step(&state, &inc);         /* advance once */
    write_u64(state_list, 0, state);

    instance_set_field(inst, "_state", make_obj((Object*)state_list));
    release_obj((Object*)state_list);

    return make_obj((Object*)inst);
}

/* ============================================================ */
/* Module registration                                          */
/* ============================================================ */

void vm_register_random_module(VM *vm) {
    pcg_class = new_class("PCG", NULL);
    retain_obj((Object*)pcg_class);

    class_add_native_method(pcg_class, "int",   pcg_int);
    class_add_native_method(pcg_class, "float", pcg_float);
    class_add_native_method(pcg_class, "seed",  pcg_seed);

    ObjModule *mod = new_module("random");

    /* random.PCG(seed) constructor */
    ObjFunction *ctor = new_native_function("PCG", random_PCG);
    retain_obj((Object*)ctor);
    ObjString *pcg_key = new_string("PCG", 3);
    retain_obj((Object*)pcg_key);
    dict_set(mod->exports,
             make_obj((Object*)pcg_key),
             make_obj((Object*)ctor));
    release_obj((Object*)pcg_key);
    release_obj((Object*)ctor);

    /* Cache module */
    Value mod_val = make_obj((Object*)mod);
    dict_set(vm->module_cache,
             make_obj((Object*)new_string("random", 6)),
             mod_val);
}
