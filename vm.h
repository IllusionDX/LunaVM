/* vm.h — Luna Register-based Virtual Machine.
 *
 * Instruction format: 32-bit uint32_t (ABC / ABx / AsBx).
 * See opcode.h for encoding/decoding macros.
 *
 * The VM uses a single shared value stack; each CallFrame stores
 * a base offset so its registers are slices of that stack.
 */

#ifndef LUNA_VM_H
#define LUNA_VM_H

#include <stdbool.h>
#include <setjmp.h>
#include <stdarg.h>
#include "chunk.h"
#include "value.h"

/* ---- Limits ---- */
#define VM_MAX_FRAMES    256
#define VM_MAX_REGISTERS 256
#define MAX_FRAMES       VM_MAX_FRAMES

/* ---- Native exception jump stack ---- */
typedef struct LunaJump {
    jmp_buf env;
    struct LunaJump *prev;
} LunaJump;

/* ---- Call Frame ---- */
typedef struct {
    Chunk *chunk;        /* bytecode being executed */
    int ip;              /* index into chunk->code[] */
    int base;            /* stack offset for this frame */
    ObjClosure *closure; /* closure for upvalue access */
    int ret_reg;         /* caller's destination register */
    int nargs;           /* number of positional args passed */
    Value kw_args;       /* kwargs dict or make_null() if none */
    struct ObjFunction *fn; /* the function being executed (for param names) */
    struct GlobalEntry **saved_globals; /* non-NULL for module-import frames */
    int leaf_ret_ip;     /* saved IP for leaf-call fast return */
    Chunk *leaf_ret_chunk; /* saved chunk for leaf-call fast return */
    int leaf_ret_base;   /* saved base for leaf-call fast return */
    int leaf_ret_reg;    /* saved return register for leaf-call fast return */
    ObjClosure        *leaf_ret_closure; /* saved closure for leaf-call fast return */
    struct ObjFunction *leaf_ret_fn;     /* saved fn     for leaf-call fast return */
} CallFrame;

/* ---- Global variable entry (chained hash table) ---- */
typedef struct GlobalEntry {
    char              *name;
    Value              value;
    bool               is_const;
    struct GlobalEntry *next;
} GlobalEntry;

#define VM_GLOBAL_BUCKETS 256

/* ---- Exception handling frame ---- */
typedef struct TryFrame {
    int catch_ip;
    int frame_depth;   /* vm->frame_count when TRY was pushed */
    int stack_count;   /* vm->stack_count when TRY was pushed */
    int exc_reg;       /* register to receive exception value */
    struct TryFrame *next;
} TryFrame;

/* ---- Inline caches ---- */
#define IC_CACHE_SIZE 64  /* must be power of 2 */

typedef struct {
    ObjString   *key;      /* interned string (pointer compare) */
    GlobalEntry *entry;    /* cached hash bucket entry */
} IC_GlobalEntry;

typedef struct {
    ObjInstance *inst;     /* cached instance */
    ObjString   *name;     /* field/method name */
    int          index;    /* cached index */
} IC_MemberEntry;

typedef struct {
    struct ObjClass *klass;   /* cached class (pointer compare) */
    ObjString       *name;    /* interned method name */
    ObjFunction     *method;  /* cached method pointer */
} IC_InvokeEntry;

typedef struct {
    Value        fn_val;     /* cached callable value (NaN-boxing compare) */
    ObjFunction *fn;         /* resolved function */
    ObjClosure  *cl;         /* closure (NULL for non-closure functions) */
} IC_CallEntry;

/* ---- VM State ---- */
typedef struct VM {
    CallFrame   frames[VM_MAX_FRAMES];
    int         frame_count;

    GlobalEntry *globals[VM_GLOBAL_BUCKETS];

    /* Shared value stack */
    Value      *stack;
    int         stack_cap;
    int         stack_count;

    /* GC intrusive list */
    Object     *objects;

    /* Exception handling */
    TryFrame   *try_stack;

    /* Open upvalues (not yet closed) */
    ObjUpvalue *open_upvalues;

    /* Last unhandled exception */
    Value       last_exception;

    /* Last return value from executed chunk (set by OP_RET top-level path) */
    Value       last_return_value;

    /* Process arguments visible to stdlib modules */
    int         process_argc;
    char      **process_argv;

    /* Inline caches */
    IC_GlobalEntry global_ic[IC_CACHE_SIZE];
    IC_MemberEntry member_ic[IC_CACHE_SIZE];
    IC_MemberEntry method_ic[IC_CACHE_SIZE];
    IC_InvokeEntry invoke_ic[IC_CACHE_SIZE];
    IC_CallEntry   call_ic[IC_CACHE_SIZE];
    uint64_t       instr_count;
    uint64_t       time_start_us; /* monotonic microseconds captured at vm_init */

    /* Module cache — maps module name string to ObjModule */
    ObjDict *module_cache;

    /* Built-in Exception classes (fast access) */
    struct ObjClass *exception_class;
    struct ObjClass *type_error_class;
    struct ObjClass *key_error_class;
    struct ObjClass *index_error_class;
    struct ObjClass *attribute_error_class;
    struct ObjClass *value_error_class;
    struct ObjClass *runtime_error_class;
    struct ObjClass *argument_error_class;

    /* Canonical classes for built-in types (fast dispatch) */
    struct ObjClass *string_class;
    struct ObjClass *list_class;
    struct ObjClass *dict_class;
    struct ObjClass *enum_class;
    struct ObjClass *buffer_class;
    struct ObjClass *vector_class;
    struct ObjClass *matrix_class;
    struct ObjClass *function_class;
    struct ObjClass *closure_class;
    struct ObjClass *bound_method_class;
    struct ObjClass *class_class;
    struct ObjClass *module_class;
    struct ObjClass *userdata_class;

    /* Native exception jump stack (for longjmp from C builtins) */
    LunaJump *native_jump;

    /* Opaque pointer for embedder (used by luna.h GC marking) */
    void *api_state;
} VM;

/* ---- Result codes ---- */
typedef enum { VM_OK = 0, VM_EXCEPTION = 1, VM_ERROR = 2 } VMResult;

/* ============================================================ */
/* Public API                                                    */
/* ============================================================ */

void     vm_init(VM *vm);
void     vm_free(VM *vm);

void     vm_define_native(VM *vm, const char *name, NativeFn fn);
void     vm_set_process_args(VM *vm, int argc, char **argv);
void     vm_set_global(VM *vm, const char *name, Value value, bool is_const);
bool     vm_get_global(VM *vm, const char *name, Value *out);
bool     vm_get_global_fast(VM *vm, ObjString *name, Value *out);
GlobalEntry *vm_resolve_global(VM *vm, const char *name);

VMResult vm_run_chunk(VM *vm, Chunk *chunk);

/* Call a Luna function/closure/bound-method/native from C.
 * Handles frame setup, execution, and result extraction.
 * Returns VM_OK on success, VM_EXCEPTION on unhandled error. */
VMResult vm_call_value(VM *vm, Value fn_val, Value *args, int arg_count, Value *out);

/* Native exception throw — usable from C builtin functions */
void luna_throw(VM *vm, struct ObjClass *error_class, const char *format, ...);
bool vm_call_native(VM *vm, NativeFn fn, Value *args, int arg_count, Value *out);

/* Stack trace — formats the call stack into a string buffer (GCC-style) */
void vm_format_stack_trace(VM *vm, char *buf, size_t buf_size, const char *error_msg);

/* GC: mark a single value from outside vm.c (e.g., embedder API stack) */
void vm_mark_value(VM *vm, Value v);
void mark_and_sweep(VM *vm);

/* C function dispatch — called by VM when fn->cfunc is non-NULL */
Value luna_cfunc_dispatch(VM *vm, struct ObjFunction *fn, Value *args, int arg_count);

/* Upvalue capture — declared here because vm.c defines it after vm_run_chunk */
ObjUpvalue *capture_upvalue(VM *vm, int stack_idx);

#endif /* LUNA_VM_H */
