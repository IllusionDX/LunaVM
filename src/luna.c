/* luna.c — Embeddable C API implementation for the Luna interpreter. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "luna.h"
#include "vm.h"
#include "value.h"
#include "chunk.h"
#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include "ast.h"

/* ============================================================ */
/* Internal state                                                */
/* ============================================================ */

#define LUNA_API_STACK_INIT 64

struct luna_State {
    VM    *vm;
    Value *stack;
    int    cap;
    int    top;
};

/* ============================================================ */
/* GC hook — called by vm.c's mark_and_sweep                    */
/* ============================================================ */

void luna_mark_roots(VM *vm) {
    luna_State *L = (luna_State *)vm->api_state;
    if (!L) return;
    for (int i = 0; i < L->top; i++) {
        vm_mark_value(vm, L->stack[i]);
    }
}

/* ============================================================ */
/* State management                                              */
/* ============================================================ */

luna_State *luna_new_state(void) {
    luna_State *L = calloc(1, sizeof(luna_State));
    if (!L) return NULL;

    L->vm = calloc(1, sizeof(VM));
    if (!L->vm) { free(L); return NULL; }

    vm_init(L->vm);
    L->vm->api_state = L;

    L->cap = LUNA_API_STACK_INIT;
    L->stack = calloc((size_t)L->cap, sizeof(Value));
    if (!L->stack) {
        vm_free(L->vm);
        free(L->vm);
        free(L);
        return NULL;
    }
    L->top = 0;

    return L;
}

void luna_close(luna_State *L) {
    if (!L) return;
    VM *vm = L->vm;
    vm->api_state = NULL;
    free(L->stack);
    vm_free(vm);
    free(vm);
    free(L);
}

/* ============================================================ */
/* Stack manipulation helpers                                    */
/* ============================================================ */

static Value *luna_stack_ptr(luna_State *L, int idx) {
    if (idx < 0) idx = L->top + idx;
    if (idx < 0 || idx >= L->top) return NULL;
    return &L->stack[idx];
}

static bool luna_grow_stack(luna_State *L, int min) {
    if (min <= L->cap) return true;
    int new_cap = L->cap * 2;
    while (new_cap < min) new_cap *= 2;
    Value *new_stack = realloc(L->stack, (size_t)new_cap * sizeof(Value));
    if (!new_stack) return false;
    /* Zero new slots */
    for (int i = L->cap; i < new_cap; i++) new_stack[i] = make_null();
    L->stack = new_stack;
    L->cap = new_cap;
    return true;
}

/* ============================================================ */
/* Stack manipulation API                                        */
/* ============================================================ */

int luna_get_top(luna_State *L) {
    return L->top;
}

void luna_set_top(luna_State *L, int n) {
    if (n < 0) n = L->top + n + 1;
    if (n < 0) n = 0;
    if (n > L->top) {
        if (!luna_grow_stack(L, n)) return;
        for (int i = L->top; i < n; i++) L->stack[i] = make_null();
    }
    L->top = n;
}

void luna_push_value(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v) return;
    if (!luna_grow_stack(L, L->top + 1)) return;
    L->stack[L->top++] = *v;
}

void luna_remove(luna_State *L, int idx) {
    if (idx < 0 || idx >= L->top) return;
    for (int i = idx; i < L->top - 1; i++)
        L->stack[i] = L->stack[i + 1];
    L->top--;
}

void luna_insert(luna_State *L, int idx) {
    if (idx < 0 || idx > L->top) return;
    if (!luna_grow_stack(L, L->top + 1)) return;
    for (int i = L->top; i > idx; i--)
        L->stack[i] = L->stack[i - 1];
    L->stack[idx] = make_null();
    L->top++;
}

void luna_replace(luna_State *L, int idx) {
    if (idx < 0 || idx >= L->top) return;
    L->stack[idx] = L->stack[L->top - 1];
    L->top--;
}

int luna_check_stack(luna_State *L, int n) {
    return luna_grow_stack(L, L->top + n) ? 1 : 0;
}

/* ============================================================ */
/* Type checking                                                 */
/* ============================================================ */

int luna_type(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v) return LUNA_TNIL;
    Value val = *v;

    if (IS_NIL(val))     return LUNA_TNIL;
    if (IS_BOOL(val))    return LUNA_TBOOLEAN;
    if (IS_INT(val))     return LUNA_TINTEGER;
    if (IS_INT64(val))   return LUNA_TINTEGER;
    if (IS_DOUBLE(val))  return LUNA_TNUMBER;
    if (IS_STRING(val))  return LUNA_TSTRING;

    if (IS_OBJ(val)) {
        Object *obj = AS_OBJ(val);
        switch (obj->type) {
            case OBJ_STRING:     return LUNA_TSTRING;
            case OBJ_FUNCTION:   return LUNA_TFUNCTION;
            case OBJ_CLOSURE:    return LUNA_TFUNCTION;
            case OBJ_CLASS:      return LUNA_TCLASS;
            case OBJ_INSTANCE:   return LUNA_TINSTANCE;
            case OBJ_DICT:       return LUNA_TTABLE;
            case OBJ_LIST:       return LUNA_TTABLE;
            case OBJ_USERDATA:   return LUNA_TUSERDATA;
            case OBJ_VECTOR:     return LUNA_TVECTOR;
            case OBJ_MATRIX:     return LUNA_TMATRIX;
            case OBJ_BUFFER:     return LUNA_TBUFFER;
            case OBJ_INT64:      return LUNA_TINTEGER;
            default:             return LUNA_TNIL;
        }
    }
    return LUNA_TNIL;
}

bool luna_is_nil(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    return v && IS_NIL(*v);
}

bool luna_is_boolean(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    return v && IS_BOOL(*v);
}

bool luna_is_number(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    return v && (IS_DOUBLE(*v) || IS_INT(*v) || IS_INT64(*v));
}

bool luna_is_integer(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    return v && (IS_INT(*v) || IS_INT64(*v));
}

bool luna_is_string(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    return v && IS_STRING(*v);
}

bool luna_is_function(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v) return false;
    if (!IS_OBJ(*v)) return false;
    Object *obj = AS_OBJ(*v);
    return obj->type == OBJ_FUNCTION || obj->type == OBJ_CLOSURE;
}

bool luna_is_cfunction(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v || !IS_OBJ(*v)) return false;
    Object *obj = AS_OBJ(*v);
    if (obj->type != OBJ_FUNCTION) return false;
    return ((ObjFunction *)obj)->cfunc != NULL;
}

bool luna_is_userdata(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    return v && IS_USERDATA(*v);
}

/* ============================================================ */
/* Access functions                                              */
/* ============================================================ */

bool luna_to_boolean(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v) return false;
    if (IS_BOOL(*v)) return AS_BOOL(*v);
    return is_truthy(*v);
}

double luna_to_number(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v) return 0.0;
    if (IS_DOUBLE(*v))  return AS_DOUBLE(*v);
    if (IS_INT(*v))     return (double)AS_INT(*v);
    if (IS_INT64(*v))   return (double)((ObjInt64 *)AS_OBJ(*v))->value;
    return 0.0;
}

int64_t luna_to_integer(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v) return 0;
    if (IS_INT(*v))     return AS_INT(*v);
    if (IS_INT64(*v))   return ((ObjInt64 *)AS_OBJ(*v))->value;
    if (IS_DOUBLE(*v))  return (int64_t)AS_DOUBLE(*v);
    return 0;
}

const char *luna_to_string(luna_State *L, int idx, size_t *len) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v || !IS_STRING(*v)) {
        if (len) *len = 0;
        return NULL;
    }
    ObjString *s = (ObjString *)AS_OBJ(*v);
    if (len) *len = (size_t)s->length;
    return s->chars;
}

/* ============================================================ */
/* Push functions                                                */
/* ============================================================ */

void luna_push_nil(luna_State *L) {
    if (luna_grow_stack(L, L->top + 1))
        L->stack[L->top++] = make_null();
}

void luna_push_boolean(luna_State *L, bool b) {
    if (luna_grow_stack(L, L->top + 1))
        L->stack[L->top++] = make_bool(b);
}

void luna_push_number(luna_State *L, double n) {
    if (luna_grow_stack(L, L->top + 1))
        L->stack[L->top++] = make_double(n);
}

void luna_push_integer(luna_State *L, int64_t n) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    if (n >= INT32_MIN && n <= INT32_MAX)
        L->stack[L->top++] = make_int((int32_t)n);
    else
        L->stack[L->top++] = make_int64(n);
}

void luna_push_string(luna_State *L, const char *s) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    L->stack[L->top++] = make_obj((Object *)new_string(s, (int)strlen(s)));
}

void luna_push_lstring(luna_State *L, const char *s, size_t len) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    L->stack[L->top++] = make_obj((Object *)new_string(s, (int)len));
}

void luna_push_cfunction(luna_State *L, luna_CFunction fn) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    ObjFunction *f = new_native_function("<cfunc>", NULL);
    f->cfunc = (int (*)(struct luna_State *L))fn;
    L->stack[L->top++] = make_obj((Object *)f);
}

/* ============================================================ */
/* Table (dict) operations                                       */
/* ============================================================ */

void luna_new_dict(luna_State *L) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    L->stack[L->top++] = make_obj((Object *)new_dict());
}

void luna_set_field(luna_State *L, int idx, const char *key) {
    Value *dv = luna_stack_ptr(L, idx);
    if (!dv || L->top < 1) return;
    if (!IS_OBJ(*dv) || ((Object *)AS_OBJ(*dv))->type != OBJ_DICT) return;
    ObjDict *dict = (ObjDict *)AS_OBJ(*dv);
    Value val = L->stack[L->top - 1];
    dict_set(dict, make_obj((Object *)new_string(key, (int)strlen(key))), val);
    L->top--;
}

int luna_get_field(luna_State *L, int idx, const char *key) {
    Value *dv = luna_stack_ptr(L, idx);
    if (!dv || !IS_OBJ(*dv)) return LUNA_TNIL;
    Object *obj = AS_OBJ(*dv);
    if (obj->type == OBJ_DICT) {
        ObjDict *dict = (ObjDict *)obj;
        Value val = dict_get(dict, make_obj((Object *)new_string(key, (int)strlen(key))));
        if (!luna_grow_stack(L, L->top + 1)) return LUNA_TNIL;
        L->stack[L->top++] = val;
        return luna_type(L, L->top - 1);
    }
    if (obj->type == OBJ_INSTANCE) {
        Value val = instance_get_field((ObjInstance *)obj, key);
        if (!luna_grow_stack(L, L->top + 1)) return LUNA_TNIL;
        L->stack[L->top++] = val;
        return luna_type(L, L->top - 1);
    }
    return LUNA_TNIL;
}

/* ============================================================ */
/* List (array) operations                                       */
/* ============================================================ */

void luna_new_list(luna_State *L) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    L->stack[L->top++] = make_obj((Object *)new_list(0));
}

void luna_list_append(luna_State *L, int idx) {
    Value *lv = luna_stack_ptr(L, idx);
    if (!lv || L->top < 1) return;
    if (!IS_OBJ(*lv) || ((Object *)AS_OBJ(*lv))->type != OBJ_LIST) return;
    ObjList *lst = (ObjList *)AS_OBJ(*lv);
    Value val = L->stack[L->top - 1];
    list_add(lst, val);
    L->top--;
}

void luna_get_index(luna_State *L, int idx, int n) {
    Value *lv = luna_stack_ptr(L, idx);
    if (!lv || !IS_OBJ(*lv) || ((Object *)AS_OBJ(*lv))->type != OBJ_LIST) {
        luna_push_nil(L);
        return;
    }
    ObjList *lst = (ObjList *)AS_OBJ(*lv);
    int len = list_length(lst);
    if (n < 0 || n >= len) {
        luna_push_nil(L);
    } else {
        Value val = lst->items ? lst->items[n] : lst->inline_items[n];
        if (luna_grow_stack(L, L->top + 1)) L->stack[L->top++] = val;
    }
}

void luna_set_index(luna_State *L, int idx, int n) {
    Value *lv = luna_stack_ptr(L, idx);
    if (!lv || L->top < 1 || !IS_OBJ(*lv) || ((Object *)AS_OBJ(*lv))->type != OBJ_LIST) return;
    ObjList *lst = (ObjList *)AS_OBJ(*lv);
    Value val = L->stack[L->top - 1];
    list_set(lst, n, val);
    L->top--;
}

/* ============================================================ */
/* Userdata                                                      */
/* ============================================================ */

void luna_new_userdata(luna_State *L, void *data, const char *tag, void (*finalizer)(void *)) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    ObjUserdata *ud = new_userdata_tagged(tag, data, finalizer);
    L->stack[L->top++] = make_obj((Object *)ud);
}

void *luna_to_userdata(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v || !IS_USERDATA(*v)) return NULL;
    return ((ObjUserdata *)AS_OBJ(*v))->data;
}

bool luna_is_userdata_tag(luna_State *L, int idx, const char *tag) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v || !IS_USERDATA(*v)) return false;
    ObjUserdata *ud = (ObjUserdata *)AS_OBJ(*v);
    return ud->tag && tag && strcmp(ud->tag, tag) == 0;
}

const char *luna_get_userdata_tag(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v || !IS_USERDATA(*v)) return NULL;
    return ((ObjUserdata *)AS_OBJ(*v))->tag;
}

void luna_push_lightuserdata(luna_State *L, void *ptr) {
    if (!luna_grow_stack(L, L->top + 1)) return;
    ObjUserdata *ud = new_userdata_tagged("lightuserdata", ptr, NULL);
    L->stack[L->top++] = make_obj((Object *)ud);
}

void *luna_to_lightuserdata(luna_State *L, int idx) {
    Value *v = luna_stack_ptr(L, idx);
    if (!v || !IS_USERDATA(*v)) return NULL;
    ObjUserdata *ud = (ObjUserdata *)AS_OBJ(*v);
    return ud->data;
}

/* ============================================================ */
/* Global access                                                 */
/* ============================================================ */

int luna_get_global(luna_State *L, const char *name) {
    Value val;
    if (!vm_get_global(L->vm, name, &val)) {
        luna_push_nil(L);
        return LUNA_TNIL;
    }
    if (!luna_grow_stack(L, L->top + 1)) return LUNA_TNIL;
    L->stack[L->top++] = val;
    return luna_type(L, L->top - 1);
}

void luna_set_global(luna_State *L, const char *name) {
    if (L->top < 1) return;
    Value val = L->stack[L->top - 1];
    vm_set_global(L->vm, name, val, false);
    L->top--;
}

/* ============================================================ */
/* System globals (persist across modules)                       */
/* ============================================================ */

void luna_set_system_global(luna_State *L, const char *name) {
    if (L->top < 1) return;
    Value val = L->stack[L->top - 1];
    vm_set_system_global(L->vm, name, val);
    L->top--;
}

int luna_get_system_global(luna_State *L, const char *name) {
    Value val;
    if (!vm_get_global(L->vm, name, &val)) {
        luna_push_nil(L);
        return LUNA_TNIL;
    }
    if (!luna_grow_stack(L, L->top + 1)) return LUNA_TNIL;
    L->stack[L->top++] = val;
    return luna_type(L, L->top - 1);
}

/* ============================================================ */
/* C function dispatch — called from vm_opcodes.inc              */
/* ============================================================ */

Value luna_cfunc_dispatch(VM *vm, ObjFunction *fn, Value *args, int arg_count) {
    luna_State *L = (luna_State *)vm->api_state;
    if (!L || !fn->cfunc) return make_null();

    /* Ensure API stack has room */
    luna_grow_stack(L, L->top + arg_count);

    /* Push args onto the API stack */
    for (int i = 0; i < arg_count; i++) {
        L->stack[L->top++] = args[i];
    }

    /* Call the C function */
    int nresults = fn->cfunc(L);

    /* Clamp nresults to available stack */
    if (nresults < 0) nresults = 0;
    if (nresults > L->top) nresults = L->top;

    /* If there are results, return the first one */
    Value result = make_null();
    if (nresults > 0) {
        result = L->stack[L->top - nresults];
    }

    /* Pop all args + results from API stack */
    L->top -= (arg_count + nresults);

    return result;
}

/* ============================================================ */
/* pcall                                                         */
/* ============================================================ */

luna_Status luna_pcall(luna_State *L, int nargs, int nresults) {
    VM *vm = L->vm;
    int func_idx = L->top - nargs - 1;

    if (func_idx < 0) return LUNA_ERRRUN;

    Value fn_val = L->stack[func_idx];

    /* Collect args */
    Value *args = nargs > 0 ? &L->stack[func_idx + 1] : NULL;

    /* Remove function + args from API stack */
    L->top = func_idx;

    /* Call through vm_call_value */
    Value result;
    VMResult vm_result = vm_call_value(vm, fn_val, args, nargs, &result);

    if (vm_result == VM_EXCEPTION) {
        /* Push error message */
        char *msg = value_to_string(vm->last_exception);
        if (msg) {
            luna_push_string(L, msg);
            free(msg);
        } else {
            luna_push_string(L, "unknown error");
        }
        return LUNA_ERRRUN;
    }

    /* Push results */
    if (nresults != 0) {
        luna_grow_stack(L, L->top + 1);
        L->stack[L->top++] = result;
    }

    return LUNA_OK;
}

/* ============================================================ */
/* Load and run LunaScript                                       */
/* ============================================================ */

luna_Status lunaL_load_string(luna_State *L, const char *str) {
    VM *vm = L->vm;

    /* Lex */
    Lexer *lexer = lexer_new(str);
    if (!lexer) return LUNA_ERRMEM;

    TokenList *tokens = lexer_tokenize(lexer);
    if (!tokens) {
        lexer_free(lexer);
        return LUNA_ERRMEM;
    }

    /* Check for lex errors */
    Token *t = tokens->head;
    while (t) {
        if (t->type == TOK_ERROR) {
            luna_push_string(L, t->value ? t->value : "lexer error");
            token_list_free(tokens);
            lexer_free(lexer);
            return LUNA_ERRSYNTAX;
        }
        t = t->next;
    }

    /* Parse */
    Parser *parser = parser_new(tokens, str, "<string>");
    if (!parser) {
        token_list_free(tokens);
        lexer_free(lexer);
        return LUNA_ERRMEM;
    }

    Program *program = parser_parse(parser);
    if (!program) {
        luna_push_string(L, "parse error");
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return LUNA_ERRSYNTAX;
    }

    /* Compile */
    Chunk chunk;
    if (!compile_program(program, &chunk, vm, false, false)) {
        luna_push_string(L, "compile error");
        chunk_free(&chunk);
        free_program(program);
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return LUNA_ERRSYNTAX;
    }
    chunk.source_path = NULL;

    /* Create a function from the chunk */
    ObjFunction *fn = new_function("<load>");
    fn->chunk = &chunk; /* Transfer ownership */
    fn->param_count = 0;

    /* Push as a closure */
    ObjClosure *cl = new_closure(fn);
    luna_grow_stack(L, L->top + 1);
    L->stack[L->top++] = make_obj((Object *)cl);

    /* The chunk is owned by the function now; don't free it */
    free_program(program);
    parser_free(parser);
    token_list_free(tokens);
    lexer_free(lexer);

    return LUNA_OK;
}

luna_Status lunaL_load_file(luna_State *L, const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        luna_push_string(L, "cannot open file");
        return LUNA_ERRRUN;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len < 0) {
        fclose(f);
        luna_push_string(L, "cannot read file");
        return LUNA_ERRRUN;
    }

    char *buf = malloc((size_t)(len + 1));
    if (!buf) {
        fclose(f);
        return LUNA_ERRMEM;
    }

    size_t read = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[read] = '\0';

    luna_Status status = lunaL_load_string(L, buf);
    free(buf);

    /* Replace chunk source path with the actual filename */
    if (status == LUNA_OK && L->top > 0) {
        Value *v = luna_stack_ptr(L, L->top - 1);
        if (v && IS_OBJ(*v)) {
            ObjClosure *cl = (ObjClosure *)AS_OBJ(*v);
            if (cl && cl->function && cl->function->chunk) {
                free(cl->function->chunk->source_path);
                cl->function->chunk->source_path = strdup(filename);
            }
        }
    }

    return status;
}

luna_Status lunaL_dostring(luna_State *L, const char *str) {
    luna_Status s = lunaL_load_string(L, str);
    if (s != LUNA_OK) return s;
    /* The function is on the stack; call with 0 args */
    return luna_pcall(L, 0, 1);
}

luna_Status lunaL_dofile(luna_State *L, const char *filename) {
    luna_Status s = lunaL_load_file(L, filename);
    if (s != LUNA_OK) return s;
    return luna_pcall(L, 0, 1);
}

/* ============================================================ */
/* GC                                                           */
/* ============================================================ */

int luna_gc(luna_State *L, int what) {
    (void)what;
    /* Trigger full GC */
    mark_and_sweep(L->vm);
    return 0;
}

/* ============================================================ */
/* Error handling and Arg checking                              */
/* ============================================================ */

void luna_error(luna_State *L, const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    L->vm->last_exception = make_exception_instance(L->vm, L->vm->exception_class, buf);
    if (L->vm->native_jump) {
        longjmp(L->vm->native_jump->env, 1);
    }
}

double lunaL_checknumber(luna_State *L, int arg) {
    if (!luna_is_number(L, arg)) {
        luna_error(L, "bad argument #%d (number expected)", arg + (arg < 0 ? L->top + 1 : 1));
    }
    return luna_to_number(L, arg);
}

int64_t lunaL_checkinteger(luna_State *L, int arg) {
    if (!luna_is_integer(L, arg)) {
        luna_error(L, "bad argument #%d (integer expected)", arg + (arg < 0 ? L->top + 1 : 1));
    }
    return luna_to_integer(L, arg);
}

const char* lunaL_checkstring(luna_State *L, int arg) {
    if (!luna_is_string(L, arg)) {
        luna_error(L, "bad argument #%d (string expected)", arg + (arg < 0 ? L->top + 1 : 1));
    }
    return luna_to_string(L, arg, NULL);
}

void* lunaL_checkuserdata(luna_State *L, int arg, const char *tag) {
    if (!luna_is_userdata_tag(L, arg, tag)) {
        luna_error(L, "bad argument #%d (%s expected)", arg + (arg < 0 ? L->top + 1 : 1), tag);
    }
    return luna_to_userdata(L, arg);
}

