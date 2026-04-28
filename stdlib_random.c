/* stdlib_random.c — Random number generation module for Luna.
 *
 * Polymorphic RNG: single Random class, multiple algorithms via state tag.
 * Algorithms are swappable; adding a new one only requires a step function
 * and a constructor — no method duplication.
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
/* Algorithm tag                                                 */
/* ============================================================ */

typedef enum {
    RNG_PCG32,
    RNG_XORSHIFT32
} RngAlgo;

static ObjClass *rng_class = NULL;

/* ============================================================ */
/* PCG32 algorithm                                               */
/* ============================================================ */

static uint32_t pcg32_step(uint64_t *state, uint64_t *inc) {
    uint64_t oldstate = *state;
    *state = oldstate * 6364136223846793005ULL + (*inc | 1);
    uint32_t xorshifted = (uint32_t)(((oldstate >> 18u) ^ oldstate) >> 27u);
    uint32_t rot = (uint32_t)(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

static uint32_t pcg32_step_from_list(ObjList *list) {
    uint32_t low  = (uint32_t)AS_INT(list_get(list, 1));
    uint32_t high = (uint32_t)AS_INT(list_get(list, 2));
    uint64_t state = ((uint64_t)high << 32) | low;

    low  = (uint32_t)AS_INT(list_get(list, 3));
    high = (uint32_t)AS_INT(list_get(list, 4));
    uint64_t inc = ((uint64_t)high << 32) | low;

    uint32_t raw = pcg32_step(&state, &inc);

    list_set(list, 1, make_int((int32_t)(state & 0xFFFFFFFF)));
    list_set(list, 2, make_int((int32_t)(state >> 32)));
    list_set(list, 3, make_int((int32_t)(inc & 0xFFFFFFFF)));
    list_set(list, 4, make_int((int32_t)(inc >> 32)));

    return raw;
}

static void pcg32_init(ObjList *list, uint64_t seed) {
    list_set(list, 1, make_int(0));
    list_set(list, 2, make_int(0));
    list_set(list, 3, make_int((int32_t)(seed & 0xFFFFFFFF)));
    list_set(list, 4, make_int((int32_t)(seed >> 32)));

    uint64_t state = 0;
    uint64_t inc   = seed;
    pcg32_step(&state, &inc);

    list_set(list, 1, make_int((int32_t)(state & 0xFFFFFFFF)));
    list_set(list, 2, make_int((int32_t)(state >> 32)));
}

/* ============================================================ */
/* Xorshift32 algorithm                                          */
/* ============================================================ */

static uint32_t xorshift32_step_from_list(ObjList *list) {
    uint32_t x = (uint32_t)AS_INT(list_get(list, 1));
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    list_set(list, 1, make_int((int32_t)x));
    return x;
}

static void xorshift32_init(ObjList *list, uint64_t seed) {
    uint32_t x = (uint32_t)(seed ? seed : 1);
    list_set(list, 1, make_int((int32_t)x));
}

/* ============================================================ */
/* Polymorphic dispatch                                          */
/* ============================================================ */

static uint32_t rng_step(ObjList *state_list) {
    int tag = AS_INT(list_get(state_list, 0));
    switch (tag) {
        case RNG_PCG32:      return pcg32_step_from_list(state_list);
        case RNG_XORSHIFT32: return xorshift32_step_from_list(state_list);
        default: return 0;
    }
}

static void rng_init(ObjList *state_list, RngAlgo algo, uint64_t seed) {
    list_set(state_list, 0, make_int((int32_t)algo));
    switch (algo) {
        case RNG_PCG32:      pcg32_init(state_list, seed); break;
        case RNG_XORSHIFT32: xorshift32_init(state_list, seed); break;
    }
}

/* ============================================================ */
/* Helpers                                                       */
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
/* Instance methods (shared by all algorithms)                   */
/* ============================================================ */

static Value rng_int(VM *vm, Value *args, int n) {
    if (n < 3) luna_throw(vm, vm->argument_error_class, "Random.int() requires 2 arguments: min, max");
    if (!IS_NUMBER(args[1])) luna_throw(vm, vm->type_error_class, "Random.int(): min must be numeric");
    if (!IS_NUMBER(args[2])) luna_throw(vm, vm->type_error_class, "Random.int(): max must be numeric");

    ObjList *state_list = get_state_list(args[0]);
    if (!state_list) luna_throw(vm, vm->runtime_error_class, "Random instance corrupted: missing _state");

    uint32_t raw = rng_step(state_list);

    int min = (int)AS_INT(args[1]);
    int max = (int)AS_INT(args[2]);
    if (min > max) { int t = min; min = max; max = t; }

    uint32_t range = (uint32_t)(max - min + 1);
    if (range == 0) return make_int((int32_t)raw);
    int result = min + (int)(raw % range);
    return make_int(result);
}

static Value rng_float(VM *vm, Value *args, int n) {
    if (n < 1) luna_throw(vm, vm->argument_error_class, "Random.float() called without instance");

    ObjList *state_list = get_state_list(args[0]);
    if (!state_list) luna_throw(vm, vm->runtime_error_class, "Random instance corrupted: missing _state");

    uint32_t raw = rng_step(state_list);
    double normalized = (double)raw / (double)UINT32_MAX;

    if (n == 1) {
        return make_double(normalized);
    }
    if (n >= 3) {
        if (!IS_NUMBER(args[1])) luna_throw(vm, vm->type_error_class, "Random.float(): min must be numeric");
        if (!IS_NUMBER(args[2])) luna_throw(vm, vm->type_error_class, "Random.float(): max must be numeric");
        double min = value_to_double(args[1]);
        double max = value_to_double(args[2]);
        return make_double(min + normalized * (max - min));
    }

    luna_throw(vm, vm->argument_error_class, "Random.float() takes 0 or 2 arguments");
    return make_null(); /* unreachable */
}

static Value rng_seed(VM *vm, Value *args, int n) {
    if (n < 2) luna_throw(vm, vm->argument_error_class, "Random.seed() requires 1 argument");
    if (!IS_NUMBER(args[1])) luna_throw(vm, vm->type_error_class, "Random.seed(): seed must be numeric");

    ObjList *state_list = get_state_list(args[0]);
    if (!state_list) luna_throw(vm, vm->runtime_error_class, "Random instance corrupted: missing _state");

    uint64_t new_seed = (uint64_t)AS_INT(args[1]);
    int tag = AS_INT(list_get(state_list, 0));
    rng_init(state_list, (RngAlgo)tag, new_seed);

    return make_null();
}

static Value rng_call(VM *vm, Value *args, int n) {
    if (n != 1 && n != 3) {
        luna_throw(vm, vm->argument_error_class, "Random() takes 0 or 2 arguments");
    }

    ObjList *state_list = get_state_list(args[0]);
    if (!state_list) luna_throw(vm, vm->runtime_error_class, "Random instance corrupted: missing _state");

    uint32_t raw = rng_step(state_list);

    if (n == 1) {
        double normalized = (double)raw / (double)UINT32_MAX;
        return make_double(normalized);
    }

    if (!IS_NUMBER(args[1])) luna_throw(vm, vm->type_error_class, "Random(): first argument must be numeric");
    if (!IS_NUMBER(args[2])) luna_throw(vm, vm->type_error_class, "Random(): second argument must be numeric");

    bool is_int_range = IS_INT(args[1]) && IS_INT(args[2]);
    if (is_int_range) {
        int min = AS_INT(args[1]);
        int max = AS_INT(args[2]);
        if (min > max) { int t = min; min = max; max = t; }
        uint32_t range = (uint32_t)(max - min + 1);
        if (range == 0) return make_int((int32_t)raw);
        int result = min + (int)(raw % range);
        return make_int(result);
    }

    double min = value_to_double(args[1]);
    double max = value_to_double(args[2]);
    double normalized = (double)raw / (double)UINT32_MAX;
    return make_double(min + normalized * (max - min));
}

/* ============================================================ */
/* Constructors                                                  */
/* ============================================================ */

static Value make_rng_instance(VM *vm, RngAlgo algo, uint64_t seed) {
    (void)vm;
    ObjInstance *inst = new_instance(rng_class, 4);

    int state_slots = (algo == RNG_PCG32) ? 5 : 2;
    ObjList *state_list = new_list(state_slots);
    retain_obj((Object*)state_list);

    for (int i = 0; i < state_slots; i++) list_add(state_list, make_int(0));

    rng_init(state_list, algo, seed);

    instance_set_field(inst, "_state", make_obj((Object*)state_list));
    release_obj((Object*)state_list);

    return make_obj((Object*)inst);
}

static Value random_PCG(VM *vm, Value *args, int n) {
    uint64_t seed = 0;
    if (n >= 1) {
        if (!IS_NUMBER(args[0])) luna_throw(vm, vm->type_error_class, "PCG(): seed must be numeric");
        seed = (uint64_t)AS_INT(args[0]);
    } else {
        seed = (uint64_t)time(NULL);
    }
    return make_rng_instance(vm, RNG_PCG32, seed);
}

static Value random_Xorshift(VM *vm, Value *args, int n) {
    uint64_t seed = 0;
    if (n >= 1) {
        if (!IS_NUMBER(args[0])) luna_throw(vm, vm->type_error_class, "Xorshift(): seed must be numeric");
        seed = (uint64_t)AS_INT(args[0]);
    } else {
        seed = (uint64_t)time(NULL);
    }
    return make_rng_instance(vm, RNG_XORSHIFT32, seed);
}

/* ============================================================ */
/* rng.choice(seq)                                               */
/* ============================================================ */

static Value rng_choice(VM *vm, Value *args, int n) {
    if (n < 2) luna_throw(vm, vm->argument_error_class, "choice() requires a sequence argument");

    ObjList *state_list = get_state_list(args[0]);
    if (!state_list) luna_throw(vm, vm->runtime_error_class, "Random instance corrupted: missing _state");

    Value seq = args[1];
    if (!IS_OBJ(seq)) luna_throw(vm, vm->type_error_class, "choice(): argument must be a list or string");

    Object *obj = AS_OBJ(seq);
    if (obj->type == OBJ_LIST) {
        ObjList *list = (ObjList*)obj;
        int len = list_length(list);
        if (len == 0) luna_throw(vm, vm->value_error_class, "choice(): list is empty");
        uint32_t raw = rng_step(state_list);
        int idx = (int)(raw % (uint32_t)len);
        return list_get(list, idx);
    }
    if (obj->type == OBJ_STRING) {
        ObjString *str = (ObjString*)obj;
        if (str->length == 0) luna_throw(vm, vm->value_error_class, "choice(): string is empty");
        uint32_t raw = rng_step(state_list);
        int idx = (int)(raw % (uint32_t)str->length);
        return make_obj((Object*)new_string(&str->chars[idx], 1));
    }

    luna_throw(vm, vm->type_error_class, "choice(): argument must be a list or string");
    return make_null(); /* unreachable */
}

/* ============================================================ */
/* Module registration                                           */
/* ============================================================ */

void vm_register_random_module(VM *vm) {
    rng_class = new_class("Random", NULL);
    retain_obj((Object*)rng_class);

    class_add_native_method(rng_class, "int",    rng_int);
    class_add_native_method(rng_class, "float",  rng_float);
    class_add_native_method(rng_class, "seed",   rng_seed);
    class_add_native_method(rng_class, "choice", rng_choice);
    class_add_native_method(rng_class, "_call",  rng_call);

    ObjModule *mod = new_module("random");

    /* random.PCG(seed) constructor */
    ObjFunction *pcg_ctor = new_native_function("PCG", random_PCG);
    retain_obj((Object*)pcg_ctor);
    ObjString *pcg_key = new_string("PCG", 3);
    retain_obj((Object*)pcg_key);
    dict_set(mod->exports, make_obj((Object*)pcg_key), make_obj((Object*)pcg_ctor));
    release_obj((Object*)pcg_key);
    release_obj((Object*)pcg_ctor);

    /* random.Xorshift(seed) constructor */
    ObjFunction *xs_ctor = new_native_function("Xorshift", random_Xorshift);
    retain_obj((Object*)xs_ctor);
    ObjString *xs_key = new_string("Xorshift", 8);
    retain_obj((Object*)xs_key);
    dict_set(mod->exports, make_obj((Object*)xs_key), make_obj((Object*)xs_ctor));
    release_obj((Object*)xs_key);
    release_obj((Object*)xs_ctor);

    /* Cache module */
    Value mod_val = make_obj((Object*)mod);
    dict_set(vm->module_cache,
             make_obj((Object*)new_string("random", 6)),
             mod_val);
}
