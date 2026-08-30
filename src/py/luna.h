/* luna.h — Embeddable C API for the Luna interpreter.
 *
 * Provides a stack-based API for calling Luna functions,
 * accessing globals, and binding native C functions.
 *
 * Stack index: 0-based, absolute from bottom.
 *   0 = first pushed value, top = last pushed value.
 */

#ifndef LUNA_LUNA_H
#define LUNA_LUNA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct py_State py_State;
typedef int (*py_CFunction)(py_State *L);

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
} py_Type;

typedef enum {
    LUNA_OK        = 0,
    LUNA_ERRRUN    = 1,
    LUNA_ERRSYNTAX = 2,
    LUNA_ERRMEM    = 3,
    LUNA_ERRERR    = 4,
} py_Status;

/* State management */
py_State *py_new_state(void);
void        py_close(py_State *L);

/* Stack manipulation */
int  py_get_top(py_State *L);
void py_set_top(py_State *L, int n);
void py_push_value(py_State *L, int idx);
void py_remove(py_State *L, int idx);
void py_insert(py_State *L, int idx);
void py_replace(py_State *L, int idx);
int  py_check_stack(py_State *L, int n);

#define py_pop(L, n) py_set_top((L), -(n) - 1)

/* Type checking and access */
int         py_type(py_State *L, int idx);
bool        py_is_nil(py_State *L, int idx);
bool        py_is_boolean(py_State *L, int idx);
bool        py_is_number(py_State *L, int idx);
bool        py_is_integer(py_State *L, int idx);
bool        py_is_string(py_State *L, int idx);
bool        py_is_function(py_State *L, int idx);
bool        py_is_cfunction(py_State *L, int idx);
bool        py_is_userdata(py_State *L, int idx);

bool        py_to_boolean(py_State *L, int idx);
double      py_to_number(py_State *L, int idx);
int64_t     py_to_integer(py_State *L, int idx);
const char *py_to_string(py_State *L, int idx, size_t *len);

/* Push values onto the stack */
void py_push_nil(py_State *L);
void py_push_boolean(py_State *L, bool b);
void py_push_number(py_State *L, double n);
void py_push_integer(py_State *L, int64_t n);
void py_push_string(py_State *L, const char *s);
void py_push_lstring(py_State *L, const char *s, size_t len);
void py_push_cfunction(py_State *L, py_CFunction fn);

/* Table (dict) operations */
void py_new_dict(py_State *L);
void py_set_field(py_State *L, int idx, const char *key);
int  py_get_field(py_State *L, int idx, const char *key);

/* List (array) operations */
void py_new_list(py_State *L);
void py_list_append(py_State *L, int idx);
void py_get_index(py_State *L, int idx, int n);
void py_set_index(py_State *L, int idx, int n);

/* Userdata */
void    py_new_userdata(py_State *L, void *data, const char *tag, void (*finalizer)(void *));
void   *py_to_userdata(py_State *L, int idx);
bool    py_is_userdata_tag(py_State *L, int idx, const char *tag);
const char *py_get_userdata_tag(py_State *L, int idx);
void    py_push_lightuserdata(py_State *L, void *ptr);
void   *py_to_lightuserdata(py_State *L, int idx);

/* Global access */
int  py_get_global(py_State *L, const char *name);
void py_set_global(py_State *L, const char *name);

/* System globals — persist across modules, visible from all scripts without import */
void py_set_system_global(py_State *L, const char *name);
int  py_get_system_global(py_State *L, const char *name);

/* Call a function (args already on stack, function at bottom) */
py_Status py_pcall(py_State *L, int nargs, int nresults);

/* Load and execute LunaScript */
py_Status lunaL_load_string(py_State *L, const char *str);
py_Status lunaL_load_file(py_State *L, const char *filename);
py_Status lunaL_dostring(py_State *L, const char *str);
py_Status lunaL_dofile(py_State *L, const char *filename);

/* Garbage collection */
int py_gc(py_State *L, int what);

/* Error handling */
void py_error(py_State *L, const char *fmt, ...);

/* Argument checking */
double      lunaL_checknumber(py_State *L, int arg);
int64_t     lunaL_checkinteger(py_State *L, int arg);
const char* lunaL_checkstring(py_State *L, int arg);
void*       lunaL_checkuserdata(py_State *L, int arg, const char *tag);

#ifdef __cplusplus
}
#endif

#endif /* LUNA_LUNA_H */
