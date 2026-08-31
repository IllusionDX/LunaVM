/* compiler.c — AST → Bytecode compiler. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "compiler.h"
#include "opcode.h"
#include "value.h"
#include "py/object.h"

/* ---- Scope / Locals ---- */

typedef struct Local {
    char *name;
    int   reg;
} Local;

typedef struct Scope {
    Local  locals[VM_MAX_REGISTERS];
    int    local_count;
    int    saved_base;      /* temp_base to restore on exit */
    struct Scope *parent;
} Scope;

/* ---- Loop patch info ---- */

typedef struct LoopInfo {
    int    start_ip;        /* backward jump target */
    int    continue_ip;     /* -1 until known */
    int   *break_patches;
    int    break_count;
    int    break_cap;
    int   *continue_patches;
    int    continue_count;
    int    continue_cap;
    struct LoopInfo *next;
} LoopInfo;

/* ---- Finally context (compile-time inlining) ---- */

typedef struct FinallyCtx {
    Stmt  **finally_body;
    int     finally_count;
    bool    has_catch;
    struct LoopInfo *enclosing_loop;
    struct FinallyCtx *next;
} FinallyCtx;

/* ---- Opcode mapping for compare-and-branch immediate variants ---- */

static const OpCode cmp_to_imm_direct[] = {
    [OP_LT_JZ] = OP_LT_JZ_IMM,
    [OP_LE_JZ] = OP_LE_JZ_IMM,
    [OP_GT_JZ] = OP_GT_JZ_IMM,
    [OP_GE_JZ] = OP_GE_JZ_IMM,
    [OP_EQ_JZ] = OP_EQ_JZ_IMM,
    [OP_NE_JZ] = OP_NE_JZ_IMM,
};

static const OpCode cmp_to_imm_swapped[] = {
    [OP_LT_JZ] = OP_GT_JZ_IMM,    /* a < const  → const > a */
    [OP_LE_JZ] = OP_GE_JZ_IMM,    /* a <= const → const >= a */
    [OP_GT_JZ] = OP_LT_JZ_IMM,    /* a > const  → const < a */
    [OP_GE_JZ] = OP_LE_JZ_IMM,    /* a >= const → const <= a */
    [OP_EQ_JZ] = OP_EQ_JZ_IMM,    /* a == const → const == a */
    [OP_NE_JZ] = OP_NE_JZ_IMM,    /* a != const → const != a */
};

/* ---- Upvalue tracking ---- */

typedef struct CompilerUpvalue {
    uint8_t index;
    bool    is_local;
} CompilerUpvalue;

/* ---- Compiler state ---- */

typedef struct Compiler {
    Chunk      *chunk;
    Scope      *scope;
    VM         *vm;
    int         temp_base;  /* next free temp register */
    int         max_temp_base; /* high-water mark of temp_base */
    int         line;       /* V1: always 1 (AST has no lines) */
    int         func_depth; /* 0 = top-level */
    LoopInfo   *loop;
    FinallyCtx *finally_ctx;
    struct Compiler *parent;
    bool        is_repl;
    ObjClass   *current_class; /* class whose method we're compiling, NULL at top-level */
    CompilerUpvalue upvalues[VM_MAX_REGISTERS];
    int         upvalue_count;
} Compiler;

/* ============================================================ */
/* Scope helpers                                                */
/* ============================================================ */

static void scope_enter(Compiler *c) {
    Scope *s = malloc(sizeof(Scope));
    s->local_count = 0;
    s->saved_base  = c->temp_base;
    s->parent      = c->scope;
    c->scope       = s;
}

static void scope_exit(Compiler *c) {
    Scope *s = c->scope;
    c->temp_base = s->saved_base;
    c->scope     = s->parent;
    free(s);
}

static int resolve_local(Compiler *c, const char *name) {
    for (Scope *s = c->scope; s; s = s->parent) {
        for (int i = s->local_count - 1; i >= 0; i--) {
            if (strcmp(s->locals[i].name, name) == 0)
                return s->locals[i].reg;
        }
    }
    return -1;
}

static int add_upvalue(Compiler *c, uint8_t index, bool is_local) {
    int count = c->upvalue_count;
    for (int i = 0; i < count; i++) {
        if (c->upvalues[i].index == index && c->upvalues[i].is_local == is_local)
            return i;
    }
    if (count >= VM_MAX_REGISTERS) {
        fprintf(stderr, "compiler: too many upvalues\n");
        return 0;
    }
    c->upvalues[count].index = index;
    c->upvalues[count].is_local = is_local;
    return c->upvalue_count++;
}

static int resolve_upvalue(Compiler *c, const char *name);

static int resolve_upvalue(Compiler *c, const char *name) {
    if (!c->parent) return -1;
    int local = resolve_local(c->parent, name);
    if (local != -1) {
        return add_upvalue(c, (uint8_t)local, true);
    }
    int upvalue = resolve_upvalue(c->parent, name);
    if (upvalue != -1) {
        return add_upvalue(c, (uint8_t)upvalue, false);
    }
    return -1;
}

typedef enum { VAR_LOCAL, VAR_UPVALUE, VAR_GLOBAL } VarKind;

static VarKind resolve_variable(Compiler *c, const char *name, int *index) {
    int local = resolve_local(c, name);
    if (local != -1) {
        *index = local;
        return VAR_LOCAL;
    }
    int upvalue = resolve_upvalue(c, name);
    if (upvalue != -1) {
        *index = upvalue;
        return VAR_UPVALUE;
    }
    return VAR_GLOBAL;
}

/* Try to resolve a field slot index for self.field access.
 * Returns -1 if not applicable (not self, unknown class, or unknown field). */
static int resolve_field_slot(Compiler *c, const char *field_name) {
    if (!c->current_class || !c->current_class->prototype) return -1;
    ObjInstance *proto = c->current_class->prototype;
    for (int i = 0; i < proto->field_count; i++) {
        if (proto->field_names[i] && strcmp(proto->field_names[i], field_name) == 0)
            return i;
    }
    return -1;
}

static int add_local(Compiler *c, const char *name, int reg) {
    if (!c->scope) return reg;
    Scope *s = c->scope;
    if (s->local_count >= VM_MAX_REGISTERS) {
        fprintf(stderr, "compiler: too many locals\n");
        return reg;
    }
    s->locals[s->local_count].name = (char *)name;
    s->locals[s->local_count].reg  = reg;
    s->local_count++;
    return reg;
}

/* ============================================================ */
/* Register allocation                                          */
/* ============================================================ */

static int alloc_reg(Compiler *c) {
    if (c->temp_base >= VM_MAX_REGISTERS) {
        fprintf(stderr, "compiler: out of registers\n");
        return VM_MAX_REGISTERS - 1;
    }
    int r = c->temp_base++;
    if (c->temp_base > c->max_temp_base) c->max_temp_base = c->temp_base;
    return r;
}

static void free_reg(Compiler *c) {
    if (c->temp_base > 0) c->temp_base--;
}

/* ============================================================ */
/* Emit helpers                                                 */
/* ============================================================ */

static void emit_ABC(Compiler *c, OpCode op, uint8_t a, uint8_t b, uint8_t ci) {
    chunk_emit_ABC(c->chunk, c->line, op, a, b, ci);
}

static void emit_ABx(Compiler *c, OpCode op, uint8_t a, uint16_t bx) {
    chunk_emit_ABx(c->chunk, c->line, op, a, bx);
}

static void emit_AsBx(Compiler *c, OpCode op, uint8_t a, int32_t sbx) {
    chunk_emit_AsBx(c->chunk, c->line, op, a, sbx);
}

static int emit_jump(Compiler *c, OpCode op, uint8_t reg) {
    return chunk_emit_AsBx(c->chunk, c->line, op, reg, 0);
}

static void patch_jump(Compiler *c, int jump_idx) {
    int target = c->chunk->count;
    int sbx    = target - (jump_idx + 1);
    chunk_patch_sBx(c->chunk, jump_idx, sbx);
}

static void emit_ret(Compiler *c, uint8_t reg) {
    emit_ABC(c, OP_RET, reg, 0, 0);
}

static void emit_loadi(Compiler *c, uint8_t dst, int32_t v) {
    if (v >= -32767 && v <= 32768) {
        emit_AsBx(c, OP_LOADI, dst, v);
    } else {
        int k = chunk_add_const(c->chunk, make_int(v));
        emit_ABx(c, OP_LOADK, dst, (uint16_t)k);
    }
}
static void emit_loadbool(Compiler *c, uint8_t dst, bool v) {
    emit_ABC(c, v ? OP_LOADTRUE : OP_LOADFALSE, dst, 0, 0);
}
static void emit_loadnull(Compiler *c, uint8_t dst) {
    emit_ABC(c, OP_LOADNULL, dst, 0, 0);
}
static void emit_loadstring_len(Compiler *c, uint8_t dst, const char *s, int length) {
    int k = chunk_add_string_len(c->vm, c->chunk, s, length);
    emit_ABx(c, OP_LOADK, dst, (uint16_t)k);
}

static void emit_move(Compiler *c, uint8_t dst, uint8_t src) {
    emit_ABC(c, OP_MOVE, dst, src, 0);
}

/* ---- Integer literal helpers (arbitrary precision) ---- */

/* Parse a literal into its exact int64 view; INT64_MIN when it is a bigint
 * beyond int64 (disables the fused int8 paths it feeds). */
static int64_t compile_int_literal_i64(const char *s) {
    Value v;
    if (!bigint_from_decimal(s, strlen(s), &v)) return 0;
    if (IS_INT(v)) return AS_INT(v);
    int64_t r;
    if (bigint_get_i64((ObjBigInt *)AS_OBJ(v), &r)) return r;
    return INT64_MIN;
}

/* Parse a literal into its correctly-rounded double view. */
static double compile_int_literal_f64(const char *s) {
    Value v;
    if (!bigint_from_decimal(s, strlen(s), &v)) return 0.0;
    if (IS_INT(v)) return (double)AS_INT(v);
    return bigint_to_f64((ObjBigInt *)AS_OBJ(v));
}

/* Emit an integer constant Value: int32 immediates via the fast load
 * opcodes, anything bigger as a chunk constant (a heap bigint). */
static void emit_int_const(Compiler *c, int target, Value v) {
    if (IS_INT(v)) {
        int32_t i = AS_INT(v);
        if (i >= -32767 && i <= 32768)
            emit_AsBx(c, OP_LOADI, (uint8_t)target, i);
        else
            emit_loadi(c, (uint8_t)target, i);
        return;
    }
    int k = chunk_add_const(c->chunk, v);
    emit_ABx(c, OP_LOADK, (uint8_t)target, (uint16_t)k);
}

/* ============================================================ */
/* Loop helpers                                                 */
/* ============================================================ */

static void loop_push(Compiler *c, int start_ip) {
    LoopInfo *l = malloc(sizeof(LoopInfo));
    l->start_ip          = start_ip;
    l->continue_ip       = -1;
    l->break_patches     = NULL;
    l->break_count       = 0;
    l->break_cap         = 0;
    l->continue_patches  = NULL;
    l->continue_count    = 0;
    l->continue_cap      = 0;
    l->next              = c->loop;
    c->loop              = l;
}

static void loop_patch_breaks(Compiler *c) {
    LoopInfo *l = c->loop;
    for (int i = 0; i < l->break_count; i++)
        patch_jump(c, l->break_patches[i]);
}

static void loop_patch_continues(Compiler *c) {
    LoopInfo *l = c->loop;
    if (l->continue_ip < 0) return;
    for (int i = 0; i < l->continue_count; i++) {
        int idx = l->continue_patches[i];
        int sbx = l->continue_ip - (idx + 1);
        chunk_patch_sBx(c->chunk, idx, sbx);
    }
}

static void loop_record_break(Compiler *c, int jump_idx) {
    LoopInfo *l = c->loop;
    if (!l) return;
    if (l->break_count >= l->break_cap) {
        l->break_cap = l->break_cap ? l->break_cap * 2 : 4;
        l->break_patches = realloc(l->break_patches, sizeof(int) * l->break_cap);
    }
    l->break_patches[l->break_count++] = jump_idx;
}

static void loop_record_continue(Compiler *c, int jump_idx) {
    LoopInfo *l = c->loop;
    if (!l) return;
    if (l->continue_count >= l->continue_cap) {
        l->continue_cap = l->continue_cap ? l->continue_cap * 2 : 4;
        l->continue_patches = realloc(l->continue_patches, sizeof(int) * l->continue_cap);
    }
    l->continue_patches[l->continue_count++] = jump_idx;
}

static void loop_pop(Compiler *c) {
    LoopInfo *l = c->loop;
    c->loop = l->next;
    free(l->break_patches);
    free(l->continue_patches);
    free(l);
}

/* ============================================================ */
/* Forward declarations                                         */
/* ============================================================ */

static void compile_expr_into(Compiler *c, Expr *expr, int target);
static void compile_stmt(Compiler *c, Stmt *stmt);
static void compile_decl(Compiler *c, Decl *decl);
static Value compile_default_thunk(Compiler *c, Expr *expr);
static int build_kw_names_const(Compiler *c, Expr *expr, int nargs);
static int compile_function_value(Compiler *c, const char *name,
                                   FunctionParam *params, int param_count,
                                   Stmt **body, int body_count);

/* ============================================================ */
/* Finally context: compile-time inlining                       */
/* ============================================================ */

static void finally_ctx_push(Compiler *c, Stmt **body, int count, bool has_catch) {
    FinallyCtx *f = malloc(sizeof(FinallyCtx));
    f->finally_body   = body;
    f->finally_count  = count;
    f->has_catch      = has_catch;
    f->enclosing_loop = c->loop;
    f->next           = c->finally_ctx;
    c->finally_ctx    = f;
}

static void finally_ctx_pop(Compiler *c) {
    FinallyCtx *f = c->finally_ctx;
    if (f) {
        c->finally_ctx = f->next;
        free(f);
    }
}

/* Emit finally blocks for an early exit (break/continue/return).
 * Walks the FinallyCtx chain innermost-first.
 * If stop_at is non-NULL, only emits contexts nested inside that loop. */
static void emit_finally_blocks_for_early_exit(Compiler *c, LoopInfo *stop_at) {
    FinallyCtx *f = c->finally_ctx;
    if (!f) return;

    int count = 0;
    for (FinallyCtx *cur = f; cur; cur = cur->next) {
        bool relevant = (stop_at == NULL);
        if (stop_at) {
            for (LoopInfo *l = cur->enclosing_loop; l; l = l->next) {
                if (l == stop_at) { relevant = true; break; }
            }
        }
        if (relevant) count++;
    }
    if (count == 0) return;

    FinallyCtx **buf = malloc(sizeof(FinallyCtx*) * count);
    int idx = 0;
    for (FinallyCtx *cur = f; cur; cur = cur->next) {
        bool relevant = (stop_at == NULL);
        if (stop_at) {
            for (LoopInfo *l = cur->enclosing_loop; l; l = l->next) {
                if (l == stop_at) { relevant = true; break; }
            }
        }
        if (relevant) buf[idx++] = cur;
    }

    for (int i = 0; i < count; i++) {
        FinallyCtx *ctx = buf[i];
        if (ctx->has_catch) {
            emit_ABC(c, OP_ENDTRY, 0, 0, 0);
        }
        scope_enter(c);
        for (int j = 0; j < ctx->finally_count; j++)
            compile_stmt(c, ctx->finally_body[j]);
        scope_exit(c);
    }
    free(buf);
}

/* ============================================================ */
/* Assignment helpers                                         */
/* ============================================================ */

static void compile_single_assignment(Compiler *c, Expr *lhs, int src_reg) {
    if (lhs->kind == EXPR_IDENTIFIER) {
        const char *name = lhs->data.identifier.name;
        int idx;
        VarKind kind = resolve_variable(c, name, &idx);
        if (kind == VAR_LOCAL) {
            if (idx != src_reg) emit_move(c, (uint8_t)idx, (uint8_t)src_reg);
        } else if (kind == VAR_UPVALUE) {
            emit_ABx(c, OP_SETUPVAL, (uint8_t)src_reg, (uint16_t)idx);
        } else {
            int k = chunk_add_string(c->vm, c->chunk, name);
            emit_ABx(c, OP_SETGLOBAL, (uint8_t)src_reg, (uint16_t)k);
        }
    } else if (lhs->kind == EXPR_FIELD_ACCESS) {
        int obj = alloc_reg(c);
        compile_expr_into(c, lhs->data.field_access.obj, obj);
        int slot = -1;
        if (lhs->data.field_access.obj->kind == EXPR_IDENTIFIER) {
            int self_idx;
            if (resolve_variable(c, lhs->data.field_access.obj->data.identifier.name, &self_idx) == VAR_LOCAL && self_idx == 0) {
                slot = resolve_field_slot(c, lhs->data.field_access.field);
            }
        }
        if (slot >= 0 && slot <= 255) {
            emit_ABC(c, OP_SETFIELD, (uint8_t)obj, (uint8_t)src_reg, (uint8_t)slot);
        } else {
            int fk = chunk_add_string(c->vm, c->chunk, lhs->data.field_access.field);
            if (fk > 255) {
                int temp = alloc_reg(c);
                emit_ABx(c, OP_LOADK, (uint8_t)temp, (uint16_t)fk);
                emit_ABC(c, OP_MEMBERSET, (uint8_t)obj, (uint8_t)src_reg, (uint8_t)temp);
                free_reg(c);
            } else {
                emit_ABC(c, OP_MEMBERSET, (uint8_t)obj, (uint8_t)src_reg, (uint8_t)fk);
            }
        }
        free_reg(c);
    } else if (lhs->kind == EXPR_INDEX_ACCESS) {
        int obj = alloc_reg(c);
        int idx = alloc_reg(c);
        compile_expr_into(c, lhs->data.index_access.obj, obj);
        compile_expr_into(c, lhs->data.index_access.index, idx);
        emit_ABC(c, OP_INDEXSET, (uint8_t)obj, (uint8_t)idx, (uint8_t)src_reg);
        free_reg(c);
        free_reg(c);
    }
}

/* ============================================================ */
/* Expressions                                                  */
/* ============================================================ */

static void compile_expr_into(Compiler *c, Expr *expr, int target) {
    switch (expr->kind) {

    case EXPR_INTEGER: {
        Value v;
        bigint_from_decimal(expr->data.integer.value, strlen(expr->data.integer.value), &v);
        emit_int_const(c, (uint8_t)target, v);
        break;
    }

    case EXPR_FLOAT: {
        double v = atof(expr->data.float_lit.value);
        int k = chunk_add_const(c->chunk, make_double(v));
        emit_ABx(c, OP_LOADK, (uint8_t)target, (uint16_t)k);
        break;
    }

    case EXPR_STRING: {
        emit_loadstring_len(c, (uint8_t)target, expr->data.string.value, expr->data.string.length);
        break;
    }

    case EXPR_BOOL: {
        emit_loadbool(c, (uint8_t)target, expr->data.boolean.value);
        break;
    }

    case EXPR_NULL: {
        emit_loadnull(c, (uint8_t)target);
        break;
    }

    case EXPR_IDENTIFIER: {
        int idx;
        VarKind kind = resolve_variable(c, expr->data.identifier.name, &idx);
        if (kind == VAR_LOCAL) {
            if (idx != target) emit_move(c, (uint8_t)target, (uint8_t)idx);
        } else if (kind == VAR_UPVALUE) {
            emit_ABx(c, OP_GETUPVAL, (uint8_t)target, (uint16_t)idx);
        } else {
            int k = chunk_add_string(c->vm, c->chunk, expr->data.identifier.name);
            emit_ABx(c, OP_GETGLOBAL, (uint8_t)target, (uint16_t)k);
        }
        break;
    }

    case EXPR_BINARY: {
        const char *op_str = expr->data.binary.operator;

        if (strcmp(op_str, "and") == 0 || strcmp(op_str, "&&") == 0) {
            compile_expr_into(c, expr->data.binary.left, target);
            /* Short-circuit AND */
            int t = alloc_reg(c);
            emit_loadbool(c, (uint8_t)t, false);
            int jz1 = emit_jump(c, OP_JZ, (uint8_t)target);
            compile_expr_into(c, expr->data.binary.right, target);
            int jz2 = emit_jump(c, OP_JZ, (uint8_t)target);
            emit_loadbool(c, (uint8_t)target, true);
            int j1 = emit_jump(c, OP_JMP, 0);
            patch_jump(c, jz1);
            patch_jump(c, jz2);
            emit_move(c, (uint8_t)target, (uint8_t)t);
            patch_jump(c, j1);
            free_reg(c); /* t */
            break;
        }
        else if (strcmp(op_str, "or") == 0 || strcmp(op_str, "||") == 0) {
            compile_expr_into(c, expr->data.binary.left, target);
            /* Short-circuit OR */
            int t = alloc_reg(c);
            emit_loadbool(c, (uint8_t)t, true);
            int jnz1 = emit_jump(c, OP_JNZ, (uint8_t)target);
            compile_expr_into(c, expr->data.binary.right, target);
            int jnz2 = emit_jump(c, OP_JNZ, (uint8_t)target);
            emit_loadbool(c, (uint8_t)target, false);
            int j1 = emit_jump(c, OP_JMP, 0);
            patch_jump(c, jnz1);
            patch_jump(c, jnz2);
            emit_move(c, (uint8_t)target, (uint8_t)t);
            patch_jump(c, j1);
            free_reg(c);
            break;
        }
        else if (strcmp(op_str, "??") == 0) {
            compile_expr_into(c, expr->data.binary.left, target);
            /* Short-circuit ?? : if target != null, skip RHS */
            int j_coalesce = emit_jump(c, OP_COALESCE, (uint8_t)target);
            compile_expr_into(c, expr->data.binary.right, target);
            patch_jump(c, j_coalesce);
            break;
        }

        /* ============================================================ */
        /* Constant folding: evaluate at compile time if both operands  */
        /* are literals instead of emitting runtime opcodes.            */
        /* ============================================================ */
        {
            Expr *l = expr->data.binary.left;
            Expr *r = expr->data.binary.right;
            bool folded = false;

            /* int op int — exact arbitrary-precision folding */
            if (l->kind == EXPR_INTEGER && r->kind == EXPR_INTEGER) {
                Value lv, rv;
                bigint_from_decimal(l->data.integer.value, strlen(l->data.integer.value), &lv);
                bigint_from_decimal(r->data.integer.value, strlen(r->data.integer.value), &rv);
                bool rzero = (IS_INT(rv) && AS_INT(rv) == 0) ||
                             (IS_BIGINT(rv) && bigint_is_zero((ObjBigInt *)AS_OBJ(rv)));

                if (strcmp(op_str, "+") == 0 || strcmp(op_str, "-") == 0 ||
                    strcmp(op_str, "*") == 0) {
                    VMOperation op = strcmp(op_str, "+") == 0 ? VM_OP_ADD
                                 : strcmp(op_str, "-") == 0 ? VM_OP_SUB : VM_OP_MUL;
                    Value res = bigint_binary_value(c->vm, op, lv, rv);
                    if (!IS_NIL(res)) {
                        emit_int_const(c, (uint8_t)target, res);
                        folded = true;
                    }
                }
                else if (strcmp(op_str, "/") == 0) {
                    /* float / → true division, kept for runtime */
                }
                else if (strcmp(op_str, "%") == 0 && !rzero) {
                    Value res = bigint_binary_value(c->vm, VM_OP_MOD, lv, rv);
                    if (!IS_NIL(res)) {
                        emit_int_const(c, (uint8_t)target, res);
                        folded = true;
                    }
                }
                else if (strcmp(op_str, "<") == 0 || strcmp(op_str, "<=") == 0 ||
                         strcmp(op_str, ">") == 0 || strcmp(op_str, ">=") == 0 ||
                         strcmp(op_str, "==") == 0 || strcmp(op_str, "!=") == 0) {
                    int cmp;
                    if (IS_INT(lv) && IS_INT(rv)) {
                        int64_t li = AS_INT(lv), ri = AS_INT(rv);
                        cmp = (li < ri) ? -1 : (li > ri) ? 1 : 0;
                    } else if (IS_BIGINT(lv) && IS_BIGINT(rv)) {
                        cmp = bigint_cmp((ObjBigInt *)AS_OBJ(lv), (ObjBigInt *)AS_OBJ(rv));
                    } else {
                        cmp = IS_BIGINT(lv)
                            ? bigint_cmp_value((ObjBigInt *)AS_OBJ(lv), rv)
                            : -bigint_cmp_value((ObjBigInt *)AS_OBJ(rv), lv);
                    }
                    if (strcmp(op_str, "<") == 0) emit_loadbool(c, (uint8_t)target, cmp < 0);
                    else if (strcmp(op_str, "<=") == 0) emit_loadbool(c, (uint8_t)target, cmp <= 0);
                    else if (strcmp(op_str, ">") == 0) emit_loadbool(c, (uint8_t)target, cmp > 0);
                    else if (strcmp(op_str, ">=") == 0) emit_loadbool(c, (uint8_t)target, cmp >= 0);
                    else if (strcmp(op_str, "==") == 0) emit_loadbool(c, (uint8_t)target, cmp == 0);
                    else emit_loadbool(c, (uint8_t)target, cmp != 0);
                    folded = true;
                }
            }
            /* float op float  /  int op float  /  float op int */
            else if ((l->kind == EXPR_FLOAT || l->kind == EXPR_INTEGER) &&
                     (r->kind == EXPR_FLOAT || r->kind == EXPR_INTEGER) &&
                     (l->kind == EXPR_FLOAT || r->kind == EXPR_FLOAT)) {
                double ld = (l->kind == EXPR_FLOAT) ? atof(l->data.float_lit.value)
                                                    : compile_int_literal_f64(l->data.integer.value);
                double rd = (r->kind == EXPR_FLOAT) ? atof(r->data.float_lit.value)
                                                    : compile_int_literal_f64(r->data.integer.value);
                double result = 0.0;
                bool valid = true;

                if (strcmp(op_str, "+") == 0) result = ld + rd;
                else if (strcmp(op_str, "-") == 0) result = ld - rd;
                else if (strcmp(op_str, "*") == 0) result = ld * rd;
                else if (strcmp(op_str, "/") == 0) result = ld / rd;  /* Inf/NaN ok */
                else if (strcmp(op_str, "%") == 0) result = fmod(ld, rd);
                else if (strcmp(op_str, "<")  == 0) { emit_loadbool(c, (uint8_t)target, ld <  rd); folded = true; }
                else if (strcmp(op_str, "<=") == 0) { emit_loadbool(c, (uint8_t)target, ld <= rd); folded = true; }
                else if (strcmp(op_str, ">")  == 0) { emit_loadbool(c, (uint8_t)target, ld >  rd); folded = true; }
                else if (strcmp(op_str, ">=") == 0) { emit_loadbool(c, (uint8_t)target, ld >= rd); folded = true; }
                else if (strcmp(op_str, "==") == 0) { emit_loadbool(c, (uint8_t)target, ld == rd); folded = true; }
                else if (strcmp(op_str, "!=") == 0) { emit_loadbool(c, (uint8_t)target, ld != rd); folded = true; }
                else valid = false;

                if (!folded && valid) {
                    int k = chunk_add_const(c->chunk, make_double(result));
                    emit_ABx(c, OP_LOADK, (uint8_t)target, (uint16_t)k);
                    folded = true;
                }
            }
            /* string + string */
            else if (l->kind == EXPR_STRING && r->kind == EXPR_STRING &&
                     strcmp(op_str, "+") == 0) {
                int len = l->data.string.length + r->data.string.length;
                char *buf = malloc(len + 1);
                memcpy(buf, l->data.string.value, l->data.string.length);
                memcpy(buf + l->data.string.length, r->data.string.value, r->data.string.length + 1);
                int k = chunk_add_string(c->vm, c->chunk, buf);
                emit_ABx(c, OP_LOADK, (uint8_t)target, (uint16_t)k);
                free(buf);
                folded = true;
            }
            /* null comparison */
            else if (l->kind == EXPR_NULL && r->kind == EXPR_NULL) {
                if (strcmp(op_str, "==") == 0) { emit_loadbool(c, (uint8_t)target, true); folded = true; }
                else if (strcmp(op_str, "!=") == 0) { emit_loadbool(c, (uint8_t)target, false); folded = true; }
            }
            /* int/float op null / null op int/float (never equal) */
            else if ((l->kind == EXPR_NULL && (r->kind == EXPR_INTEGER || r->kind == EXPR_FLOAT)) ||
                     (r->kind == EXPR_NULL && (l->kind == EXPR_INTEGER || l->kind == EXPR_FLOAT))) {
                if (strcmp(op_str, "==") == 0) { emit_loadbool(c, (uint8_t)target, false); folded = true; }
                else if (strcmp(op_str, "!=") == 0) { emit_loadbool(c, (uint8_t)target, true); folded = true; }
            }

            if (folded) break;
        }

        /* Not short-circuit: evaluate both sides eagerly */

        /* ADDI/SUBI fast path: x +/- small_literal  →  single imm instruction,
           no temp register needed. Range -128..127 fits in the C byte field. */
        if ((strcmp(op_str, "+") == 0 || strcmp(op_str, "-") == 0) &&
            expr->data.binary.right->kind == EXPR_INTEGER) {
            int64_t imm = compile_int_literal_i64(expr->data.binary.right->data.integer.value);
            if (imm >= -128 && imm <= 127) {
                /* ADDI/SUBI with separate dest: skip MOVE when left is a local in a different reg */
                if (expr->data.binary.left->kind == EXPR_IDENTIFIER) {
                    int idx;
                    VarKind kind = resolve_variable(c, expr->data.binary.left->data.identifier.name, &idx);
                    if (kind == VAR_LOCAL && idx != target) {
                        OpCode iop = (strcmp(op_str, "+") == 0) ? OP_ADDI : OP_SUBI;
                        emit_ABC(c, iop, (uint8_t)target, (uint8_t)idx, (uint8_t)(int8_t)imm);
                        break;
                    }
                }
                compile_expr_into(c, expr->data.binary.left, target);
                OpCode iop = (strcmp(op_str, "+") == 0) ? OP_ADDI : OP_SUBI;
                emit_ABC(c, iop, (uint8_t)target, (uint8_t)target, (uint8_t)(int8_t)imm);
                break;
            }
        }

        /* ADDK/MULK: literal right operand in constant pool, no LOADK needed */
        if ((strcmp(op_str, "+") == 0 || strcmp(op_str, "*") == 0) &&
            (expr->data.binary.right->kind == EXPR_INTEGER ||
             expr->data.binary.right->kind == EXPR_FLOAT)) {
            Value cv;
            if (expr->data.binary.right->kind == EXPR_INTEGER) {
                bigint_from_decimal(expr->data.binary.right->data.integer.value,
                                    strlen(expr->data.binary.right->data.integer.value), &cv);
            } else {
                cv = make_double(atof(expr->data.binary.right->data.float_lit.value));
            }
            int cidx = -1;
            for (int i = 0; i < c->chunk->const_count; i++) {
                if (c->chunk->constants[i] == cv) { cidx = i; break; }
            }
            if (cidx < 0) cidx = chunk_add_const(c->chunk, cv);
            if (cidx <= 255) {
                compile_expr_into(c, expr->data.binary.left, target);
                OpCode kop = (strcmp(op_str, "+") == 0) ? OP_ADDK : OP_MULK;
                emit_ABC(c, kop, (uint8_t)target, (uint8_t)target, (uint8_t)cidx);
                break;
            }
        }

        compile_expr_into(c, expr->data.binary.left, target);
        int temp = alloc_reg(c);
        compile_expr_into(c, expr->data.binary.right, temp);
        
        OpCode op;
        if (strcmp(op_str, "+") == 0) op = OP_ADD;
        else if (strcmp(op_str, "-") == 0) op = OP_SUB;
        else if (strcmp(op_str, "*") == 0) op = OP_MUL;
        else if (strcmp(op_str, "/") == 0) op = OP_DIV;
        else if (strcmp(op_str, "//") == 0) op = OP_IDIV;
        else if (strcmp(op_str, "%") == 0) op = OP_MOD;
        else if (strcmp(op_str, "**") == 0) op = OP_POW;
        else if (strcmp(op_str, "==") == 0) op = OP_EQ;
        else if (strcmp(op_str, "!=") == 0) op = OP_NE;
        else if (strcmp(op_str, "<") == 0) op = OP_LT;
        else if (strcmp(op_str, "<=") == 0) op = OP_LE;
        else if (strcmp(op_str, ">") == 0) op = OP_GT;
        else if (strcmp(op_str, ">=") == 0) op = OP_GE;
        else if (strcmp(op_str, "in") == 0) op = OP_IN;
        else if (strcmp(op_str, "not in") == 0) op = OP_IN;
        else if (strcmp(op_str, "is") == 0) op = OP_RAW_EQ;
        else if (strcmp(op_str, "is not") == 0) op = OP_RAW_NE;
        else if (strcmp(op_str, "&") == 0) op = OP_BAND;
        else if (strcmp(op_str, "|") == 0) op = OP_BOR;
        else if (strcmp(op_str, "^") == 0) op = OP_BXOR;
        else if (strcmp(op_str, "<<") == 0) op = OP_SHL;
        else if (strcmp(op_str, ">>") == 0) op = OP_SHR;
        else {
            op = OP_ADD; /* fallback */
        }
        
        emit_ABC(c, op, (uint8_t)target, (uint8_t)target, (uint8_t)temp);
        free_reg(c); /* temp */
        if (strcmp(op_str, "not in") == 0)
            emit_ABC(c, OP_NOT, (uint8_t)target, (uint8_t)target, 0);
        break;
    }

    case EXPR_UNARY: {
        const char *op_str = expr->data.unary.operator;
        Expr *operand = expr->data.unary.operand;

        /* Constant folding for unary minus / bitwise NOT on integers (exact) */
        if ((strcmp(op_str, "-") == 0 || strcmp(op_str, "~") == 0) &&
            operand->kind == EXPR_INTEGER) {
            Value v;
            bigint_from_decimal(operand->data.integer.value,
                                strlen(operand->data.integer.value), &v);
            Value res = bigint_unary_value(c->vm, op_str[0] == '-' ? VM_OP_NEG : VM_OP_BNOT, v);
            emit_int_const(c, (uint8_t)target, res);
            break;
        }
        /* Constant folding for unary minus on floats */
        if (strcmp(op_str, "-") == 0 && operand->kind == EXPR_FLOAT) {
            double v = -atof(operand->data.float_lit.value);
            int k = chunk_add_const(c->chunk, make_double(v));
            emit_ABx(c, OP_LOADK, (uint8_t)target, (uint16_t)k);
            break;
        }
        /* Constant folding for logical NOT on booleans */
        if (strcmp(op_str, "!") == 0 && operand->kind == EXPR_BOOL) {
            emit_loadbool(c, (uint8_t)target, !operand->data.boolean.value);
            break;
        }

        compile_expr_into(c, operand, target);
        if (strcmp(op_str, "-") == 0) {
            emit_ABC(c, OP_NEG, (uint8_t)target, (uint8_t)target, 0);
        } else if (strcmp(op_str, "!") == 0 || strcmp(op_str, "not") == 0) {
            emit_ABC(c, OP_NOT, (uint8_t)target, (uint8_t)target, 0);
        } else if (strcmp(op_str, "~") == 0) {
            emit_ABC(c, OP_BNOT, (uint8_t)target, (uint8_t)target, 0);
        }
        break;
    }

    case EXPR_CALL: {
        Expr *callee = expr->data.call.callee;
        int nargs = expr->data.call.arg_count;
        bool has_keywords = false;
        for (int i = 0; i < nargs; i++) {
            if (expr->data.call.arg_names && expr->data.call.arg_names[i]) {
                has_keywords = true;
                break;
            }
        }
        /* Detect method call: callee is field access */
        if (callee->kind == EXPR_FIELD_ACCESS) {
            Expr *obj = callee->data.field_access.obj;
            /* Detect super.method() / super().method() */
            bool is_super = false;
            if (obj->kind == EXPR_IDENTIFIER &&
                strcmp(obj->data.identifier.name, "super") == 0) {
                is_super = true;
            } else if (obj->kind == EXPR_CALL &&
                       obj->data.call.callee->kind == EXPR_IDENTIFIER &&
                       strcmp(obj->data.call.callee->data.identifier.name, "super") == 0 &&
                       obj->data.call.arg_count == 0) {
                is_super = true;
            }
            if (is_super) {
                int saved_base = c->temp_base;
                c->temp_base = target + 2 + nargs;
                for (int i = 0; i < nargs; i++) {
                    compile_expr_into(c, expr->data.call.arguments[i], target + 2 + i);
                }
                c->temp_base = saved_base;
                int mk = chunk_add_string(c->vm, c->chunk, callee->data.field_access.field);
                emit_ABx(c, OP_LOADK, (uint8_t)(target + 1), (uint16_t)mk);
                emit_ABC(c, OP_SUPER, (uint8_t)target, 0, (uint8_t)nargs);
            } else if (has_keywords) {
                /* Keyword method call: resolve to bound method, then
                   OP_KW_PREFIX + OP_CALL. */
                compile_expr_into(c, obj, target);
                int mk = chunk_add_string(c->vm, c->chunk, callee->data.field_access.field);
                if (mk <= 255) {
                    emit_ABC(c, OP_MEMBERGET, (uint8_t)target, (uint8_t)target, (uint8_t)mk);
                    int pos_count = 0;
                    for (int i = 0; i < nargs; i++) {
                        if (expr->data.call.arg_names[i] == NULL) pos_count = i + 1;
                    }
                    int kw_count = nargs - pos_count;
                    int saved_base = c->temp_base;
                    c->temp_base = target + 1 + nargs;
                    int compiled_pos = 0;
                    for (int i = 0; i < nargs; i++) {
                        if (expr->data.call.arg_names[i] == NULL) {
                            compile_expr_into(c, expr->data.call.arguments[i], target + 1 + compiled_pos);
                            compiled_pos++;
                        }
                    }
                    int kw_reg = target + 1 + pos_count;
                    for (int i = 0; i < nargs; i++) {
                        if (expr->data.call.arg_names[i] != NULL) {
                            compile_expr_into(c, expr->data.call.arguments[i], kw_reg);
                            kw_reg++;
                        }
                    }
                    int kw_idx = build_kw_names_const(c, expr, nargs);
                    c->temp_base = saved_base;
                    emit_ABx(c, OP_KW_PREFIX, (uint8_t)kw_count, (uint16_t)kw_idx);
                    emit_ABC(c, OP_CALL, (uint8_t)target, (uint8_t)target, (uint8_t)nargs);
                } else {
                    /* Method name constant index > 255 — rare edge case.
                     * Fall back to OP_INVOKE (ignores kwargs, positional only). */
                    int saved_base = c->temp_base;
                    c->temp_base = target + 2 + nargs;
                    for (int i = 0; i < nargs; i++) {
                        compile_expr_into(c, expr->data.call.arguments[i], target + 2 + i);
                    }
                    c->temp_base = saved_base;
                    emit_ABx(c, OP_LOADK, (uint8_t)(target + 1), (uint16_t)mk);
                    emit_ABC(c, OP_INVOKE, (uint8_t)target, (uint8_t)target, (uint8_t)nargs);
                }
            } else {
                compile_expr_into(c, obj, target);
                int saved_base = c->temp_base;
                /* method name at target+1, args at target+2.. */
                c->temp_base = target + 2 + nargs;
                for (int i = 0; i < nargs; i++) {
                    compile_expr_into(c, expr->data.call.arguments[i], target + 2 + i);
                }
                c->temp_base = saved_base;
                /* Load method name into target+1, then INVOKE */
                int mk = chunk_add_string(c->vm, c->chunk, callee->data.field_access.field);
                emit_ABx(c, OP_LOADK, (uint8_t)(target + 1), (uint16_t)mk);
                emit_ABC(c, OP_INVOKE, (uint8_t)target, (uint8_t)target, (uint8_t)nargs);
            }
        } else if (has_keywords) {
            /* Keyword call: compile callee, positional args, then keyword
               values, and emit OP_KW_PREFIX + OP_CALL (no kwargs dict). */
            compile_expr_into(c, callee, target);
            int pos_count = 0;
            for (int i = 0; i < nargs; i++) {
                if (expr->data.call.arg_names[i] == NULL) pos_count = i + 1;
            }
            int kw_count = nargs - pos_count;
            int saved_base = c->temp_base;
            c->temp_base = target + 1 + nargs;
            /* Compile positional args into target+1..target+pos_count */
            int compiled_pos = 0;
            for (int i = 0; i < nargs; i++) {
                if (expr->data.call.arg_names[i] == NULL) {
                    compile_expr_into(c, expr->data.call.arguments[i], target + 1 + compiled_pos);
                    compiled_pos++;
                }
            }
            /* Compile keyword values (declaration order) into target+pos_count+1.. */
            int kw_reg = target + 1 + pos_count;
            for (int i = 0; i < nargs; i++) {
                if (expr->data.call.arg_names[i] != NULL) {
                    compile_expr_into(c, expr->data.call.arguments[i], kw_reg);
                    kw_reg++;
                }
            }
            int kw_idx = build_kw_names_const(c, expr, nargs);
            c->temp_base = saved_base;
            emit_ABx(c, OP_KW_PREFIX, (uint8_t)kw_count, (uint16_t)kw_idx);
            emit_ABC(c, OP_CALL, (uint8_t)target, (uint8_t)target, (uint8_t)nargs);
        } else {
            compile_expr_into(c, callee, target);
            int saved_base = c->temp_base;
            c->temp_base = target + 1 + nargs;
            for (int i = 0; i < nargs; i++) {
                compile_expr_into(c, expr->data.call.arguments[i], target + 1 + i);
            }
            c->temp_base = saved_base;
            emit_ABC(c, OP_CALL, (uint8_t)target, (uint8_t)target, (uint8_t)nargs);
        }
        break;
    }

    case EXPR_FIELD_ACCESS: {
        /* Try slot-based access for self.field in class methods */
        int slot = -1;
        if (!expr->data.field_access.optional &&
            expr->data.field_access.obj->kind == EXPR_IDENTIFIER) {
            const char *obj_name = expr->data.field_access.obj->data.identifier.name;
            int self_idx;
            if (resolve_variable(c, obj_name, &self_idx) == VAR_LOCAL && self_idx == 0) {
                slot = resolve_field_slot(c, expr->data.field_access.field);
            }
        }
        if (slot >= 0 && slot <= 255) {
            compile_expr_into(c, expr->data.field_access.obj, target);
            emit_ABC(c, OP_GETFIELD, (uint8_t)target, (uint8_t)target, (uint8_t)slot);
        } else {
            compile_expr_into(c, expr->data.field_access.obj, target);
            int fk = chunk_add_string(c->vm, c->chunk, expr->data.field_access.field);
            OpCode get_op = expr->data.field_access.optional ? OP_MEMBERGET_SAFE : OP_MEMBERGET;
            if (fk > 255) {
                int temp = alloc_reg(c);
                emit_ABx(c, OP_LOADK, (uint8_t)temp, (uint16_t)fk);
                emit_ABC(c, expr->data.field_access.optional ? OP_INDEXGET_SAFE : OP_INDEXGET,
                         (uint8_t)target, (uint8_t)target, (uint8_t)temp);
                free_reg(c);
            } else {
                emit_ABC(c, get_op, (uint8_t)target, (uint8_t)target, (uint8_t)fk);
            }
        }
        break;
    }

    case EXPR_INDEX_ACCESS: {
        compile_expr_into(c, expr->data.index_access.obj, target);
        int temp = alloc_reg(c);
        compile_expr_into(c, expr->data.index_access.index, temp);
        OpCode get_op = expr->data.index_access.optional ? OP_INDEXGET_SAFE : OP_INDEXGET;
        emit_ABC(c, get_op, (uint8_t)target, (uint8_t)target, (uint8_t)temp);
        free_reg(c);
        break;
    }

    case EXPR_SLICE: {
        /* OP_SLICE (kept: Python-style slicing) routes through the frontend
         * slice hook; a null/non-indexable obj yields null, so the optional
         * (?.) form needs no separate opcode. */
        compile_expr_into(c, expr->data.slice.obj, target);
        int saved_base = c->temp_base;
        c->temp_base = target + 1;   /* runtime reads object + bounds from RB..RB+3 */
        int start_reg = alloc_reg(c);
        int stop_reg  = alloc_reg(c);
        int step_reg  = alloc_reg(c);
        if (expr->data.slice.start)
            compile_expr_into(c, expr->data.slice.start, start_reg);
        else
            emit_ABC(c, OP_LOADNULL, (uint8_t)start_reg, 0, 0);
        if (expr->data.slice.stop)
            compile_expr_into(c, expr->data.slice.stop, stop_reg);
        else
            emit_ABC(c, OP_LOADNULL, (uint8_t)stop_reg, 0, 0);
        if (expr->data.slice.step)
            compile_expr_into(c, expr->data.slice.step, step_reg);
        else
            emit_ABC(c, OP_LOADNULL, (uint8_t)step_reg, 0, 0);
        emit_ABC(c, OP_SLICE, (uint8_t)target, (uint8_t)target, 0);
        free_reg(c);
        free_reg(c);
        free_reg(c);
        c->temp_base = saved_base;
        break;
    }

    case EXPR_ASSIGNMENT: {
        Expr *lhs = expr->data.assignment.target;
        Expr *rhs = expr->data.assignment.value;
        if (lhs->kind == EXPR_IDENTIFIER) {
            const char *name = lhs->data.identifier.name;
            int idx;
            VarKind kind = resolve_variable(c, name, &idx);
            if (kind == VAR_LOCAL) {
                compile_expr_into(c, rhs, idx);
                if (idx != target) emit_move(c, (uint8_t)target, (uint8_t)idx);
            } else if (kind == VAR_UPVALUE) {
                compile_expr_into(c, rhs, target);
                emit_ABx(c, OP_SETUPVAL, (uint8_t)target, (uint16_t)idx);
            } else {
                compile_expr_into(c, rhs, target);
                int k = chunk_add_string(c->vm, c->chunk, name);
                emit_ABx(c, OP_SETGLOBAL, (uint8_t)target, (uint16_t)k);
            }
        } else if (lhs->kind == EXPR_FIELD_ACCESS) {
            int obj = alloc_reg(c);
            compile_expr_into(c, lhs->data.field_access.obj, obj);
            compile_expr_into(c, rhs, target);
            int slot = -1;
            if (lhs->data.field_access.obj->kind == EXPR_IDENTIFIER) {
                int self_idx;
                if (resolve_variable(c, lhs->data.field_access.obj->data.identifier.name, &self_idx) == VAR_LOCAL && self_idx == 0) {
                    slot = resolve_field_slot(c, lhs->data.field_access.field);
                }
            }
            if (slot >= 0 && slot <= 255) {
                emit_ABC(c, OP_SETFIELD, (uint8_t)obj, (uint8_t)target, (uint8_t)slot);
            } else {
                int fk = chunk_add_string(c->vm, c->chunk, lhs->data.field_access.field);
                if (fk > 255) {
                    int temp = alloc_reg(c);
                    emit_ABx(c, OP_LOADK, (uint8_t)temp, (uint16_t)fk);
                    emit_ABC(c, OP_MEMBERSET, (uint8_t)obj, (uint8_t)target, (uint8_t)temp);
                    free_reg(c);
                } else {
                    emit_ABC(c, OP_MEMBERSET, (uint8_t)obj, (uint8_t)target, (uint8_t)fk);
                }
            }
            free_reg(c); /* obj */
        } else if (lhs->kind == EXPR_INDEX_ACCESS) {
            int obj = alloc_reg(c);
            int idx = alloc_reg(c);
            compile_expr_into(c, lhs->data.index_access.obj, obj);
            compile_expr_into(c, lhs->data.index_access.index, idx);
            compile_expr_into(c, rhs, target);
            emit_ABC(c, OP_INDEXSET, (uint8_t)obj, (uint8_t)idx, (uint8_t)target);
            free_reg(c); /* idx */
            free_reg(c); /* obj */
        } else if (lhs->kind == EXPR_LIST_LITERAL) {
            /* Expression-level list destructuring: [a, b] = rhs
             * Variables must already exist — resolve, not add_local. */
            int src = alloc_reg(c);
            compile_expr_into(c, rhs, src);
            for (int i = 0; i < lhs->data.list_literal.element_count; i++) {
                Expr *e = lhs->data.list_literal.elements[i];
                if (e->kind != EXPR_IDENTIFIER) continue;
                const char *vname = e->data.identifier.name;
                if (strcmp(vname, "_") == 0) continue;
                int idx_reg = alloc_reg(c);
                int val_reg = alloc_reg(c);
                emit_loadi(c, (uint8_t)idx_reg, i);
                emit_ABC(c, OP_INDEXGET, (uint8_t)val_reg, (uint8_t)src, (uint8_t)idx_reg);
                int var_idx;
                VarKind vkind = resolve_variable(c, vname, &var_idx);
                if (vkind == VAR_LOCAL) {
                    emit_move(c, (uint8_t)var_idx, (uint8_t)val_reg);
                } else if (vkind == VAR_UPVALUE) {
                    emit_ABx(c, OP_SETUPVAL, (uint8_t)val_reg, (uint16_t)var_idx);
                } else {
                    int k = chunk_add_string(c->vm, c->chunk, vname);
                    emit_ABx(c, OP_SETGLOBAL, (uint8_t)val_reg, (uint16_t)k);
                }
                free_reg(c); /* val_reg */
                free_reg(c); /* idx_reg */
            }
            free_reg(c); /* src */
            emit_loadnull(c, (uint8_t)target);
        } else if (lhs->kind == EXPR_DICT_LITERAL) {
            /* Expression-level dict destructuring: {"key": var} = rhs */
            int src = alloc_reg(c);
            compile_expr_into(c, rhs, src);
            for (int i = 0; i < lhs->data.dict_literal.entry_count; i++) {
                Expr *key_expr = lhs->data.dict_literal.entries[i].key;
                Expr *val_expr = lhs->data.dict_literal.entries[i].value;
                if (val_expr->kind != EXPR_IDENTIFIER) continue;
                const char *vname = val_expr->data.identifier.name;
                if (strcmp(vname, "_") == 0) continue;
                int key_reg = alloc_reg(c);
                int val_reg = alloc_reg(c);
                compile_expr_into(c, key_expr, key_reg);
                emit_ABC(c, OP_INDEXGET, (uint8_t)val_reg, (uint8_t)src, (uint8_t)key_reg);
                int var_idx;
                VarKind vkind = resolve_variable(c, vname, &var_idx);
                if (vkind == VAR_LOCAL) {
                    emit_move(c, (uint8_t)var_idx, (uint8_t)val_reg);
                } else if (vkind == VAR_UPVALUE) {
                    emit_ABx(c, OP_SETUPVAL, (uint8_t)val_reg, (uint16_t)var_idx);
                } else {
                    int k = chunk_add_string(c->vm, c->chunk, vname);
                    emit_ABx(c, OP_SETGLOBAL, (uint8_t)val_reg, (uint16_t)k);
                }
                free_reg(c); /* val_reg */
                free_reg(c); /* key_reg */
            }
            free_reg(c); /* src */
            emit_loadnull(c, (uint8_t)target);
        } else {
            compile_expr_into(c, rhs, target);
        }
        break;
    }

    case EXPR_COMPOUND_ASSIGN: {
        Expr *lhs = expr->data.compound_assign.target;
        Expr *rhs = expr->data.compound_assign.value;
        const char *op_str = expr->data.compound_assign.operator;
        OpCode op = OP_ADD;
        if (strcmp(op_str, "+=") == 0) op = OP_ADD;
        else if (strcmp(op_str, "-=") == 0) op = OP_SUB;
        else if (strcmp(op_str, "*=") == 0) op = OP_MUL;
        else if (strcmp(op_str, "**=") == 0) op = OP_POW;
        else if (strcmp(op_str, "/=") == 0) op = OP_DIV;
        else if (strcmp(op_str, "//=") == 0) op = OP_IDIV;
        else if (strcmp(op_str, "%=") == 0) op = OP_MOD;
        else if (strcmp(op_str, "&=") == 0) op = OP_BAND;
        else if (strcmp(op_str, "|=") == 0) op = OP_BOR;
        else if (strcmp(op_str, "^=") == 0) op = OP_BXOR;

        if (lhs->kind == EXPR_IDENTIFIER) {
            /* simple identifiers can be re-evaluated cleanly */
            Expr fake_rhs = *rhs;
            /* Rebuild the binary operator by stripping the trailing '=':
               "+=" -> "+", "//=" -> "//", "**=" -> "**" */
            char bin_op[3] = {0};
            int oplen = (int)strlen(op_str) - 1;
            if (oplen > 2) oplen = 2;
            memcpy(bin_op, op_str, (size_t)oplen);
            Expr fake_bin = {
                .kind = EXPR_BINARY,
                .data.binary = { .left = lhs, .operator = bin_op, .right = &fake_rhs }
            };
            Expr fake_assign = {
                .kind = EXPR_ASSIGNMENT,
                .data.assignment = { .target = lhs, .value = &fake_bin }
            };
            compile_expr_into(c, &fake_assign, target);
        } else if (lhs->kind == EXPR_FIELD_ACCESS) {
            int obj = alloc_reg(c);
            compile_expr_into(c, lhs->data.field_access.obj, obj);
            
            int slot = -1;
            if (lhs->data.field_access.obj->kind == EXPR_IDENTIFIER) {
                int self_idx;
                if (resolve_variable(c, lhs->data.field_access.obj->data.identifier.name, &self_idx) == VAR_LOCAL && self_idx == 0) {
                    slot = resolve_field_slot(c, lhs->data.field_access.field);
                }
            }
            int temp_val = alloc_reg(c);
            
            if (slot >= 0 && slot <= 255) {
                emit_ABC(c, OP_GETFIELD, (uint8_t)temp_val, (uint8_t)obj, (uint8_t)slot);
                int temp_rhs = alloc_reg(c);
                compile_expr_into(c, rhs, temp_rhs);
                emit_ABC(c, op, (uint8_t)temp_val, (uint8_t)temp_val, (uint8_t)temp_rhs);
                emit_ABC(c, OP_SETFIELD, (uint8_t)obj, (uint8_t)temp_val, (uint8_t)slot);
                free_reg(c);
            } else {
                int fk = chunk_add_string(c->vm, c->chunk, lhs->data.field_access.field);
                if (fk > 255) {
                    int temp_k = alloc_reg(c);
                    emit_ABx(c, OP_LOADK, (uint8_t)temp_k, (uint16_t)fk);
                    emit_ABC(c, OP_INDEXGET, (uint8_t)temp_val, (uint8_t)obj, (uint8_t)temp_k);
                    int temp_rhs = alloc_reg(c);
                    compile_expr_into(c, rhs, temp_rhs);
                    emit_ABC(c, op, (uint8_t)temp_val, (uint8_t)temp_val, (uint8_t)temp_rhs);
                    emit_ABC(c, OP_INDEXSET, (uint8_t)obj, (uint8_t)temp_k, (uint8_t)temp_val);
                    free_reg(c);
                    free_reg(c);
                } else {
                    emit_ABC(c, OP_MEMBERGET, (uint8_t)temp_val, (uint8_t)obj, (uint8_t)fk);
                    int temp_rhs = alloc_reg(c);
                    compile_expr_into(c, rhs, temp_rhs);
                    emit_ABC(c, op, (uint8_t)temp_val, (uint8_t)temp_val, (uint8_t)temp_rhs);
                    emit_ABC(c, OP_MEMBERSET, (uint8_t)obj, (uint8_t)temp_val, (uint8_t)fk);
                    free_reg(c);
                }
            }
            if (target != temp_val) emit_move(c, (uint8_t)target, (uint8_t)temp_val);
            free_reg(c);
            free_reg(c);
        } else if (lhs->kind == EXPR_INDEX_ACCESS) {
            int obj = alloc_reg(c);
            int idx = alloc_reg(c);
            compile_expr_into(c, lhs->data.index_access.obj, obj);
            compile_expr_into(c, lhs->data.index_access.index, idx);
            
            int temp_val = alloc_reg(c);
            emit_ABC(c, OP_INDEXGET, (uint8_t)temp_val, (uint8_t)obj, (uint8_t)idx);
            
            int temp_rhs = alloc_reg(c);
            compile_expr_into(c, rhs, temp_rhs);
            emit_ABC(c, op, (uint8_t)temp_val, (uint8_t)temp_val, (uint8_t)temp_rhs);
            
            emit_ABC(c, OP_INDEXSET, (uint8_t)obj, (uint8_t)idx, (uint8_t)temp_val);
            if (target != temp_val) emit_move(c, (uint8_t)target, (uint8_t)temp_val);
            
            free_reg(c);
            free_reg(c);
            free_reg(c);
            free_reg(c);
        } else {
            compile_expr_into(c, rhs, target);
        }
        break;
    }

    case EXPR_TERNARY: {
        compile_expr_into(c, expr->data.ternary.condition, target);
        int jz = emit_jump(c, OP_JZ, (uint8_t)target);
        compile_expr_into(c, expr->data.ternary.then_expr, target);
        int j1 = emit_jump(c, OP_JMP, 0);
        patch_jump(c, jz);
        compile_expr_into(c, expr->data.ternary.else_expr, target);
        patch_jump(c, j1);
        break;
    }

    case EXPR_LIST_LITERAL: {
        emit_ABx(c, OP_NEWLIST, (uint8_t)target, expr->data.list_literal.element_count);
        for (int i = 0; i < expr->data.list_literal.element_count; i++) {
            int temp = alloc_reg(c);
            compile_expr_into(c, expr->data.list_literal.elements[i], temp);
            emit_ABC(c, OP_LISTAPPEND, (uint8_t)target, (uint8_t)temp, 0);
            free_reg(c);
        }
        break;
    }

    case EXPR_LIST_COMPREHENSION: {
        emit_ABC(c, OP_NEWLIST, (uint8_t)target, 0, 0);

        int iter = target + 4;
        int len  = target + 5;
        int i    = target + 6;
        int var  = target + 7;
        int cond = target + 8;

        int saved_base = c->temp_base;
        c->temp_base = target + 9;

        compile_expr_into(c, expr->data.list_comprehension.iterable, iter);

        int lk = chunk_add_string(c->vm, c->chunk, "length");
        if (lk > 255) {
            int temp = alloc_reg(c);
            emit_ABx(c, OP_LOADK, (uint8_t)temp, (uint16_t)lk);
            emit_ABC(c, OP_INDEXGET, (uint8_t)len, (uint8_t)iter, (uint8_t)temp);
            free_reg(c);
        } else {
            emit_ABC(c, OP_MEMBERGET, (uint8_t)len, (uint8_t)iter, (uint8_t)lk);
        }

        emit_loadi(c, (uint8_t)i, 0);

        scope_enter(c);
        add_local(c, expr->data.list_comprehension.variable, var);

        int loop_start = c->chunk->count;

        emit_ABC(c, OP_LT, (uint8_t)cond, (uint8_t)i, (uint8_t)len);
        int end_jump = emit_jump(c, OP_JZ, (uint8_t)cond);

        emit_ABC(c, OP_INDEXGET, (uint8_t)var, (uint8_t)iter, (uint8_t)i);

        int skip_jump = -1;
        if (expr->data.list_comprehension.condition) {
            compile_expr_into(c, expr->data.list_comprehension.condition, cond);
            skip_jump = emit_jump(c, OP_JZ, (uint8_t)cond);
        }

        int temp = alloc_reg(c);
        compile_expr_into(c, expr->data.list_comprehension.element, temp);
        emit_ABC(c, OP_LISTAPPEND, (uint8_t)target, (uint8_t)temp, 0);
        free_reg(c);

        if (skip_jump >= 0) patch_jump(c, skip_jump);

        int one = target + 9;
        emit_loadi(c, (uint8_t)one, 1);
        emit_ABC(c, OP_ADD, (uint8_t)i, (uint8_t)i, (uint8_t)one);

        int sbx = loop_start - (c->chunk->count + 1);
        emit_AsBx(c, OP_JMP, 0, sbx);

        patch_jump(c, end_jump);

        scope_exit(c);
        c->temp_base = saved_base;
        break;
    }

    case EXPR_DICT_LITERAL: {
        emit_ABC(c, OP_NEWDICT, (uint8_t)target, 0, 0);
        for (int i = 0; i < expr->data.dict_literal.entry_count; i++) {
            int k = alloc_reg(c);
            int v = alloc_reg(c);
            compile_expr_into(c, expr->data.dict_literal.entries[i].key, k);
            compile_expr_into(c, expr->data.dict_literal.entries[i].value, v);
            emit_ABC(c, OP_INDEXSET, (uint8_t)target, (uint8_t)k, (uint8_t)v);
            free_reg(c);
            free_reg(c);
        }
        break;
    }

    case EXPR_NEW: {
        int ck = chunk_add_string(c->vm, c->chunk, expr->data.new_expr.class_name);
        emit_ABx(c, OP_NEW, (uint8_t)target, (uint16_t)ck);
        int nargs = expr->data.new_expr.arg_count;
        int saved_base = c->temp_base;
        c->temp_base = target + nargs + 2;
        for (int i = 0; i < nargs; i++) {
            compile_expr_into(c, expr->data.new_expr.arguments[i], target + 2 + i);
        }
        c->temp_base = saved_base;
        /*
         * Always emit OP_INVOKE("__init__") — don't peek at runtime state.
         * OP_INVOKE already handles missing __init__ (returns nil, see
         * py_invoke "__init__" branch).
         *
         * Register layout: r_target=instance, r_target+1="__init__",
         * r_target+2..=args, r_target+nargs+1=scratch (nil return lands here,
         * NOT on the instance).
         */
        int mk = chunk_add_string(c->vm, c->chunk, "__init__");
        emit_ABx(c, OP_LOADK, (uint8_t)(target + 1), (uint16_t)mk);
        emit_ABC(c, OP_INVOKE, (uint8_t)(target + nargs + 1), (uint8_t)target, (uint8_t)nargs);
        break;
    }

    case EXPR_FUNCTION: {
        int fn_const = compile_function_value(c, expr->data.function.name,
                                               expr->data.function.params,
                                               expr->data.function.param_count,
                                               expr->data.function.body,
                                               expr->data.function.body_count);
        emit_ABx(c, OP_CLOSURE, (uint8_t)target, (uint16_t)fn_const);
        break;
    }

    case EXPR_MULTI_ASSIGN: {
        int n = expr->data.multi_assign.target_count;
        int m = expr->data.multi_assign.value_count;

        /* Optimisation: detect a, b = b, a where both are local variables */
        if (n == 2 && m == 2) {
            Expr *t0 = expr->data.multi_assign.targets[0];
            Expr *t1 = expr->data.multi_assign.targets[1];
            Expr *v0 = expr->data.multi_assign.values[0];
            Expr *v1 = expr->data.multi_assign.values[1];
            if (t0->kind == EXPR_IDENTIFIER && t1->kind == EXPR_IDENTIFIER &&
                v0->kind == EXPR_IDENTIFIER && v1->kind == EXPR_IDENTIFIER) {
                int idx_a, idx_b;
                VarKind kind_a = resolve_variable(c, t0->data.identifier.name, &idx_a);
                VarKind kind_b = resolve_variable(c, t1->data.identifier.name, &idx_b);
                if (kind_a == VAR_LOCAL && kind_b == VAR_LOCAL &&
                    strcmp(t0->data.identifier.name, v1->data.identifier.name) == 0 &&
                    strcmp(t1->data.identifier.name, v0->data.identifier.name) == 0) {
                    emit_ABC(c, OP_SWAP, (uint8_t)idx_a, (uint8_t)idx_b, 0);
                    emit_loadnull(c, (uint8_t)target);
                    break;
                }
            }
        }

        /* General case: evaluate all RHS values into temps first,
         * then assign each temp to its target.
         * Simple literals (int, float, string, bool, null, char) skip the
         * temp and compile directly into the target register. */
        int stack_temps[16];
        bool stack_direct[16] = {0};
        int *temps = (m <= 16) ? stack_temps : (int *)malloc(m * sizeof(int));
        bool *direct = (m <= 16) ? stack_direct : (bool *)calloc(m, sizeof(bool));
        for (int i = 0; i < m; i++) {
            ExprKind vk = expr->data.multi_assign.values[i]->kind;
            bool is_literal = (vk == EXPR_INTEGER || vk == EXPR_FLOAT ||
                               vk == EXPR_STRING || vk == EXPR_BOOL ||
                               vk == EXPR_NULL);
            if (is_literal) {
                direct[i] = true;
                temps[i] = -1;
            } else {
                temps[i] = alloc_reg(c);
                compile_expr_into(c, expr->data.multi_assign.values[i], temps[i]);
            }
        }
        for (int i = 0; i < n && i < m; i++) {
            if (direct[i]) {
                /* Resolve target register first so compile_expr_into writes there */
                Expr *lhs = expr->data.multi_assign.targets[i];
                if (lhs->kind == EXPR_IDENTIFIER) {
                    int idx;
                    VarKind kind = resolve_variable(c, lhs->data.identifier.name, &idx);
                    if (kind == VAR_LOCAL) {
                        compile_expr_into(c, expr->data.multi_assign.values[i], idx);
                    } else if (kind == VAR_UPVALUE) {
                        int t = alloc_reg(c);
                        compile_expr_into(c, expr->data.multi_assign.values[i], t);
                        emit_ABx(c, OP_SETUPVAL, (uint8_t)t, (uint16_t)idx);
                        free_reg(c);
                    } else {
                        int t = alloc_reg(c);
                        compile_expr_into(c, expr->data.multi_assign.values[i], t);
                        int k = chunk_add_string(c->vm, c->chunk, lhs->data.identifier.name);
                        emit_ABx(c, OP_SETGLOBAL, (uint8_t)t, (uint16_t)k);
                        free_reg(c);
                    }
                } else if (lhs->kind == EXPR_FIELD_ACCESS || lhs->kind == EXPR_INDEX_ACCESS) {
                    int t = alloc_reg(c);
                    compile_expr_into(c, expr->data.multi_assign.values[i], t);
                    compile_single_assignment(c, lhs, t);
                    free_reg(c);
                }
            } else {
                compile_single_assignment(c, expr->data.multi_assign.targets[i], temps[i]);
            }
        }
        for (int i = 0; i < m; i++) {
            if (!direct[i]) free_reg(c);
        }
        if (m > 16) {
            free(temps);
            free(direct);
        }
        emit_loadnull(c, (uint8_t)target);
        break;
    }

    default:
        emit_loadnull(c, (uint8_t)target);
        break;
    }
}

/* ============================================================ */
/* Statements                                                   */
/* ============================================================ */

static void compile_stmt(Compiler *c, Stmt *stmt) {
    c->line = stmt->line;
    switch (stmt->kind) {

    case STMT_EXPRESSION: {
        int r = alloc_reg(c);
        compile_expr_into(c, stmt->data.expression.expression, r);
        free_reg(c);
        break;
    }

    case STMT_DECLARATION: {
        compile_decl(c, stmt->data.declaration.decl);
        break;
    }

    case STMT_VAR_DECL: {
        if (stmt->data.var_decl.pattern) {
            Expr *pattern = stmt->data.var_decl.pattern;
            int src = alloc_reg(c);
            if (stmt->data.var_decl.initializer) {
                compile_expr_into(c, stmt->data.var_decl.initializer, src);
            } else {
                emit_loadnull(c, (uint8_t)src);
            }
            if (pattern->kind == EXPR_LIST_LITERAL) {
                for (int i = 0; i < pattern->data.list_literal.element_count; i++) {
                    Expr *e = pattern->data.list_literal.elements[i];
                    if (e->kind != EXPR_IDENTIFIER) continue;
                    const char *var_name = e->data.identifier.name;
                    if (strcmp(var_name, "_") == 0) continue;
                    if (c->func_depth > 0) {
                        int dst = alloc_reg(c);
                        add_local(c, var_name, dst);
                        int idx_reg = alloc_reg(c);
                        emit_loadi(c, (uint8_t)idx_reg, i);
                        emit_ABC(c, OP_INDEXGET, (uint8_t)dst, (uint8_t)src, (uint8_t)idx_reg);
                        free_reg(c);
                    } else {
                        int dst = alloc_reg(c);
                        int idx_reg = alloc_reg(c);
                        emit_loadi(c, (uint8_t)idx_reg, i);
                        emit_ABC(c, OP_INDEXGET, (uint8_t)dst, (uint8_t)src, (uint8_t)idx_reg);
                        int k = chunk_add_string(c->vm, c->chunk, var_name);
                        emit_ABx(c, OP_SETGLOBAL, (uint8_t)dst, (uint16_t)k);
                        free_reg(c);
                        free_reg(c);
                    }
                }
            } else if (pattern->kind == EXPR_DICT_LITERAL) {
                for (int i = 0; i < pattern->data.dict_literal.entry_count; i++) {
                    Expr *key_expr = pattern->data.dict_literal.entries[i].key;
                    Expr *val_expr = pattern->data.dict_literal.entries[i].value;
                    if (val_expr->kind != EXPR_IDENTIFIER) continue;
                    const char *var_name = val_expr->data.identifier.name;
                    if (strcmp(var_name, "_") == 0) continue;
                    if (c->func_depth > 0) {
                        int dst = alloc_reg(c);
                        add_local(c, var_name, dst);
                        int key_reg = alloc_reg(c);
                        compile_expr_into(c, key_expr, key_reg);
                        emit_ABC(c, OP_INDEXGET, (uint8_t)dst, (uint8_t)src, (uint8_t)key_reg);
                        free_reg(c);
                    } else {
                        int dst = alloc_reg(c);
                        int key_reg = alloc_reg(c);
                        compile_expr_into(c, key_expr, key_reg);
                        emit_ABC(c, OP_INDEXGET, (uint8_t)dst, (uint8_t)src, (uint8_t)key_reg);
                        int k = chunk_add_string(c->vm, c->chunk, var_name);
                        emit_ABx(c, OP_SETGLOBAL, (uint8_t)dst, (uint16_t)k);
                        free_reg(c);
                        free_reg(c);
                    }
                }
            }
            free_reg(c);
        } else {
            const char *name = stmt->data.var_decl.name;
            if (c->func_depth > 0) {
                int r = alloc_reg(c);
                add_local(c, name, r);
                if (stmt->data.var_decl.initializer) {
                    compile_expr_into(c, stmt->data.var_decl.initializer, r);
                } else {
                    emit_loadnull(c, (uint8_t)r);
                }
            } else {
                int r = alloc_reg(c);
                if (stmt->data.var_decl.initializer) {
                    compile_expr_into(c, stmt->data.var_decl.initializer, r);
                } else {
                    emit_loadnull(c, (uint8_t)r);
                }
                int k = chunk_add_string(c->vm, c->chunk, name);
                emit_ABx(c, OP_SETGLOBAL, (uint8_t)r, (uint16_t)k);
                free_reg(c);
            }
        }
        break;
    }

    case STMT_RETURN: {
        int r = alloc_reg(c);
        if (stmt->data.return_stmt.value) {
            compile_expr_into(c, stmt->data.return_stmt.value, r);
        } else {
            emit_loadnull(c, (uint8_t)r);
        }
        emit_finally_blocks_for_early_exit(c, NULL);
        emit_ret(c, (uint8_t)r);
        free_reg(c);
        break;
    }

    case STMT_PASS:
        break;

    case STMT_BREAK: {
        if (!c->loop) {
            fprintf(stderr, "compiler: break outside loop\n");
            break;
        }
        emit_finally_blocks_for_early_exit(c, c->loop);
        int j = emit_jump(c, OP_JMP, 0);
        loop_record_break(c, j);
        break;
    }

    case STMT_CONTINUE: {
        if (!c->loop) {
            fprintf(stderr, "compiler: continue outside loop\n");
            break;
        }
        emit_finally_blocks_for_early_exit(c, c->loop);
        int j = emit_jump(c, OP_JMP, 0);
        loop_record_continue(c, j);
        break;
    }

    case STMT_IF: {
        Expr *cond = stmt->data.if_stmt.condition;
        int jz = -1;
        bool fused = false;

        if (cond->kind == EXPR_BINARY) {
            const char *op_str = cond->data.binary.operator;
            OpCode cmp_op = -1;
            if (strcmp(op_str, "<") == 0) cmp_op = OP_LT_JZ;
            else if (strcmp(op_str, "<=") == 0) cmp_op = OP_LE_JZ;
            else if (strcmp(op_str, ">") == 0) cmp_op = OP_GT_JZ;
            else if (strcmp(op_str, ">=") == 0) cmp_op = OP_GE_JZ;
            else if (strcmp(op_str, "==") == 0) cmp_op = OP_EQ_JZ;
            else if (strcmp(op_str, "!=") == 0) cmp_op = OP_NE_JZ;

            if (cmp_op != (OpCode)-1) {
                Expr *l = cond->data.binary.left;
                Expr *r = cond->data.binary.right;
                if (r->kind == EXPR_INTEGER) {
                    int64_t imm = compile_int_literal_i64(r->data.integer.value);
                    if (imm >= -128 && imm <= 127) {
                        int reg = alloc_reg(c);
                        compile_expr_into(c, l, reg);
                        jz = chunk_emit_ABC(c->chunk, c->line, cmp_to_imm_direct[cmp_op], (uint8_t)reg, (uint8_t)(int8_t)imm, 0);
                        free_reg(c);
                        fused = true;
                    }
                } else if (l->kind == EXPR_INTEGER) {
                    int64_t imm = compile_int_literal_i64(l->data.integer.value);
                    if (imm >= -128 && imm <= 127) {
                        int reg = alloc_reg(c);
                        compile_expr_into(c, r, reg);
                        jz = chunk_emit_ABC(c->chunk, c->line, cmp_to_imm_swapped[cmp_op], (uint8_t)reg, (uint8_t)(int8_t)imm, 0);
                        free_reg(c);
                        fused = true;
                    }
                }
                if (!fused) {
                    int right = alloc_reg(c);
                    compile_expr_into(c, r, right);
                    int left = alloc_reg(c);
                    compile_expr_into(c, l, left);
                    jz = chunk_emit_ABC(c->chunk, c->line, cmp_op, (uint8_t)left, (uint8_t)right, 0);
                    free_reg(c);
                    free_reg(c);
                    fused = true;
                }
            }
        }

        if (!fused) {
            int condreg = alloc_reg(c);
            compile_expr_into(c, cond, condreg);
            jz = emit_jump(c, OP_JZ, (uint8_t)condreg);
            free_reg(c);
        }

        scope_enter(c);
        for (int i = 0; i < stmt->data.if_stmt.then_count; i++)
            compile_stmt(c, stmt->data.if_stmt.then_body[i]);
        scope_exit(c);

        if (stmt->data.if_stmt.else_count > 0) {
            int j = emit_jump(c, OP_JMP, 0);
            if (fused) {
                int off = c->chunk->count - (jz + 1);
                if (off >= 0 && off <= 255)
                    c->chunk->code[jz] = (c->chunk->code[jz] & 0x00FFFFFF) | ((uint32_t)off << 24);
            } else {
                patch_jump(c, jz);
            }
            scope_enter(c);
            for (int i = 0; i < stmt->data.if_stmt.else_count; i++)
                compile_stmt(c, stmt->data.if_stmt.else_body[i]);
            scope_exit(c);
            patch_jump(c, j);
        } else {
            if (fused) {
                int off = c->chunk->count - (jz + 1);
                if (off >= 0 && off <= 255)
                    c->chunk->code[jz] = (c->chunk->code[jz] & 0x00FFFFFF) | ((uint32_t)off << 24);
            } else {
                patch_jump(c, jz);
            }
        }
        break;
    }

    case STMT_WHILE: {
        int loop_start = c->chunk->count;
        loop_push(c, loop_start);
        c->loop->continue_ip = loop_start;

        Expr *cond = stmt->data.while_stmt.condition;
        int jz = -1;
        bool fused = false;

        if (cond->kind == EXPR_BINARY) {
            const char *op_str = cond->data.binary.operator;
            OpCode cmp_op = -1;
            if (strcmp(op_str, "<") == 0) cmp_op = OP_LT_JZ;
            else if (strcmp(op_str, "<=") == 0) cmp_op = OP_LE_JZ;
            else if (strcmp(op_str, ">") == 0) cmp_op = OP_GT_JZ;
            else if (strcmp(op_str, ">=") == 0) cmp_op = OP_GE_JZ;
            else if (strcmp(op_str, "==") == 0) cmp_op = OP_EQ_JZ;
            else if (strcmp(op_str, "!=") == 0) cmp_op = OP_NE_JZ;

            if (cmp_op != (OpCode)-1) {
                Expr *l = cond->data.binary.left;
                Expr *r = cond->data.binary.right;
                if (r->kind == EXPR_INTEGER) {
                    int64_t imm = compile_int_literal_i64(r->data.integer.value);
                    if (imm >= -128 && imm <= 127) {
                        int reg = alloc_reg(c);
                        compile_expr_into(c, l, reg);
                        jz = chunk_emit_ABC(c->chunk, c->line, cmp_to_imm_direct[cmp_op], (uint8_t)reg, (uint8_t)(int8_t)imm, 0);
                        free_reg(c);
                        fused = true;
                    }
                } else if (l->kind == EXPR_INTEGER) {
                    int64_t imm = compile_int_literal_i64(l->data.integer.value);
                    if (imm >= -128 && imm <= 127) {
                        int reg = alloc_reg(c);
                        compile_expr_into(c, r, reg);
                        jz = chunk_emit_ABC(c->chunk, c->line, cmp_to_imm_swapped[cmp_op], (uint8_t)reg, (uint8_t)(int8_t)imm, 0);
                        free_reg(c);
                        fused = true;
                    }
                }
                if (!fused) {
                    int right = alloc_reg(c);
                    compile_expr_into(c, r, right);
                    int left = alloc_reg(c);
                    compile_expr_into(c, l, left);
                    jz = chunk_emit_ABC(c->chunk, c->line, cmp_op, (uint8_t)left, (uint8_t)right, 0);
                    free_reg(c);
                    free_reg(c);
                    fused = true;
                }
            }
        }

        if (!fused) {
            int condreg = alloc_reg(c);
            compile_expr_into(c, cond, condreg);
            jz = emit_jump(c, OP_JZ, (uint8_t)condreg);
            free_reg(c);
        }

        scope_enter(c);
        for (int i = 0; i < stmt->data.while_stmt.body_count; i++)
            compile_stmt(c, stmt->data.while_stmt.body[i]);
        scope_exit(c);

        int sbx = loop_start - (c->chunk->count + 1);
        emit_AsBx(c, OP_JMP, 0, sbx);

        if (fused) {
            int offset = c->chunk->count - (jz + 1);
            if (offset >= 0 && offset <= 255) {
                c->chunk->code[jz] = (c->chunk->code[jz] & 0x00FFFFFF) | ((uint32_t)offset << 24);
            }
        } else {
            patch_jump(c, jz);
        }

        loop_patch_breaks(c);
        loop_patch_continues(c);
        loop_pop(c);
        break;
    }

    case STMT_FOR: {
        scope_enter(c);

        Expr *iterable = stmt->data.for_stmt.iterable;
        const char *var_name = stmt->data.for_stmt.variable;

        /* Numeric range loop:  for i in range(a[, b[, step]])  -> OP_FORLOOP.
         * Recognised only when the callee is the builtin `range` with 1..3
         * positional arguments (no keyword arguments). The compiler extracts
         * start/stop/step and emits a numeric loop instead of materialising a
         * list via the iterator. */
        if (iterable->kind == EXPR_CALL &&
            iterable->data.call.callee->kind == EXPR_IDENTIFIER &&
            strcmp(iterable->data.call.callee->data.identifier.name, "range") == 0) {

            int nargs = iterable->data.call.arg_count;
            char **anames = iterable->data.call.arg_names;
            bool positional_only = true;
            for (int i = 0; i < nargs; i++) {
                if (anames && anames[i]) { positional_only = false; break; }
            }

            if (positional_only && nargs >= 1 && nargs <= 3) {
            Expr **rargs = iterable->data.call.arguments;
            Expr *start_expr = NULL, *stop_expr = NULL, *step_expr = NULL;
            if (nargs >= 1) stop_expr = rargs[0];
            if (nargs >= 2) { start_expr = rargs[0]; stop_expr = rargs[1]; }
            if (nargs >= 3) step_expr = rargs[2];

            int r_idx  = alloc_reg(c);   /* loop variable / current index */
            int r_lim  = alloc_reg(c);   /* limit (exclusive)            */
            int r_step = alloc_reg(c);   /* step                         */

            if (stop_expr) compile_expr_into(c, stop_expr, r_lim);
            else emit_AsBx(c, OP_LOADI, (uint8_t)r_lim, 0);

            if (step_expr) compile_expr_into(c, step_expr, r_step);
            else emit_AsBx(c, OP_LOADI, (uint8_t)r_step, 1);

            if (start_expr) compile_expr_into(c, start_expr, r_idx);
            else emit_AsBx(c, OP_LOADI, (uint8_t)r_idx, 0);

            add_local(c, var_name, r_idx);

            /* OP_FORPREP skips the loop when the range is already exhausted
             * (or step == 0) and falls through into the body otherwise, so the
             * first iteration runs with RA = start (no seed arithmetic, no entry
             * JMP on the happy path). */
            int forprep_ip = emit_jump(c, OP_FORPREP, (uint8_t)r_idx);

            int loop_start = c->chunk->count;
            loop_push(c, loop_start);

            scope_enter(c);
            for (int b = 0; b < stmt->data.for_stmt.body_count; b++)
                compile_stmt(c, stmt->data.for_stmt.body[b]);
            scope_exit(c);

            int continue_ip = c->chunk->count;
            c->loop->continue_ip = continue_ip;

            int sbx = loop_start - (c->chunk->count + 1);
            emit_AsBx(c, OP_FORLOOP, (uint8_t)r_idx, sbx);

            /* OP_FORPREP jumps to here (just past OP_FORLOOP) when the range is empty. */
            patch_jump(c, forprep_ip);

            loop_patch_breaks(c);
            loop_patch_continues(c);
            loop_pop(c);
            scope_exit(c);
            break;
            }
        }

        /* Generic iterator loop:  for x in iterable  -> OP_GETITER + OP_FORITER */
        int iter_expr = alloc_reg(c);
        compile_expr_into(c, iterable, iter_expr);

        int iter_base = alloc_reg(c);
        alloc_reg(c);
        int var = alloc_reg(c);
        
        emit_ABC(c, OP_GETITER, (uint8_t)iter_base, (uint8_t)iter_expr, 0);
        
        add_local(c, var_name, var);

        int jmp_to_forloop = emit_jump(c, OP_JMP, 0);

        int loop_start = c->chunk->count;
        loop_push(c, loop_start);

        /* body */
        scope_enter(c);
        for (int b = 0; b < stmt->data.for_stmt.body_count; b++)
            compile_stmt(c, stmt->data.for_stmt.body[b]);
        scope_exit(c);

        /* continue target */
        int continue_ip = c->chunk->count;
        c->loop->continue_ip = continue_ip;

        patch_jump(c, jmp_to_forloop);

        /* jump back if has next */
        int sbx = loop_start - (c->chunk->count + 1);
        emit_AsBx(c, OP_FORITER, (uint8_t)iter_base, sbx);

        loop_patch_breaks(c);
        loop_patch_continues(c);
        loop_pop(c);
        
        scope_exit(c);
        break;
    }

    case STMT_SWITCH: {
        int expr_reg = alloc_reg(c);
        compile_expr_into(c, stmt->data.switch_stmt.expression, expr_reg);

        int case_count = stmt->data.switch_stmt.case_count;
        int *cmp_jumps = malloc(sizeof(int) * case_count);
        int *case_indices = malloc(sizeof(int) * case_count);
        int cmp_count = 0;
        int default_idx = -1;

        for (int i = 0; i < case_count; i++) {
            SwitchCase *sc = &stmt->data.switch_stmt.cases[i];
            if (sc->value == NULL) {
                default_idx = i;
                continue;
            }
            int temp = alloc_reg(c);
            compile_expr_into(c, sc->value, temp);
            emit_ABC(c, OP_EQ, (uint8_t)temp, (uint8_t)expr_reg, (uint8_t)temp);
            case_indices[cmp_count] = i;
            cmp_jumps[cmp_count] = emit_jump(c, OP_JNZ, (uint8_t)temp);
            free_reg(c);
            cmp_count++;
        }

        int no_match = emit_jump(c, OP_JMP, 0);

        int *end_jumps = malloc(sizeof(int) * case_count);
        int end_count = 0;

        for (int i = 0; i < cmp_count; i++) {
            patch_jump(c, cmp_jumps[i]);
            SwitchCase *sc = &stmt->data.switch_stmt.cases[case_indices[i]];
            scope_enter(c);
            for (int j = 0; j < sc->body_count; j++)
                compile_stmt(c, sc->body[j]);
            scope_exit(c);
            end_jumps[end_count++] = emit_jump(c, OP_JMP, 0);
        }

        if (default_idx >= 0) {
            patch_jump(c, no_match);
            SwitchCase *sc = &stmt->data.switch_stmt.cases[default_idx];
            scope_enter(c);
            for (int j = 0; j < sc->body_count; j++)
                compile_stmt(c, sc->body[j]);
            scope_exit(c);
        } else {
            patch_jump(c, no_match);
        }

        for (int i = 0; i < end_count; i++)
            patch_jump(c, end_jumps[i]);

        free(cmp_jumps);
        free(case_indices);
        free(end_jumps);
        free_reg(c);
        break;
    }

    case STMT_THROW: {
        int r = alloc_reg(c);
        compile_expr_into(c, stmt->data.throw_stmt.expression, r);
        emit_ABC(c, OP_THROW, (uint8_t)r, 0, 0);
        free_reg(c);
        break;
    }

    case STMT_TRY: {
        int exc_reg = alloc_reg(c);
        int try_idx = -1;
        int skip_dispatch = -1;

        bool has_catch = stmt->data.try_stmt.catch_count > 0;
        bool has_finally = stmt->data.try_stmt.finally_count > 0;

        if (has_finally) {
            finally_ctx_push(c, stmt->data.try_stmt.finally_body,
                              stmt->data.try_stmt.finally_count,
                              has_catch);
        }

        if (has_catch) {
            try_idx = emit_jump(c, OP_TRY, (uint8_t)exc_reg);
        }

        scope_enter(c);
        for (int i = 0; i < stmt->data.try_stmt.try_count; i++)
            compile_stmt(c, stmt->data.try_stmt.try_body[i]);
        scope_exit(c);

        if (has_catch) {
            emit_ABC(c, OP_ENDTRY, 0, 0, 0);
            skip_dispatch = emit_jump(c, OP_JMP, 0);
            patch_jump(c, try_idx);

            int *end_jumps = malloc(sizeof(int) * stmt->data.try_stmt.catch_count);
            int end_count = 0;

            for (int ci = 0; ci < stmt->data.try_stmt.catch_count; ci++) {
                CatchClause *cc = &stmt->data.try_stmt.catch_clauses[ci];
                int next_clause = -1;

                if (cc->type_name) {
                    int cls_reg = alloc_reg(c);
                    int const_idx = chunk_add_const(c->chunk, make_obj((Object*)new_string(cc->type_name, (int)strlen(cc->type_name))));
                    chunk_emit_ABx(c->chunk, c->line, OP_GETGLOBAL, (uint8_t)cls_reg, (uint16_t)const_idx);
                    int bool_reg = alloc_reg(c);
                    emit_ABC(c, OP_ISINSTANCE, (uint8_t)bool_reg, (uint8_t)exc_reg, (uint8_t)cls_reg);
                    next_clause = emit_jump(c, OP_JZ, (uint8_t)bool_reg);
                    free_reg(c);
                    free_reg(c);
                }

                if (cc->variable) {
                    int r = alloc_reg(c);
                    add_local(c, cc->variable, r);
                    emit_move(c, (uint8_t)r, (uint8_t)exc_reg);
                }
                scope_enter(c);
                for (int i = 0; i < cc->body_count; i++)
                    compile_stmt(c, cc->body[i]);
                scope_exit(c);

                end_jumps[end_count++] = emit_jump(c, OP_JMP, 0);

                if (next_clause >= 0) {
                    patch_jump(c, next_clause);
                }
            }

            /* No handler matched — rethrow */
            emit_ABC(c, OP_THROW, (uint8_t)exc_reg, 0, 0);

            for (int i = 0; i < end_count; i++)
                patch_jump(c, end_jumps[i]);
            free(end_jumps);

            patch_jump(c, skip_dispatch);
        }

        if (has_finally) {
            scope_enter(c);
            for (int i = 0; i < stmt->data.try_stmt.finally_count; i++)
                compile_stmt(c, stmt->data.try_stmt.finally_body[i]);
            scope_exit(c);
            finally_ctx_pop(c);
        }

        free_reg(c);
        break;
    }
    }
}

/* ============================================================ */
/* Declarations                                                 */
/* ============================================================ */

static void compile_function(Compiler *c, Decl *decl);
static void compile_class(Compiler *c, Decl *decl);
static void compile_enum(Compiler *c, Decl *decl);

static void compile_decl(Compiler *c, Decl *decl) {
    c->line = decl->line;
    switch (decl->kind) {
    case DECL_FUNCTION: compile_function(c, decl); break;
    case DECL_CLASS:    compile_class(c, decl);    break;
    case DECL_ENUM:     compile_enum(c, decl);     break;
    case DECL_IMPORT: {
        int mod_reg = alloc_reg(c);
        int mod_k = chunk_add_string(c->vm, c->chunk, decl->data.import_decl.module_name);
        emit_ABx(c, OP_IMPORT, (uint8_t)mod_reg, (uint16_t)mod_k);

        if (decl->data.import_decl.import_all) {
            /* from X import * — store the module object itself */
            emit_ABx(c, OP_SETGLOBAL, (uint8_t)mod_reg, (uint16_t)mod_k);
        } else if (decl->data.import_decl.items && decl->data.import_decl.item_count > 0) {
            /* from X import a, b */
            for (int i = 0; i < decl->data.import_decl.item_count; i++) {
                int item_reg = alloc_reg(c);
                int item_k = chunk_add_string(c->vm, c->chunk, decl->data.import_decl.items[i]);
                emit_ABC(c, OP_MEMBERGET, (uint8_t)item_reg, (uint8_t)mod_reg, (uint8_t)item_k);
                emit_ABx(c, OP_SETGLOBAL, (uint8_t)item_reg, (uint16_t)item_k);
                free_reg(c);
            }
        } else {
            /* import module */
            emit_ABx(c, OP_SETGLOBAL, (uint8_t)mod_reg, (uint16_t)mod_k);
        }
        free_reg(c);
        break;
    }
    }
}

/* Compile a default-value expression into a 0-argument thunk.  The resulting
 * ObjFunction is stored in the callee's `defaults` array and invoked by
 * bind_keyword_arguments when the parameter is not supplied.  Captures of the
 * enclosing scope are allowed (the thunk is compiled with `c` as parent). */
static Value compile_default_thunk(Compiler *c, Expr *expr) {
    Chunk thunk_chunk;
    chunk_init(&thunk_chunk, "<default>");

    Compiler sub = {
        .chunk       = &thunk_chunk,
        .scope       = NULL,
        .vm          = c->vm,
        .temp_base   = 0,
        .max_temp_base = 0,
        .line        = c->line,
        .func_depth  = c->func_depth + 1,
        .loop        = NULL,
        .finally_ctx = NULL,
        .parent      = c,
        .upvalue_count = 0
    };

    scope_enter(&sub);
    compile_expr_into(&sub, expr, 0);
    emit_ret(&sub, 0);
    scope_exit(&sub);

    thunk_chunk.max_registers = sub.max_temp_base > 0 ? sub.max_temp_base : 1;
    ObjFunction *thunk = new_function("<default>");
    thunk->chunk       = malloc(sizeof(Chunk));
    *thunk->chunk      = thunk_chunk;
    thunk->param_count = 0;
    thunk->is_leaf     = true;
    thunk->upvalue_count = sub.upvalue_count;
    if (sub.upvalue_count > 0) {
        thunk->upvalue_descriptors = malloc(sizeof(UpvalueDesc) * sub.upvalue_count);
        for (int i = 0; i < sub.upvalue_count; i++) {
            thunk->upvalue_descriptors[i].index = sub.upvalues[i].index;
            thunk->upvalue_descriptors[i].is_local = sub.upvalues[i].is_local;
        }
    }
    return make_obj((Object*)thunk);
}

/* Build a constant List of keyword argument names (in declaration order) and
 * return its constant-pool index.  Consumed by OP_KW_PREFIX + bind_keyword_arguments. */
static int build_kw_names_const(Compiler *c, Expr *expr, int nargs) {
    ObjList *names = new_list(nargs);
    for (int i = 0; i < nargs; i++) {
        const char *nm = expr->data.call.arg_names[i];
        if (nm) list_add(names, make_obj((Object*)new_string(nm, (int)strlen(nm))));
    }
    return chunk_add_const(c->chunk, make_obj((Object*)names));
}

static int compile_function_value(Compiler *c, const char *name,
                                   FunctionParam *params, int param_count,
                                   Stmt **body, int body_count) {
    Chunk fn_chunk;
    chunk_init(&fn_chunk, name ? name : "");

    Compiler sub = {
        .chunk       = &fn_chunk,
        .scope       = NULL,
        .vm          = c->vm,
        .temp_base   = 0,
        .max_temp_base = 0,
        .line        = c->line,
        .func_depth  = c->func_depth + 1,
        .loop        = NULL,
        .finally_ctx = NULL,
        .parent      = c,
        .upvalue_count = 0
    };

    scope_enter(&sub);

    for (int i = 0; i < param_count; i++) {
        add_local(&sub, params[i].name, i);
    }
    sub.temp_base = param_count;

    /* Build default-value thunks (applied by bind_keyword_arguments at call
       time).  Stored on the function after it is created below. */
    Value *def_thunks = NULL;
    if (param_count > 0) {
        def_thunks = malloc(sizeof(Value) * param_count);
        for (int i = 0; i < param_count; i++) def_thunks[i] = make_null();
    }
    for (int i = 0; i < param_count; i++) {
        if (params[i].default_value)
            def_thunks[i] = compile_default_thunk(c, params[i].default_value);
    }

    for (int i = 0; i < body_count; i++) {
        compile_stmt(&sub, body[i]);
    }

    bool last_is_return = body_count > 0 && body[body_count - 1]->kind == STMT_RETURN;
    if (!last_is_return) {
        emit_loadnull(&sub, 0);
        emit_ret(&sub, 0);
    }

    scope_exit(&sub);

    /* Leaf detection: no calls, no upvalues, no exceptions, no kwargs.
     * OP_CLOSURE is also disqualifying: a leaf call reuses the caller's
     * CallFrame (no new frame pushed, no close_upvalues on return).  Any
     * capture_upvalue() executed inside the leaf body stamps frame_depth =
     * vm->frame_count and stack_index = FRAME.base + reg.  When the leaf
     * returns, FRAME.base is restored to the caller's base but the upvalue
     * remains open, pointing at slots that are now caller-owned — silent
     * heap/value corruption on the next write to those slots. */
    bool is_leaf = (sub.upvalue_count == 0);
    if (is_leaf) {
        for (int i = 0; i < fn_chunk.count; i++) {
            OpCode op = DECODE_OP(fn_chunk.code[i]);
            if (op == OP_CALL || op == OP_INVOKE || op == OP_SUPER ||
                op == OP_TRY || op == OP_CLOSURE) {
                is_leaf = false;
                break;
            }
        }
    }

    fn_chunk.max_registers = sub.max_temp_base;
    ObjFunction *fn = new_function(name ? name : "");
    fn->chunk       = malloc(sizeof(Chunk));
    *fn->chunk      = fn_chunk;
    fn->param_count = param_count;
    if (param_count > 0) {
        fn->param_names = malloc(sizeof(char*) * param_count);
        fn->param_name_objs = malloc(sizeof(ObjString*) * param_count);
        for (int i = 0; i < param_count; i++) {
            fn->param_names[i] = strdup(params[i].name);
            fn->param_name_objs[i] = new_string(params[i].name, (int)strlen(params[i].name));
        }
    }
    fn->upvalue_count = sub.upvalue_count;
    fn->is_leaf = is_leaf;
    fn->defaults = def_thunks;
    fn->default_count = param_count;
    if (sub.upvalue_count > 0) {
        fn->upvalue_descriptors = malloc(sizeof(UpvalueDesc) * sub.upvalue_count);
        for (int i = 0; i < sub.upvalue_count; i++) {
            fn->upvalue_descriptors[i].index = sub.upvalues[i].index;
            fn->upvalue_descriptors[i].is_local = sub.upvalues[i].is_local;
        }
    }

    int fn_const = chunk_add_const(c->chunk, make_obj((Object*)fn));
    return fn_const;
}

static void compile_function(Compiler *c, Decl *decl) {
    const char *name = decl->data.function.name;
    int param_count  = decl->data.function.param_count;

    int fn_const = compile_function_value(c, name,
                                           decl->data.function.params, param_count,
                                           decl->data.function.body,
                                           decl->data.function.body_count);

    int r = alloc_reg(c);

    if (c->func_depth == 0) {
        emit_ABx(c, OP_CLOSURE, (uint8_t)r, (uint16_t)fn_const);
        int k = chunk_add_string(c->vm, c->chunk, name);
        emit_ABx(c, OP_SETGLOBAL, (uint8_t)r, (uint16_t)k);
        free_reg(c);
    } else {
        add_local(c, name, r);
        emit_ABx(c, OP_CLOSURE, (uint8_t)r, (uint16_t)fn_const);
    }
}

static void compile_class(Compiler *c, Decl *decl) {
    const char *name = decl->data.class_decl.name;
    ObjClass *cls = new_class(name, decl->data.class_decl.base_class);

    /* Inheritance */
    if (decl->data.class_decl.base_class) {
        Value parent_val;
        if (vm_get_global(c->vm, decl->data.class_decl.base_class, &parent_val)
            && IS_CLASS(parent_val)) {
            ObjClass *parent = (ObjClass*)AS_OBJ(parent_val);
            cls->base = parent;

            /* Copy parent methods */
            if (parent->method_count > 0) {
                cls->methods = malloc(sizeof(ObjFunction*) * parent->method_count);
                cls->method_names = malloc(sizeof(char*) * parent->method_count);
                memcpy(cls->methods, parent->methods, sizeof(ObjFunction*) * parent->method_count);
                for (int i = 0; i < parent->method_count; i++) {
                    cls->method_names[i] = strdup(parent->method_names[i]);
                }
                cls->method_count = parent->method_count;
                cls->method_capacity = parent->method_count;
            }
        }
    }

    /* Create prototype with default field values */
    cls->prototype = new_instance(cls, 4);
    /* The prototype's klass points to cls, but we should NOT retain-cycle.
       The prototype is owned by the class, and the class is owned by globals,
       so when the class is freed, it frees the prototype too. */

    /* Inherit parent prototype fields first (so slot indices match parent methods) */
    if (cls->base && cls->base->prototype) {
        ObjInstance *pp = cls->base->prototype;
        for (int i = 0; i < pp->field_count; i++) {
            instance_set_field(cls->prototype, pp->field_names[i], pp->fields[i]);
        }
    }

    /* Fields */
    for (int i = 0; i < decl->data.class_decl.field_count; i++) {
        instance_set_field(cls->prototype, decl->data.class_decl.fields[i].name, make_null());
    }

    /* Build field_slot_map for O(1) slot-based access */
    cls->field_count = cls->prototype->field_count;
    cls->field_slot_map = new_dict();
    for (int i = 0; i < cls->prototype->field_count; i++) {
        dict_set(cls->field_slot_map,
                 make_obj((Object*)new_string(cls->prototype->field_names[i],
                                              (int)strlen(cls->prototype->field_names[i]))),
                 make_int(i));
    }

    /* Methods */
    for (int i = 0; i < decl->data.class_decl.method_count; i++) {
        Decl *m = decl->data.class_decl.methods[i];
        if (m->kind != DECL_FUNCTION) continue;

        Chunk mchunk;
        chunk_init(&mchunk, m->data.function.name);
        Compiler sub = {
            .chunk       = &mchunk,
            .scope       = NULL,
            .vm          = c->vm,
            .temp_base   = 0,
            .max_temp_base = 0,
            .line        = c->line,
            .func_depth  = c->func_depth + 1,
            .loop        = NULL,
            .finally_ctx = NULL,
            .current_class = cls
        };
        scope_enter(&sub);
        /* params (incl. explicit self) at reg 0..n-1 */
        for (int j = 0; j < m->data.function.param_count; j++) {
            add_local(&sub, m->data.function.params[j].name, j);
        }
        sub.temp_base = m->data.function.param_count;
        /* Build default-value thunks for method params (self is a regular param). */
        int mparam = m->data.function.param_count;
        size_t mdef_bytes = sizeof(Value) * ((mparam > 0) ? (size_t)mparam : (size_t)1);
        Value *mdef = malloc(mdef_bytes);
        for (int j = 0; j < mparam; j++) mdef[j] = make_null();
        for (int j = 0; j < mparam; j++) {
            if (m->data.function.params[j].default_value)
                mdef[j] = compile_default_thunk(c, m->data.function.params[j].default_value);
        }

        for (int j = 0; j < m->data.function.body_count; j++)
            compile_stmt(&sub, m->data.function.body[j]);
        emit_loadnull(&sub, 0);
        emit_ret(&sub, 0);
        scope_exit(&sub);

        /* Leaf detection for method: no calls, no upvalues, no exceptions, no kwargs.
         * OP_CLOSURE is disqualifying for the same reason as in compile_function_value:
         * the leaf fast-path never closes upvalues on return, so any capture_upvalue()
         * call inside the method body leaves dangling open upvalues pointing at stale
         * stack slots once the leaf frame's base is restored to the caller's base. */
        bool is_leaf = (sub.upvalue_count == 0);
        if (is_leaf) {
            for (int i = 0; i < mchunk.count; i++) {
                OpCode op = DECODE_OP(mchunk.code[i]);
                if (op == OP_CALL || op == OP_INVOKE || op == OP_SUPER ||
                    op == OP_TRY || op == OP_CLOSURE) {
                    is_leaf = false;
                    break;
                }
            }
        }

        mchunk.max_registers = sub.max_temp_base;
        ObjFunction *mf = new_function(m->data.function.name);
        mf->chunk       = malloc(sizeof(Chunk));
        *mf->chunk      = mchunk;
        mf->is_leaf = is_leaf;
        mf->param_count = m->data.function.param_count;
        mf->defaults = mdef;
        mf->default_count = mparam;
        mf->param_names = malloc(sizeof(char*) * mf->param_count);
        mf->param_name_objs = malloc(sizeof(ObjString*) * mf->param_count);
        for (int j = 0; j < m->data.function.param_count; j++) {
            mf->param_names[j] = strdup(m->data.function.params[j].name);
            mf->param_name_objs[j] = new_string(
                m->data.function.params[j].name,
                (int)strlen(m->data.function.params[j].name));
        }

        /* Store in class */
        if (cls->method_count >= cls->method_capacity) {
            cls->method_capacity = cls->method_capacity ? cls->method_capacity * 2 : 4;
            cls->methods = realloc(cls->methods, sizeof(ObjFunction*) * cls->method_capacity);
            cls->method_names = realloc(cls->method_names, sizeof(char*) * cls->method_capacity);
        }
        cls->method_names[cls->method_count] = strdup(m->data.function.name);
        cls->methods[cls->method_count] = mf;
        cls->method_count++;
        /* Add method function to parent chunk constants so --dump-bytecode can find it */
        chunk_add_const(c->chunk, make_obj((Object*)mf));
    }

    Value cls_val = make_obj((Object*)cls);
    vm_set_global(c->vm, name, cls_val, false);
}

static void compile_enum(Compiler *c, Decl *decl) {
    const char *name = decl->data.enum_decl.name;
    int count = decl->data.enum_decl.variant_count;
    ObjEnum *e = new_enum(name, count);
    int64_t next_val = 0;
    for (int i = 0; i < count; i++) {
        EnumVariant *v = &decl->data.enum_decl.variants[i];
        int64_t val = v->has_value ? v->value : next_val;
        next_val = val + 1;
        e->names[i]  = strdup(v->name);
        e->values[i] = val;
    }
    vm_set_global(c->vm, name, make_obj((Object*)e), false);
}

/* ============================================================ */
/* Public API                                                   */
/* ============================================================ */

bool compile_program(Program *program, Chunk *chunk, VM *vm, bool is_repl, bool is_module) {
    chunk_init(chunk, "<main>");

    Compiler c = {
        .chunk      = chunk,
        .scope      = NULL,
        .vm         = vm,
        .temp_base  = 0,
        .max_temp_base = 0,
        .line       = 0,
        .func_depth = 0,
        .loop       = NULL,
        .finally_ctx = NULL,
        .is_repl    = is_repl
    };

    scope_enter(&c);

    /* First pass: declarations (functions, classes, enums) so they're global */
    for (int i = 0; i < program->decl_count; i++)
        compile_decl(&c, program->declarations[i]);

    /* Second pass: top-level statements */
    for (int i = 0; i < program->stmt_count; i++) {
        if (c.is_repl && i == program->stmt_count - 1 &&
            program->statements[i]->kind == STMT_EXPRESSION) {
            Expr *expr = program->statements[i]->data.expression.expression;
            if (expr->kind != EXPR_ASSIGNMENT && expr->kind != EXPR_COMPOUND_ASSIGN &&
                expr->kind != EXPR_MULTI_ASSIGN) {
                int r = alloc_reg(&c);
                compile_expr_into(&c, expr, r);
                int k = chunk_add_string(c.vm, c.chunk, "_");
                emit_ABx(&c, OP_SETGLOBAL, (uint8_t)r, (uint16_t)k);
                free_reg(&c);
                continue;
            }
        }
        compile_stmt(&c, program->statements[i]);
    }

    scope_exit(&c);

    chunk->max_registers = c.max_temp_base;
    if (is_module)
        emit_ABC(&c, OP_RET, 0, 0, 0);
    else
        emit_ABC(&c, OP_HALT, 0, 0, 0);
    return true;
}
