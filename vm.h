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
    Chunk       *chunk;      /* bytecode being executed       */
    int          ip;         /* index into chunk->code[]      */
    int          base;       /* stack offset for this frame   */
    ObjClosure  *closure;    /* closure for upvalue access    */
    int          ret_reg;    /* caller's destination register */
    int          nargs;      /* number of positional args passed */
    Value        kw_args;    /* kwargs dict or make_null() if none */
    struct ObjFunction *fn;  /* the function being executed (for param names) */
    struct GlobalEntry **saved_globals; /* non-NULL for module-import frames */
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

    /* Process arguments visible to stdlib modules */
    int         process_argc;
    char      **process_argv;

    /* Inline caches */
    IC_GlobalEntry global_ic[IC_CACHE_SIZE];
    IC_MemberEntry member_ic[IC_CACHE_SIZE];
    IC_MemberEntry method_ic[IC_CACHE_SIZE];
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

    /* Native exception jump stack (for longjmp from C builtins) */
    LunaJump *native_jump;
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

/* Native exception throw — usable from C builtin functions */
void luna_throw(VM *vm, struct ObjClass *error_class, const char *format, ...);
bool vm_call_native(VM *vm, NativeFn fn, Value *args, int arg_count, Value *out);

/* Stack trace — formats the call stack into a string buffer */
void vm_format_stack_trace(VM *vm, char *buf, size_t buf_size);

/* Upvalue capture — declared here because vm.c defines it after vm_run_chunk */
ObjUpvalue *capture_upvalue(VM *vm, int stack_idx);

#endif /* LUNA_VM_H */
