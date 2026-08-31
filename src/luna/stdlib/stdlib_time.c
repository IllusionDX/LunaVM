/* stdlib_time.c — Built-in time module for Luna.
 *
 * time.now()        -> Unix time in seconds as a float
 * time.ticks_usec() -> Microseconds elapsed since the VM started
 * time.ticks_msec() -> Milliseconds elapsed since the VM started
 * time.sleep(ms)    -> Yield execution to the OS for the requested milliseconds
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "stdlib_time.h"
#include "value.h"
#include "luna/object.h"
#include "luna/frontend_state.h"

#ifdef _WIN32
#include <windows.h>

static double unix_time_seconds(void) {
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return ((double)uli.QuadPart / 10000000.0) - 11644473600.0;
}

uint64_t luna_time_monotonic_us(void) {
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER counter;

    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&counter);
    return (uint64_t)(((long double)counter.QuadPart * 1000000.0L) / (long double)freq.QuadPart);
}

static void sleep_ms(double ms) {
    if (ms <= 0.0) return;
    DWORD whole_ms = (DWORD)ms;
    if (whole_ms == 0) whole_ms = 1;
    Sleep(whole_ms);
}

#else
#include <time.h>

static double unix_time_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

uint64_t luna_time_monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
}

static void sleep_ms(double ms) {
    if (ms <= 0.0) return;
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000.0);
    double rem_ms = ms - ((double)req.tv_sec * 1000.0);
    req.tv_nsec = (long)(rem_ms * 1000000.0);
    if (req.tv_nsec < 0) req.tv_nsec = 0;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        /* retry with remaining time */
    }
}

#endif

static inline double checked_number(VM *vm, Value v, const char *fn) {
    if (!IS_NUMBER(v)) {
        luna_throw(vm, luna_fe(vm)->type_error_class, "%s() argument must be numeric", fn);
    }
    return value_to_double(v);
}

static Value time_now(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, luna_fe(vm)->argument_error_class, "time.now() takes no arguments");
    }
    return make_double(unix_time_seconds());
}

static uint64_t time_elapsed_us(VM *vm) {
    uint64_t now_us = luna_time_monotonic_us();
    return (now_us >= vm->time_start_us) ? (now_us - vm->time_start_us) : 0;
}

static Value time_ticks_usec(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, luna_fe(vm)->argument_error_class, "time.ticks_usec() takes no arguments");
    }
    return make_double((double)time_elapsed_us(vm));
}

static Value time_ticks_msec(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, luna_fe(vm)->argument_error_class, "time.ticks_msec() takes no arguments");
    }
    return make_double((double)time_elapsed_us(vm) / 1000.0);
}

static Value time_sleep(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, luna_fe(vm)->argument_error_class, "time.sleep() expects exactly 1 argument");
    }
    double ms = checked_number(vm, args[0], "time.sleep");
    sleep_ms(ms);
    return make_null();
}

static void module_add_native(ObjModule *mod, const char *name, NativeFn fn) {
    ObjFunction *f = new_native_function(name, fn);
    ObjString *key = new_string(name, (int)strlen(name));
    dict_set(mod->exports, make_obj((Object *)key), make_obj((Object *)f));
}

void vm_register_time_module(VM *vm) {
    ObjModule *mod = new_module("time");
    module_add_native(mod, "now", time_now);
    module_add_native(mod, "ticks_usec", time_ticks_usec);
    module_add_native(mod, "ticks_msec", time_ticks_msec);
    module_add_native(mod, "sleep", time_sleep);

    Value mod_val = make_obj((Object *)mod);
    ObjString *key = new_string("time", 4);
    dict_set(luna_fe(vm)->module_cache, make_obj((Object *)key), mod_val);
}
