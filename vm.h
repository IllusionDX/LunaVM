/* vm.h — Luna Register-based Virtual Machine.
 *
 * Instruction format: 32-bit uint32_t (ABC / ABx / AsBx).
 * See opcode.h for encoding/decoding macros.
 */

#ifndef LUNA_VM_H
#define LUNA_VM_H

#include <stdbool.h>
#include "chunk.h"
#include "value.h"

/* ---- Limits ---- */
#define VM_MAX_FRAMES    256
#define VM_MAX_REGISTERS 256

/* ---- Call Frame ---- */
typedef struct {
    Chunk   *chunk;                      /* bytecode being executed       */
    int      ip;                         /* index into chunk->code[]      */
    Value    regs[VM_MAX_REGISTERS];     /* register window               */
    int      ret_reg;                    /* caller's destination register */
    bool     has_self;
    Value    self_val;
    ObjUpvalue *upvalues[VM_MAX_REGISTERS]; /* captured upvalues          */
    int      upvalue_count;
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
    int exc_reg;
    int frame_depth;   /* vm->frame_count when TRY was pushed */
    struct TryFrame *next;
} TryFrame;

/* Forward declaration */
typedef struct ObjUpvalue ObjUpvalue;

/* ---- VM State ---- */
typedef struct VM {
    CallFrame   frames[VM_MAX_FRAMES];
    int         frame_count;

    GlobalEntry *globals[VM_GLOBAL_BUCKETS];

    /* GC intrusive list */
    Object     *objects;

    /* Exception handling */
    TryFrame   *try_stack;

    /* Open upvalues (not yet closed) */
    ObjUpvalue *open_upvalues;

    /* Last unhandled exception */
    Value       last_exception;
} VM;

/* ---- Result codes ---- */
typedef enum { VM_OK = 0, VM_EXCEPTION = 1, VM_ERROR = 2 } VMResult;

/* ============================================================ */
/* Public API                                                    */
/* ============================================================ */

void     vm_init(VM *vm);
void     vm_free(VM *vm);

void     vm_define_native(VM *vm, const char *name, NativeFn fn);
void     vm_set_global(VM *vm, const char *name, Value value, bool is_const);
bool     vm_get_global(VM *vm, const char *name, Value *out);
GlobalEntry *vm_resolve_global(VM *vm, const char *name);

VMResult vm_run_chunk(VM *vm, Chunk *chunk);

#endif /* LUNA_VM_H */
