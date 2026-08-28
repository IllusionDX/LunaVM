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

typedef struct luna_State luna_State;
typedef int (*luna_CFunction)(luna_State *L);

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
} luna_Type;

typedef enum {
    LUNA_OK        = 0,
    LUNA_ERRRUN    = 1,
    LUNA_ERRSYNTAX = 2,
    LUNA_ERRMEM    = 3,
    LUNA_ERRERR    = 4,
} luna_Status;

/* State management */
luna_State *luna_new_state(void);
void        luna_close(luna_State *L);

/* Stack manipulation */
int  luna_get_top(luna_State *L);
void luna_set_top(luna_State *L, int n);
void luna_push_value(luna_State *L, int idx);
void luna_remove(luna_State *L, int idx);
void luna_insert(luna_State *L, int idx);
void luna_replace(luna_State *L, int idx);
int  luna_check_stack(luna_State *L, int n);

#define luna_pop(L, n) luna_set_top((L), -(n) - 1)

/* Type checking and access */
int         luna_type(luna_State *L, int idx);
bool        luna_is_nil(luna_State *L, int idx);
bool        luna_is_boolean(luna_State *L, int idx);
bool        luna_is_number(luna_State *L, int idx);
bool        luna_is_integer(luna_State *L, int idx);
bool        luna_is_string(luna_State *L, int idx);
bool        luna_is_function(luna_State *L, int idx);
bool        luna_is_cfunction(luna_State *L, int idx);
bool        luna_is_userdata(luna_State *L, int idx);

bool        luna_to_boolean(luna_State *L, int idx);
double      luna_to_number(luna_State *L, int idx);
int64_t     luna_to_integer(luna_State *L, int idx);
const char *luna_to_string(luna_State *L, int idx, size_t *len);

/* Push values onto the stack */
void luna_push_nil(luna_State *L);
void luna_push_boolean(luna_State *L, bool b);
void luna_push_number(luna_State *L, double n);
void luna_push_integer(luna_State *L, int64_t n);
void luna_push_string(luna_State *L, const char *s);
void luna_push_lstring(luna_State *L, const char *s, size_t len);
void luna_push_cfunction(luna_State *L, luna_CFunction fn);

/* Table (dict) operations */
void luna_new_dict(luna_State *L);
void luna_set_field(luna_State *L, int idx, const char *key);
int  luna_get_field(luna_State *L, int idx, const char *key);

/* List (array) operations */
void luna_new_list(luna_State *L);
void luna_list_append(luna_State *L, int idx);
void luna_get_index(luna_State *L, int idx, int n);
void luna_set_index(luna_State *L, int idx, int n);

/* Userdata */
void    luna_new_userdata(luna_State *L, void *data, const char *tag, void (*finalizer)(void *));
void   *luna_to_userdata(luna_State *L, int idx);
bool    luna_is_userdata_tag(luna_State *L, int idx, const char *tag);
const char *luna_get_userdata_tag(luna_State *L, int idx);
void    luna_push_lightuserdata(luna_State *L, void *ptr);
void   *luna_to_lightuserdata(luna_State *L, int idx);

/* Global access */
int  luna_get_global(luna_State *L, const char *name);
void luna_set_global(luna_State *L, const char *name);

/* System globals — persist across modules, visible from all scripts without import */
void luna_set_system_global(luna_State *L, const char *name);
int  luna_get_system_global(luna_State *L, const char *name);

/* Call a function (args already on stack, function at bottom) */
luna_Status luna_pcall(luna_State *L, int nargs, int nresults);

/* Load and execute LunaScript */
luna_Status lunaL_load_string(luna_State *L, const char *str);
luna_Status lunaL_load_file(luna_State *L, const char *filename);
luna_Status lunaL_dostring(luna_State *L, const char *str);
luna_Status lunaL_dofile(luna_State *L, const char *filename);

/* Garbage collection */
int luna_gc(luna_State *L, int what);

/* Error handling */
void luna_error(luna_State *L, const char *fmt, ...);

/* Argument checking */
double      lunaL_checknumber(luna_State *L, int arg);
int64_t     lunaL_checkinteger(luna_State *L, int arg);
const char* lunaL_checkstring(luna_State *L, int arg);
void*       lunaL_checkuserdata(luna_State *L, int arg, const char *tag);

#ifdef __cplusplus
}
#endif

#endif /* LUNA_LUNA_H */
