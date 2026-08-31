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

/* Host-callable ABI owned by the core. Frontends may typedef their own name
 * for the same signature, but the VM must not depend on that frontend name. */
typedef Value (*VMNativeFn)(struct VM *vm, Value *args, int arg_count);

/* Primitive operation identifiers are part of the VM ABI, not a language
 * object model.  A frontend can implement these operations without exposing
 * its classes, slots, or exception hierarchy to the core. */
typedef enum {
    VM_OP_ADD, VM_OP_SUB, VM_OP_MUL, VM_OP_DIV, VM_OP_MOD,
    VM_OP_IDIV, VM_OP_POW,
    VM_OP_NEG, VM_OP_BAND, VM_OP_BOR, VM_OP_BXOR, VM_OP_BNOT,
    VM_OP_SHL, VM_OP_SHR,
    VM_OP_EQ, VM_OP_NE, VM_OP_LT, VM_OP_LE, VM_OP_GT, VM_OP_GE
} VMOperation;

typedef bool (*VMUnaryOperation)(struct VM *vm, VMOperation op,
                                 Value operand, Value *result);
typedef bool (*VMBinaryOperation)(struct VM *vm, VMOperation op,
                                  Value left, Value right, Value *result);
typedef bool (*VMCompareOperation)(struct VM *vm, VMOperation op,
                                   Value left, Value right, Value *result);
typedef bool (*VMIndexGetOperation)(struct VM *vm, Value object, Value key,
                                    bool safe, Value *result);
typedef bool (*VMIndexSetOperation)(struct VM *vm, Value object, Value key,
                                     Value value);
typedef bool (*VMIndexSliceOperation)(struct VM *vm, Value object, Value start,
                                       Value stop, Value step, bool safe,
                                       Value *result);
typedef bool (*VMIterateOperation)(struct VM *vm, Value object, Value *iter,
                                   Value *state);
typedef bool (*VMIterNextOperation)(struct VM *vm, Value iter, Value *state,
                                    Value *elem);
typedef bool (*VMNewListOperation)(struct VM *vm, int capacity, Value *out);
typedef bool (*VMNewDictOperation)(struct VM *vm, Value *out);
typedef Value (*VMNewStringOperation)(struct VM *vm, const char *chars, int length);
typedef bool (*VMListAppendOperation)(struct VM *vm, Value list, Value value);
typedef bool (*VMConstructOperation)(struct VM *vm, Value class_name, Value *out);
typedef bool (*VMMemberGetOperation)(struct VM *vm, Value object, Value name,
                                     bool safe, Value *result);
typedef bool (*VMMemberSetOperation)(struct VM *vm, Value object, Value name,
                                     Value value);
typedef bool (*VMInstanceOfOperation)(struct VM *vm, Value obj, Value cls,
                                      bool *result);
typedef bool (*VMGetFieldSlotOperation)(struct VM *vm, Value obj, int slot,
                                        Value *out);
typedef bool (*VMSetFieldSlotOperation)(struct VM *vm, Value obj, int slot,
                                        Value value);
typedef bool (*VMInvokeOperation)(struct VM *vm, Value obj, Value name,
                                  Value *self_arg, Value *callable);
typedef bool (*VMContainsOperation)(struct VM *vm, Value needle, Value haystack,
                                    bool *found);
typedef bool (*VMImportOperation)(struct VM *vm, Value module_name,
                                  const char *from_path, Value *result);
typedef enum {
    VM_EXCEPTION_GENERIC, VM_EXCEPTION_TYPE, VM_EXCEPTION_KEY,
    VM_EXCEPTION_INDEX, VM_EXCEPTION_ATTRIBUTE, VM_EXCEPTION_VALUE,
    VM_EXCEPTION_RUNTIME, VM_EXCEPTION_ARGUMENT
} VMExceptionKind;
typedef Value (*VMExceptionOperation)(struct VM *vm, VMExceptionKind kind,
                                       const char *message);
typedef Value (*VMExceptionClassOperation)(struct VM *vm, void *cls,
                                            const char *message);

/* Frontend boundary.  The VM owns the execution machinery; a language
 * frontend supplies these optional hooks for roots and language objects.
 * Keeping this table in the core lets the Luna implementation evolve without
 * adding another Luna type to VM's public ABI. */
typedef struct VMFrontendHooks {
    void  (*mark_roots)(struct VM *vm);
    /* Upvalue lifecycle.  The open-upvalue list is frontend state: the core
     * captures/closes via these hooks and never inspects ObjUpvalue internals. */
    Object *(*capture_upvalue)(struct VM *vm, int stack_idx);
    void    (*close_upvalues)(struct VM *vm, int frame_depth);
    VMUnaryOperation unary;
    VMBinaryOperation binary;
    VMCompareOperation compare;
    VMIndexGetOperation getitem;
    VMIndexSetOperation setitem;
    VMMemberGetOperation getattr;
    VMMemberSetOperation setattr;
    VMIndexSliceOperation slice;
    VMIterateOperation iterate;
    VMIterNextOperation iter_next;
    VMNewListOperation new_list;
    VMNewDictOperation new_dict;
    VMNewStringOperation new_string;
    VMListAppendOperation list_append;
    VMConstructOperation construct;
    VMInstanceOfOperation instance_of;
    VMGetFieldSlotOperation get_field_slot;
    VMSetFieldSlotOperation set_field_slot;
    VMInvokeOperation invoke;
    VMInvokeOperation super_fn;
    VMContainsOperation contains;
    VMImportOperation import_module;
    VMExceptionOperation make_exception;
    /* Build an exception from a frontend class object.  Used by luna_throw,
     * which native functions call with the class pointer they were given. */
    VMExceptionClassOperation make_exception_for_class;
    /* Opaque type queries — the frontend owns the object model, so the core
     * asks "is this a string / instance?" through these hooks rather than
     * switching on inline object kinds (which would couple it to Luna). */
    bool (*is_string)(struct VM *vm, Value v);
    bool (*is_instance)(struct VM *vm, Value v);
    /* Build a closure from a function constant, capturing upvalues from the
     * current frame.  The frontend owns the closure layout, so OP_CLOSURE
     * delegates the whole construction here. */
    Object *(*new_closure)(struct VM *vm, Value fn_val);
} VMFrontendHooks;

/* ============================================================
 * FrontendDef — a language frontend's self-description + the
 * embeddable C API bridge (see api.h).  The embedder picks a
 * language by installing one of these on a VM; the core reaches
 * the object model only through these hooks, never by name.
 * ============================================================ */
typedef struct APIState APIState;
typedef int (*api_CFunction)(APIState *L);

/* Object-model operations the language-agnostic C API needs that aren't
 * already covered by the MOP Type vtable or VMFrontendHooks. */
typedef struct FrontendObject {
    int   (*type_of)(Value v);                      /* api_Type code */
    Value (*new_cfunction)(api_CFunction fn);       /* native function */
    bool  (*is_cfunction)(Value v);
    const char *(*cstring)(Value v, size_t *len);   /* string chars (owned) */
    Value (*new_userdata)(void *data, const char *tag, void (*finalizer)(void*));
    void *(*userdata_data)(Value v);
    const char *(*userdata_tag)(Value v);
    int64_t (*int64_value)(Value v);
    Value (*make_int64)(int64_t n);
} FrontendObject;

typedef struct FrontendDef {
    const char *name;
    void (*init_vm)(struct VM *vm);                 /* register builtins/classes */
    void (*wire_lifecycle)(void);                   /* one-time MOP vtable wiring */
    const VMFrontendHooks *hooks;
    const FrontendObject  *object;
    /* Lex + parse + compile source into a callable Value (closure).
     * Returns NULL on success, or a static error message on failure.
     * `is_repl` stores the last expression's value into the `_` global. */
    const char *(*compile_source)(struct VM *vm, const char *source,
                                  const char *path, bool is_repl, Value *out_fn);
} FrontendDef;

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
    Object *closure;     /* opaque callable for upvalue access (type->get_upvalue) */
    int ret_reg;         /* caller's destination register */
    int nargs;           /* number of positional args passed */
    Object *fn;          /* opaque callable being executed (type->name_of for module naming) */
    struct GlobalEntry **saved_globals; /* non-NULL for module-import frames */
    int leaf_ret_ip;     /* saved IP for leaf-call fast return */
    Chunk *leaf_ret_chunk; /* saved chunk for leaf-call fast return */
    int leaf_ret_base;   /* saved base for leaf-call fast return */
    int leaf_ret_reg;    /* saved return register for leaf-call fast return */
    Object *leaf_ret_closure; /* saved closure for leaf-call fast return */
    Object *leaf_ret_fn;       /* saved fn     for leaf-call fast return */
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
#define IC_CACHE_SIZE 1024  /* must be power of 2 */

typedef struct {
    void        *key;      /* frontend string object (pointer compare) */
    GlobalEntry *entry;    /* cached hash bucket entry */
} IC_GlobalEntry;

typedef struct {
    void        *klass;   /* frontend class object (pointer compare) */
    void        *name;    /* field/method name */
    int          index;    /* cached index */
} IC_MemberEntry;

typedef struct {
    void            *klass;   /* frontend class object (pointer compare) */
    void            *name;    /* interned method name */
    void            *method;  /* cached method object */
} IC_InvokeEntry;

typedef struct {
    Value        fn_val;     /* cached callable value (NaN-boxing compare) */
    void        *fn;         /* resolved callable */
    void        *cl;         /* closure (NULL for non-closure functions) */
} IC_CallEntry;

/* ---- VM State ---- */
typedef struct VM {
    const VMFrontendHooks *frontend;
    const FrontendDef     *frontend_def;
    void *frontend_state;
    void **frontend_slots;
    size_t frontend_slot_count;
    size_t frontend_slot_capacity;
    CallFrame   frames[VM_MAX_FRAMES];
    int         frame_count;

    GlobalEntry *globals[VM_GLOBAL_BUCKETS];
    GlobalEntry *system_globals[VM_GLOBAL_BUCKETS]; /* never swapped on import */

    /* Shared value stack */
    Value      *stack;
    int         stack_cap;
    int         stack_count;

    /* GC intrusive list */
    Object     *objects;

    /* Exception handling */
    TryFrame   *try_stack;

    /* Open upvalues (not yet closed) */
    void       *open_upvalues;

    /* Keyword-argument prefix state.  Set by OP_KW_PREFIX and consumed by the
     * immediately-following OP_CALL, then reset to zero.  The positional
     * fast path pays only a register read + compare here. */
    uint16_t    next_call_kw_idx;   /* constant-pool index of the kw-names tuple */
    uint8_t     next_call_kw_count; /* number of keyword arguments            */

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

    /* Frontend-owned runtime state. The core treats this as opaque: each
     * language stores its canonical classes, exception classes and module
     * cache here (see py/frontend_state.h, luna/frontend_state.h). */
    void *frontend_data;

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
void     vm_set_frontend(VM *vm, const VMFrontendHooks *hooks);
void     vm_install_frontend(VM *vm, const FrontendDef *fe);
bool     vm_unary(VM *vm, VMOperation op, Value operand, Value *result);
bool     vm_binary(VM *vm, VMOperation op, Value left, Value right, Value *result);
bool     vm_compare(VM *vm, VMOperation op, Value left, Value right, Value *result);
bool     vm_getitem(VM *vm, Value object, Value key, bool safe, Value *result);
bool     vm_setitem(VM *vm, Value object, Value key, Value value);
bool     vm_slice(VM *vm, Value object, Value start, Value stop, Value step,
                 bool safe, Value *result);
bool     vm_iterate(VM *vm, Value object, Value *iter, Value *state);
bool     vm_iter_next(VM *vm, Value iter, Value *state, Value *elem);
bool     vm_new_list(VM *vm, int capacity, Value *out);
bool     vm_new_dict(VM *vm, Value *out);
bool     vm_list_append(VM *vm, Value list, Value value);
bool     vm_construct(VM *vm, Value class_name, Value *out);
bool     vm_instance_of(VM *vm, Value obj, Value cls, bool *result);
bool     vm_get_field_slot(VM *vm, Value obj, int slot, Value *out);
bool     vm_set_field_slot(VM *vm, Value obj, int slot, Value value);
bool     vm_invoke(VM *vm, Value obj, Value name, Value *self_arg, Value *callable);
bool     vm_super_fn(VM *vm, Value self, Value name, Value *self_arg, Value *callable);
bool     vm_contains(VM *vm, Value needle, Value haystack, bool *found);
bool     vm_getattr(VM *vm, Value object, Value name, bool safe, Value *result);
bool     vm_setattr(VM *vm, Value object, Value name, Value value);
bool     vm_import_module(VM *vm, Value name, const char *from_path, Value *result);
Value    vm_make_exception(VM *vm, VMExceptionKind kind, const char *message);
int      vm_register_slot(VM *vm, void *value);
void    *vm_get_slot(const VM *vm, int slot);
void     vm_free(VM *vm);

void     vm_define_native(VM *vm, const char *name, VMNativeFn fn);
void     vm_set_process_args(VM *vm, int argc, char **argv);
void     vm_set_global(VM *vm, const char *name, Value value, bool is_const);
bool     vm_get_global(VM *vm, const char *name, Value *out);
GlobalEntry **vm_globals_save(VM *vm);
void          vm_globals_restore(VM *vm, GlobalEntry **saved);
void          vm_globals_fresh(VM *vm);
void         *vm_globals_to_dict(VM *vm);
bool     vm_get_global_fast(VM *vm, Value name, Value *out);
GlobalEntry *vm_resolve_global(VM *vm, const char *name);
void     vm_set_system_global(VM *vm, const char *name, Value value);

VMResult vm_run_chunk(VM *vm, Chunk *chunk);

/* Call a Luna function/closure/bound-method/native from C.
 * Handles frame setup, execution, and result extraction.
 * Returns VM_OK on success, VM_EXCEPTION on unhandled error. */
VMResult vm_call_value(VM *vm, Value fn_val, Value *args, int arg_count, Value *out);

/* Native exception throw — usable from C builtin functions */
void luna_throw(VM *vm, void *error_class, const char *format, ...);
bool vm_call_native(VM *vm, VMNativeFn fn, Value *args, int arg_count, Value *out);

/* Stack trace — formats the call stack into a string buffer (GCC-style) */
void vm_format_stack_trace(VM *vm, char *buf, size_t buf_size, const char *error_msg);

/* GC: mark a single value from outside vm.c (e.g., embedder API stack) */
void vm_mark_value(VM *vm, Value v);
void mark_and_sweep(VM *vm);

/* Upvalue capture — declared here because vm.c defines it after vm_run_chunk */
Object *capture_upvalue(VM *vm, int stack_idx);

#endif /* LUNA_VM_H */
