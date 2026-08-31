/* api.h — Embeddable C API for the Luna VM (language-agnostic).
 *
 * Stack-based API for pushing values, reading globals, and calling
 * functions.  The embedder chooses a language by installing a FrontendDef
 * (see vm.h) before opening an API state on the VM.
 *
 * Stack index: 0-based, absolute from bottom.
 *   0 = first pushed value, top = last pushed value.
 */

#ifndef LUNA_API_H
#define LUNA_API_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "vm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LUNA_TNIL         = 0,
    LUNA_TBOOLEAN     = 1,
    LUNA_TNUMBER      = 2,
    LUNA_TINTEGER     = 3,
    LUNA_TSTRING      = 4,
    LUNA_TFUNCTION    = 5,
    LUNA_TTABLE       = 6,
    LUNA_TUSERDATA    = 7,
    LUNA_TVECTOR      = 8,
    LUNA_TMATRIX      = 9,
    LUNA_TBUFFER      = 10,
    LUNA_TCLASS       = 11,
    LUNA_TINSTANCE    = 12,
    LUNA_TCFUNCTION   = 13,
} api_Type;

typedef enum {
    LUNA_OK        = 0,
    LUNA_ERRRUN    = 1,
    LUNA_ERRSYNTAX = 2,
    LUNA_ERRMEM    = 3,
    LUNA_ERRERR    = 4,
} api_Status;

/* State management — APIState lives on a VM whose frontend is already installed. */
APIState *api_open(VM *vm);
void      api_close(APIState *L);

/* Mark the API stack as GC roots (called from a frontend's mark_roots hook). */
void      api_mark_roots(VM *vm);

/* Stack manipulation */
int  api_get_top(APIState *L);
void api_set_top(APIState *L, int n);
void api_push_value(APIState *L, int idx);
void api_remove(APIState *L, int idx);
void api_insert(APIState *L, int idx);
void api_replace(APIState *L, int idx);
int  api_check_stack(APIState *L, int n);

#define api_pop(L, n) api_set_top((L), -(n) - 1)

/* Type checking and access */
int         api_type(APIState *L, int idx);
bool        api_is_nil(APIState *L, int idx);
bool        api_is_boolean(APIState *L, int idx);
bool        api_is_number(APIState *L, int idx);
bool        api_is_integer(APIState *L, int idx);
bool        api_is_string(APIState *L, int idx);
bool        api_is_function(APIState *L, int idx);
bool        api_is_cfunction(APIState *L, int idx);
bool        api_is_userdata(APIState *L, int idx);

bool        api_to_boolean(APIState *L, int idx);
double      api_to_number(APIState *L, int idx);
int64_t     api_to_integer(APIState *L, int idx);
const char *api_to_string(APIState *L, int idx, size_t *len);

/* Push values onto the stack */
void api_push_nil(APIState *L);
void api_push_boolean(APIState *L, bool b);
void api_push_number(APIState *L, double n);
void api_push_integer(APIState *L, int64_t n);
void api_push_string(APIState *L, const char *s);
void api_push_lstring(APIState *L, const char *s, size_t len);
void api_push_cfunction(APIState *L, api_CFunction fn);

/* Table (dict) operations */
void api_new_dict(APIState *L);
void api_set_field(APIState *L, int idx, const char *key);
int  api_get_field(APIState *L, int idx, const char *key);

/* List (array) operations */
void api_new_list(APIState *L);
void api_list_append(APIState *L, int idx);
void api_get_index(APIState *L, int idx, int n);
void api_set_index(APIState *L, int idx, int n);

/* Userdata */
void        api_new_userdata(APIState *L, void *data, const char *tag, void (*finalizer)(void *));
void       *api_to_userdata(APIState *L, int idx);
bool        api_is_userdata_tag(APIState *L, int idx, const char *tag);
const char *api_get_userdata_tag(APIState *L, int idx);
void        api_push_lightuserdata(APIState *L, void *ptr);
void       *api_to_lightuserdata(APIState *L, int idx);

/* Global access */
int  api_get_global(APIState *L, const char *name);
void api_set_global(APIState *L, const char *name);

/* System globals — persist across modules, visible without import */
void api_set_system_global(APIState *L, const char *name);
int  api_get_system_global(APIState *L, const char *name);

/* Call a function (args already on stack, function at bottom) */
api_Status api_pcall(APIState *L, int nargs, int nresults);

/* Native C function dispatcher (called by the frontend object model) */
Value api_cfunc_dispatch(VM *vm, api_CFunction fn, Value *args, int arg_count);

/* Load and run source (uses the installed frontend's compiler) */
api_Status api_load_string(APIState *L, const char *str);
api_Status api_load_file(APIState *L, const char *filename);
api_Status api_dostring(APIState *L, const char *str);
api_Status api_dofile(APIState *L, const char *filename);

/* Garbage collection */
int  api_gc(APIState *L, int what);

/* Error handling */
void api_error(APIState *L, const char *fmt, ...);

/* Argument checking */
double      api_checknumber(APIState *L, int arg);
int64_t     api_checkinteger(APIState *L, int arg);
const char* api_checkstring(APIState *L, int arg);
void*       api_checkuserdata(APIState *L, int arg, const char *tag);

#ifdef __cplusplus
}
#endif

#endif /* LUNA_API_H */