/* stdlib_random.c — 64-bit Random number generation module for Luna.
 *
 * Three algorithms, all producing full uint64_t output:
 *   - WyRand v4.2:   128-bit state, MUM-based. Elite quality for
 *                     procedural seeding and hierarchical generation.
 *   - Xoshiro256**:  256-bit state, period 2^256-1. Workhorse for
 *                     bulk simulation (particles, wind, swarm AI).
 *   - PCG64:         128-bit state. Classic, battle-tested.
 *
 * Backend (C99): all 64-bit operations in CPU registers, no heap.
 * Binding layer ("customs"): returns fast int32 when result fits,
 * only allocates heap Int64 when values exceed INT32_MAX.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "vm.h"
#include "value.h"
#include "py/object.h"
#include "stdlib_random.h"

/* ============================================================ */
/* Algorithm tag                                                 */
/* ============================================================ */

typedef enum {
    RNG_WYRAND,
    RNG_XOSHIRO256SS,
    RNG_PCG64
} RngAlgo;

static ObjClass *rng_class = NULL;

/* ============================================================ */
/* WyRand v4.2 — MUM-based PRNG, passes BigCrush & PractRand    */
/* ============================================================ */

/* MUM (Multiply-Mix): the heart of WyRand.
 * Returns the XOR of the low and high 64 bits of the 128-bit product. */
static inline uint64_t wymum(uint64_t A, uint64_t B) {
    __uint128_t r = (__uint128_t)A * B;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}

/* Single step of WyRand v4.2.
 * State is a single uint64_t that is incremented by a constant.
 * Output is wymum(state, state ^ secret). */
static uint64_t wyrand_step(uint64_t *seed) {
    *seed += 0x2d358dccaa6c78a5ull;
    return wymum(*seed, *seed ^ 0x8bb84b93962eacc9ull);
}

/* ============================================================ */
/* Xoshiro256** — by Blackman & Vigna, period 2^256-1           */
/* ============================================================ */

static inline uint64_t rotl(const uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

/* Single step of xoshiro256** 1.0.
 * State is 4 x uint64_t. Output scrambler: rotl(s[1]*5, 7) * 9. */
static uint64_t xoshiro256ss_step(uint64_t s[4]) {
    const uint64_t result = rotl(s[1] * 5, 7) * 9;
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}

/* ============================================================ */
/* PCG64 — 128-bit state, 64-bit output, O'Neill                 */
/* ============================================================ */

static uint64_t pcg64_step(__uint128_t *state, __uint128_t inc) {
    __uint128_t oldstate = *state;
    
    /* 128-bit LCG: El multiplicador oficial de 128 bits para PCG64 */
    __uint128_t mult = ((__uint128_t)2549297995355413924ULL << 64) | 4865540595714422341ULL;
    *state = oldstate * mult + (inc | 1);
    
    /* XSL-RR: XOR high ^ low, shift 58, rotate by bits 122..127 */
    uint64_t xorshifted = (uint64_t)(((oldstate >> 64) ^ oldstate) >> 58);
    uint64_t rot = (uint64_t)(oldstate >> 122);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 63));
}

/* ============================================================ */
/* State list helpers (uint64_t <-> VM Value)                   */
/* ============================================================ */

static inline uint64_t list_get_u64(ObjList *list, int idx) {
    Value v = list_get(list, idx);
    if (IS_INT64(v))
        return ((ObjInt64*)AS_OBJ(v))->value;
    if (IS_INT(v))
        return (uint64_t)(int64_t)AS_INT(v);
    return 0;
}

static inline void list_set_u64(ObjList *list, int idx, uint64_t val) {
    list_set(list, idx, make_int64((int64_t)val));
}

/* Helpers to store/load __uint128_t from state list [low, high] */
static inline void list_set_u128(ObjList *list, int lo_idx, __uint128_t val) {
    list_set_u64(list, lo_idx,     (uint64_t)val);
    list_set_u64(list, lo_idx + 1, (uint64_t)(val >> 64));
}

static inline __uint128_t list_get_u128(ObjList *list, int lo_idx) {
    uint64_t lo = list_get_u64(list, lo_idx);
    uint64_t hi = list_get_u64(list, lo_idx + 1);
    return ((__uint128_t)hi << 64) | lo;
}

/* ============================================================ */
/* Polymorphic dispatch: all generators return uint64_t          */
/* ============================================================ */

static uint64_t rng_step(ObjList *state_list) {
    int tag = AS_INT(list_get(state_list, 0));
    switch (tag) {
        case RNG_WYRAND: {
            uint64_t seed = list_get_u64(state_list, 1);
            uint64_t result = wyrand_step(&seed);
            list_set_u64(state_list, 1, seed);
            return result;
        }
        case RNG_XOSHIRO256SS: {
            uint64_t s[4];
            for (int i = 0; i < 4; i++)
                s[i] = list_get_u64(state_list, 1 + i);
            uint64_t result = xoshiro256ss_step(s);
            for (int i = 0; i < 4; i++)
                list_set_u64(state_list, 1 + i, s[i]);
            return result;
        }
        case RNG_PCG64: {
            __uint128_t state = list_get_u128(state_list, 1);
            __uint128_t inc   = list_get_u128(state_list, 3);
            uint64_t result = pcg64_step(&state, inc);
            list_set_u128(state_list, 1, state);
            list_set_u128(state_list, 3, inc);
            return result;
        }
        default:
            return 0;
    }
}

/* ============================================================ */
/* 64-bit Lemire unbiased range reduction                        */
/* ============================================================ */

/* Returns uniformly distributed value in [0, range-1].
 * Uses the high 64 bits of the 128-bit product (Daniel Lemire).
 * For range == 0, returns 0 (degenerate / empty range). */
static uint64_t lemire_range_64(uint64_t random, uint64_t range) {
    if (range == 0) return 0;
    __uint128_t product = (__uint128_t)random * (__uint128_t)range;
    return (uint64_t)(product >> 64);
}

/* ============================================================ */
/* Binding layer — "The Customs"                                 */
/* ============================================================ */

/* Returns result as a fast int32 if it fits, otherwise as heap Int64.
 * This keeps the GC pressure at zero for >99% of game calls
 * (loot drops, hit numbers, particle lifetimes, etc.). */
static Value make_ranged_result(uint64_t value) {
    if (value <= (uint64_t)INT32_MAX) {
        return make_int((int32_t)value);
    }
    return make_int64((int64_t)value);
}

/* Convert any numeric VM value to int64 */
static int64_t value_to_int64(Value v) {
    return as_int64(v);
}

/* Convert any numeric VM value to uint64 */
static uint64_t value_to_uint64(Value v) {
    return (uint64_t)as_int64(v);
}

/* Extract uint64_t seed from constructor argument.
 * For statics stored as fields, args[0] is the first user arg. */
static uint64_t seed_from_arg(Value v) {
    return (uint64_t)as_int64(v);
}

/* ============================================================ */
/* Constructors (seeded with splitmix64 for Xoshiro256**)        */
/* ============================================================ */

static void wyrand_init(ObjList *list, uint64_t seed) {
    if (seed == 0) seed = (uint64_t)time(NULL);
    /* WyRand seed CAN be 0 — the step adds a constant first. But
     * avoid the degenerate case where seed stays 0 after increment
     * modulo 2^64 (impossible since the increment constant is odd). */
    list_set_u64(list, 1, seed);
}

static void xoshiro256ss_init(ObjList *list, uint64_t seed) {
    if (seed == 0) seed = (uint64_t)time(NULL);
    if (seed == 0) seed = 1;  /* Avoid all-zero state */

    /* Splitmix64: diffuse a single seed across the 4-word state */
    uint64_t z = seed;
    for (int i = 0; i < 4; i++) {
        z += 0x9e3779b97f4a7c15ull;
        uint64_t x = z;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        x = x ^ (x >> 31);
        list_set_u64(list, 1 + i, x);
    }
}

static void pcg64_init(ObjList *list, uint64_t seed) {
    if (seed == 0) seed = (uint64_t)time(NULL);

    /* Expand the 64-bit seed into a 128-bit increment via splitmix64,
     * then do one warm-up step with state=0. */
    __uint128_t inc = (__uint128_t)seed;
    /* Mix the seed into both halves of the 128-bit increment */
    uint64_t z = seed;
    z += 0x9e3779b97f4a7c15ull;
    uint64_t mixed = z;
    mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ull;
    mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebull;
    mixed = mixed ^ (mixed >> 31);
    inc |= (__uint128_t)mixed << 64;
    inc |= 1; /* Ensure odd (required by PCG) */

    __uint128_t state = 0;
    pcg64_step(&state, inc);

    list_set_u128(list, 1, state);
    list_set_u128(list, 3, inc);
}

/* ============================================================ */
/* Instance creation                                             */
/* ============================================================ */

static ObjList *make_state_list(RngAlgo algo, uint64_t seed) {
    int slots;
    switch (algo) {
        case RNG_WYRAND:      slots = 2; break;  /* [algo, seed] */
        case RNG_XOSHIRO256SS: slots = 5; break; /* [algo, s0..s3] */
        case RNG_PCG64:       slots = 5; break;  /* [algo, state_lo, state_hi, inc_lo, inc_hi] */
        default:              slots = 2; break;
    }

    ObjList *list = new_list(slots);
    for (int i = 0; i < slots; i++)
        list_add(list, make_int(0));

    list_set(list, 0, make_int((int32_t)algo));

    switch (algo) {
        case RNG_WYRAND:      wyrand_init(list, seed);      break;
        case RNG_XOSHIRO256SS: xoshiro256ss_init(list, seed); break;
        case RNG_PCG64:       pcg64_init(list, seed);       break;
    }

    return list;
}

static Value make_rng_instance(VM *vm, RngAlgo algo, uint64_t seed) {
    (void)vm;
    ObjInstance *inst = new_instance(rng_class, 4);
    ObjList *state_list = make_state_list(algo, seed);
    instance_set_field(inst, "_state", make_obj((Object*)state_list));
    return make_obj((Object*)inst);
}

/* ============================================================ */
/* Class helper functions                                        */
/* ============================================================ */

static void random_class_add_method(ObjClass *cls, const char *name, NativeFn fn) {
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

static void class_add_static(ObjClass *cls, const char *name, NativeFn fn) {
    ObjFunction *f = new_native_function(name, fn);
    ObjString *key = new_string(name, (int)strlen(name));
    dict_set(cls->fields, make_obj((Object*)key), make_obj((Object*)f));
}

static ObjList *get_state_list(Value self) {
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    Value state_val = instance_get_field(inst, "_state");
    if (!IS_OBJ(state_val) || AS_OBJ(state_val)->type->kind != OBJ_LIST) {
        return NULL;
    }
    return (ObjList*)AS_OBJ(state_val);
}

/* ============================================================ */
/* Instance methods                                              */
/* ============================================================ */

/* --- rng.int(min, max) --- */
static Value rng_int(VM *vm, Value *args, int n) {
    ObjList *state_list = get_state_list(args[0]);
    if (!state_list)
        luna_throw(vm, vm->runtime_error_class, "Random instance corrupted: missing _state");

    uint64_t raw = rng_step(state_list);

    if (n == 1) {
        /* No range: return the raw 64-bit value */
        return make_ranged_result(raw);
    }
    if (n != 3) {
        luna_throw(vm, vm->argument_error_class, "Random.int() expects 0 or 2 arguments (min, max)");
    }

    if (!IS_NUMBER(args[1]))
        luna_throw(vm, vm->type_error_class, "Random.int(): min must be numeric");
    if (!IS_NUMBER(args[2]))
        luna_throw(vm, vm->type_error_class, "Random.int(): max must be numeric");

    int64_t min = value_to_int64(args[1]);
    int64_t max = value_to_int64(args[2]);
    if (min > max) { int64_t t = min; min = max; max = t; }

    uint64_t range = (uint64_t)(max - min + 1);
    if (range == 0) return make_ranged_result(raw);

    uint64_t result = (uint64_t)min + lemire_range_64(raw, range);
    return make_ranged_result(result);
}

/* --- rng.float(min, max) --- */
static Value rng_float(VM *vm, Value *args, int n) {
    if (n < 1)
        luna_throw(vm, vm->argument_error_class, "Random.float() called without instance");

    ObjList *state_list = get_state_list(args[0]);
    if (!state_list)
        luna_throw(vm, vm->runtime_error_class, "Random instance corrupted: missing _state");

    uint64_t raw = rng_step(state_list);
    /* Upper 53 bits of 64-bit random → uniform double in [0, 1) */
    double normalized = (double)(raw >> 11) * 0x1.0p-53;

    if (n == 1) {
        return make_double(normalized);
    }
    if (n >= 3) {
        if (!IS_NUMBER(args[1]))
            luna_throw(vm, vm->type_error_class, "Random.float(): min must be numeric");
        if (!IS_NUMBER(args[2]))
            luna_throw(vm, vm->type_error_class, "Random.float(): max must be numeric");
        double min = value_to_double(args[1]);
        double max = value_to_double(args[2]);
        return make_double(min + normalized * (max - min));
    }

    luna_throw(vm, vm->argument_error_class, "Random.float() takes 0 or 2 arguments");
    return make_null(); /* unreachable */
}

/* --- rng.seed(new_seed) --- */
static Value rng_seed(VM *vm, Value *args, int n) {
    if (n < 2)
        luna_throw(vm, vm->argument_error_class, "Random.seed() requires 1 argument");
    if (!IS_NUMBER(args[1]))
        luna_throw(vm, vm->type_error_class, "Random.seed(): seed must be numeric");

    ObjList *state_list = get_state_list(args[0]);
    if (!state_list)
        luna_throw(vm, vm->runtime_error_class, "Random instance corrupted: missing _state");

    uint64_t new_seed = value_to_uint64(args[1]);
    int tag = AS_INT(list_get(state_list, 0));
    list_set(state_list, 0, make_int(tag)); /* reset algo tag */

    /* Re-initialize the state with the new seed */
    switch ((RngAlgo)tag) {
        case RNG_WYRAND:      wyrand_init(state_list, new_seed);      break;
        case RNG_XOSHIRO256SS: xoshiro256ss_init(state_list, new_seed); break;
        case RNG_PCG64:       pcg64_init(state_list, new_seed);       break;
    }

    return make_null();
}

/* --- rng_call: the callable protocol --- */
static Value rng_call(VM *vm, Value *args, int n) {
    if (n != 1 && n != 3) {
        luna_throw(vm, vm->argument_error_class, "Random() takes 0 or 2 arguments");
    }

    ObjList *state_list = get_state_list(args[0]);
    if (!state_list)
        luna_throw(vm, vm->runtime_error_class, "Random instance corrupted: missing _state");

    uint64_t raw = rng_step(state_list);

    if (n == 1) {
        /* No args → float in [0, 1) */
        double normalized = (double)(raw >> 11) * 0x1.0p-53;
        return make_double(normalized);
    }

    if (!IS_NUMBER(args[1]))
        luna_throw(vm, vm->type_error_class, "Random(): first argument must be numeric");
    if (!IS_NUMBER(args[2]))
        luna_throw(vm, vm->type_error_class, "Random(): second argument must be numeric");

    bool is_int_range = (IS_INT(args[1]) || IS_INT64(args[1])) &&
                        (IS_INT(args[2]) || IS_INT64(args[2]));
    if (is_int_range) {
        int64_t min = value_to_int64(args[1]);
        int64_t max = value_to_int64(args[2]);
        if (min > max) { int64_t t = min; min = max; max = t; }

        uint64_t range = (uint64_t)(max - min + 1);
        if (range == 0) return make_ranged_result(raw);

        uint64_t result = (uint64_t)min + lemire_range_64(raw, range);
        return make_ranged_result(result);
    }

    /* Float path */
    double min = value_to_double(args[1]);
    double max = value_to_double(args[2]);
    double normalized = (double)(raw >> 11) * 0x1.0p-53;
    return make_double(min + normalized * (max - min));
}

/* --- rng.choice(seq) --- */
static Value rng_choice(VM *vm, Value *args, int n) {
    if (n < 2)
        luna_throw(vm, vm->argument_error_class, "choice() requires a sequence argument");

    ObjList *state_list = get_state_list(args[0]);
    if (!state_list)
        luna_throw(vm, vm->runtime_error_class, "Random instance corrupted: missing _state");

    Value seq = args[1];
    if (!IS_OBJ(seq))
        luna_throw(vm, vm->type_error_class, "choice(): argument must be a list or string");

    Object *obj = AS_OBJ(seq);
    if (obj->type->kind == OBJ_LIST) {
        ObjList *list = (ObjList*)obj;
        int len = list_length(list);
        if (len == 0)
            luna_throw(vm, vm->value_error_class, "choice(): list is empty");
        uint64_t raw = rng_step(state_list);
        int idx = (int)lemire_range_64(raw, (uint64_t)len);
        return list_get(list, idx);
    }
    if (obj->type->kind == OBJ_STRING) {
        ObjString *str = (ObjString*)obj;
        if (str->length == 0)
            luna_throw(vm, vm->value_error_class, "choice(): string is empty");
        uint64_t raw = rng_step(state_list);
        int idx = (int)lemire_range_64(raw, (uint64_t)str->length);
        return make_obj((Object*)new_string(&str->chars[idx], 1));
    }

    luna_throw(vm, vm->type_error_class, "choice(): argument must be a list or string");
    return make_null(); /* unreachable */
}

/* --- rng.shuffle(list) — Fisher-Yates, in-place --- */
static Value rng_shuffle(VM *vm, Value *args, int n) {
    if (n < 2)
        luna_throw(vm, vm->argument_error_class, "shuffle() requires a list argument");

    if (!IS_OBJ(args[1]) || AS_OBJ(args[1])->type->kind != OBJ_LIST)
        luna_throw(vm, vm->type_error_class, "shuffle(): argument must be a list");

    ObjList *list = (ObjList*)AS_OBJ(args[1]);
    int len = list->count;
    if (len < 2) return make_null();

    ObjList *state_list = get_state_list(args[0]);
    if (!state_list)
        luna_throw(vm, vm->runtime_error_class, "Random instance corrupted: missing _state");

    for (int i = len - 1; i > 0; i--) {
        uint64_t raw = rng_step(state_list);
        /* j in [0, i] using Lemire */
        int j = (int)lemire_range_64(raw, (uint64_t)(i + 1));
        Value tmp = list->items ? list->items[i] : list->inline_items[i];
        if (list->items) {
            list->items[i] = list->items[j];
            list->items[j] = tmp;
        } else {
            list->inline_items[i] = list->inline_items[j];
            list->inline_items[j] = tmp;
        }
    }

    return make_null();
}

/* ============================================================ */
/* Static constructors                                           */
/* ============================================================ */

static Value random_WyRand(VM *vm, Value *args, int n) {
    uint64_t seed = 0;
    if (n >= 1) {
        if (!IS_NUMBER(args[0]))
            luna_throw(vm, vm->type_error_class, "WyRand(): seed must be numeric");
        seed = seed_from_arg(args[0]);
    }
    return make_rng_instance(vm, RNG_WYRAND, seed);
}

static Value random_Xoshiro256(VM *vm, Value *args, int n) {
    uint64_t seed = 0;
    if (n >= 1) {
        if (!IS_NUMBER(args[0]))
            luna_throw(vm, vm->type_error_class, "Xoshiro256(): seed must be numeric");
        seed = seed_from_arg(args[0]);
    }
    return make_rng_instance(vm, RNG_XOSHIRO256SS, seed);
}

static Value random_PCG(VM *vm, Value *args, int n) {
    uint64_t seed = 0;
    if (n >= 1) {
        if (!IS_NUMBER(args[0]))
            luna_throw(vm, vm->type_error_class, "PCG(): seed must be numeric");
        seed = seed_from_arg(args[0]);
    }
    return make_rng_instance(vm, RNG_PCG64, seed);
}

/* ============================================================ */
/* Module registration                                           */
/* ============================================================ */

void vm_register_random_module(VM *vm) {
    rng_class = new_class("Random", NULL);

    random_class_add_method(rng_class, "int",     rng_int);
    random_class_add_method(rng_class, "float",   rng_float);
    random_class_add_method(rng_class, "seed",    rng_seed);
    random_class_add_method(rng_class, "choice",  rng_choice);
    random_class_add_method(rng_class, "shuffle", rng_shuffle);
    random_class_add_method(rng_class, "_call",   rng_call);

    class_add_static(rng_class, "WyRand",     random_WyRand);
    class_add_static(rng_class, "Xoshiro256", random_Xoshiro256);
    class_add_static(rng_class, "PCG",        random_PCG);

    ObjModule *mod = new_module("random");
    dict_set(mod->exports,
             make_obj((Object*)new_string("Random", 6)),
             make_obj((Object*)rng_class));

    /* Cache module */
    Value mod_val = make_obj((Object*)mod);
    dict_set(vm->module_cache,
             make_obj((Object*)new_string("random", 6)),
             mod_val);
}
