/* compiler.c — AST → Bytecode compiler. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "compiler.h"
#include "opcode.h"
#include "value.h"

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
    struct Compiler *parent;
    bool        is_repl;
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

static void emit_move(Compiler *c, uint8_t dst, uint8_t src) {
    emit_ABC(c, OP_MOVE, dst, src, 0);
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

static void emit_loadstring(Compiler *c, uint8_t dst, const char *s) {
    int k = chunk_add_string(c->chunk, s);
    emit_ABx(c, OP_LOADK, dst, (uint16_t)k);
}

static void emit_loadstring_len(Compiler *c, uint8_t dst, const char *s, int length) {
    int k = chunk_add_string_len(c->chunk, s, length);
    emit_ABx(c, OP_LOADK, dst, (uint16_t)k);
}

static void emit_ret(Compiler *c, uint8_t reg) {
    emit_ABC(c, OP_RET, reg, 0, 0);
}

static void emit_enter(Compiler *c, uint16_t nlocals) {
    emit_ABx(c, OP_ENTER, 0, nlocals);
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
static int compile_function_value(Compiler *c, const char *name,
                                   FunctionParam *params, int param_count,
                                   Stmt **body, int body_count);

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
            int k = chunk_add_string(c->chunk, name);
            emit_ABx(c, OP_SETGLOBAL, (uint8_t)src_reg, (uint16_t)k);
        }
    } else if (lhs->kind == EXPR_FIELD_ACCESS) {
        int obj = alloc_reg(c);
        compile_expr_into(c, lhs->data.field_access.obj, obj);
        int fk = chunk_add_string(c->chunk, lhs->data.field_access.field);
        if (fk > 255) {
            int temp = alloc_reg(c);
            emit_ABx(c, OP_LOADK, (uint8_t)temp, (uint16_t)fk);
            emit_ABC(c, OP_MEMBERSET, (uint8_t)obj, (uint8_t)src_reg, (uint8_t)temp);
            free_reg(c);
        } else {
            emit_ABC(c, OP_MEMBERSET, (uint8_t)obj, (uint8_t)src_reg, (uint8_t)fk);
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
        int64_t v = atoll(expr->data.integer.value);
        if (v >= INT32_MIN && v <= INT32_MAX) {
            emit_loadi(c, (uint8_t)target, (int32_t)v);
        } else {
            int k = chunk_add_const(c->chunk, make_int64(v));
            emit_ABx(c, OP_LOADK, (uint8_t)target, (uint16_t)k);
        }
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

    case EXPR_CHAR: {
        emit_loadi(c, (uint8_t)target, (int32_t)expr->data.char_lit.value);
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
            int k = chunk_add_string(c->chunk, expr->data.identifier.name);
            emit_ABx(c, OP_GETGLOBAL, (uint8_t)target, (uint16_t)k);
        }
        break;
    }

    case EXPR_BINARY: {
        const char *op_str = expr->data.binary.operator;

        if (strcmp(op_str, "and") == 0) {
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
        else if (strcmp(op_str, "or") == 0) {
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

        /* Not short-circuit: evaluate both sides eagerly */

        /* ADDI/SUBI fast path: x +/- small_literal  →  single imm instruction,
           no temp register needed. Range -128..127 fits in the B byte field. */
        if ((strcmp(op_str, "+") == 0 || strcmp(op_str, "-") == 0) &&
            expr->data.binary.right->kind == EXPR_INTEGER) {
            int64_t imm = atoll(expr->data.binary.right->data.integer.value);
            if (imm >= -128 && imm <= 127) {
                compile_expr_into(c, expr->data.binary.left, target);
                OpCode iop = (strcmp(op_str, "+") == 0) ? OP_ADDI : OP_SUBI;
                emit_ABC(c, iop, (uint8_t)target, (uint8_t)(int8_t)imm, 0);
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
        else if (strcmp(op_str, "%") == 0) op = OP_MOD;
        else if (strcmp(op_str, "==") == 0) op = OP_EQ;
        else if (strcmp(op_str, "!=") == 0) op = OP_NE;
        else if (strcmp(op_str, "<") == 0) op = OP_LT;
        else if (strcmp(op_str, "<=") == 0) op = OP_LE;
        else if (strcmp(op_str, ">") == 0) op = OP_GT;
        else if (strcmp(op_str, ">=") == 0) op = OP_GE;
        else if (strcmp(op_str, "in") == 0) op = OP_IN;
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
        break;
    }

    case EXPR_UNARY: {
        compile_expr_into(c, expr->data.unary.operand, target);
        const char *op_str = expr->data.unary.operator;
        if (strcmp(op_str, "-") == 0) {
            emit_ABC(c, OP_NEG, (uint8_t)target, (uint8_t)target, 0);
        } else if (strcmp(op_str, "!") == 0) {
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
            /* Detect super.method() */
            bool is_super = (obj->kind == EXPR_IDENTIFIER &&
                             strcmp(obj->data.identifier.name, "super") == 0);
            if (is_super) {
                int saved_base = c->temp_base;
                c->temp_base = target + 2 + nargs;
                for (int i = 0; i < nargs; i++) {
                    compile_expr_into(c, expr->data.call.arguments[i], target + 2 + i);
                }
                c->temp_base = saved_base;
                int mk = chunk_add_string(c->chunk, callee->data.field_access.field);
                emit_ABx(c, OP_LOADK, (uint8_t)(target + 1), (uint16_t)mk);
                emit_ABC(c, OP_SUPER, (uint8_t)target, 0, (uint8_t)nargs);
            } else if (has_keywords) {
                /* Keyword method call: resolve to bound method, then OP_KCALL */
                compile_expr_into(c, obj, target);
                int mk = chunk_add_string(c->chunk, callee->data.field_access.field);
                if (mk <= 255) {
                    emit_ABC(c, OP_MEMBERGET, (uint8_t)target, (uint8_t)target, (uint8_t)mk);
                    /* Now same as regular keyword call — target holds bound method */
                    int pos_count = 0;
                    for (int i = 0; i < nargs; i++) {
                        if (expr->data.call.arg_names[i] == NULL) pos_count = i + 1;
                    }
                    int saved_base = c->temp_base;
                    c->temp_base = target + 1 + nargs + 1;
                    int compiled_pos = 0;
                    for (int i = 0; i < nargs; i++) {
                        if (expr->data.call.arg_names[i] == NULL) {
                            compile_expr_into(c, expr->data.call.arguments[i], target + 1 + compiled_pos);
                            compiled_pos++;
                        }
                    }
                    int kwargs_reg = target + pos_count + 1;
                    emit_ABC(c, OP_NEWDICT, (uint8_t)kwargs_reg, 0, 0);
                    int key_reg = alloc_reg(c);
                    int val_reg = alloc_reg(c);
                    for (int i = 0; i < nargs; i++) {
                        if (expr->data.call.arg_names[i] != NULL) {
                            emit_loadstring(c, (uint8_t)key_reg, expr->data.call.arg_names[i]);
                            compile_expr_into(c, expr->data.call.arguments[i], val_reg);
                            emit_ABC(c, OP_INDEXSET, (uint8_t)kwargs_reg, (uint8_t)key_reg, (uint8_t)val_reg);
                        }
                    }
                    free_reg(c);
                    free_reg(c);
                    c->temp_base = saved_base;
                    emit_ABC(c, OP_KCALL, (uint8_t)target, (uint8_t)target, (uint8_t)pos_count);
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
                int mk = chunk_add_string(c->chunk, callee->data.field_access.field);
                emit_ABx(c, OP_LOADK, (uint8_t)(target + 1), (uint16_t)mk);
                emit_ABC(c, OP_INVOKE, (uint8_t)target, (uint8_t)target, (uint8_t)nargs);
            }
        } else if (has_keywords) {
            /* Keyword call: compile callee, positional args, kwargs dict, then OP_KCALL */
            compile_expr_into(c, callee, target);
            int pos_count = 0;
            for (int i = 0; i < nargs; i++) {
                if (expr->data.call.arg_names[i] == NULL) pos_count = i + 1;
            }
            int saved_base = c->temp_base;
            c->temp_base = target + 1 + nargs + 1;
            /* Compile positional args into target+1..target+pos_count */
            int compiled_pos = 0;
            for (int i = 0; i < nargs; i++) {
                if (expr->data.call.arg_names[i] == NULL) {
                    compile_expr_into(c, expr->data.call.arguments[i], target + 1 + compiled_pos);
                    compiled_pos++;
                }
            }
            /* Create kwargs dict at target + pos_count + 1 */
            int kwargs_reg = target + pos_count + 1;
            emit_ABC(c, OP_NEWDICT, (uint8_t)kwargs_reg, 0, 0);
            /* Build kwargs dict */
            int key_reg = alloc_reg(c);
            int val_reg = alloc_reg(c);
            for (int i = 0; i < nargs; i++) {
                if (expr->data.call.arg_names[i] != NULL) {
                    emit_loadstring(c, (uint8_t)key_reg, expr->data.call.arg_names[i]);
                    compile_expr_into(c, expr->data.call.arguments[i], val_reg);
                    emit_ABC(c, OP_INDEXSET, (uint8_t)kwargs_reg, (uint8_t)key_reg, (uint8_t)val_reg);
                }
            }
            free_reg(c);
            free_reg(c);
            c->temp_base = saved_base;
            emit_ABC(c, OP_KCALL, (uint8_t)target, (uint8_t)target, (uint8_t)pos_count);
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
        compile_expr_into(c, expr->data.field_access.obj, target);
        int fk = chunk_add_string(c->chunk, expr->data.field_access.field);
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
        compile_expr_into(c, expr->data.slice.obj, target);
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
        OpCode slice_op = expr->data.slice.optional ? OP_SLICE_SAFE : OP_SLICE;
        emit_ABC(c, slice_op, (uint8_t)target, (uint8_t)target, 0);
        free_reg(c);
        free_reg(c);
        free_reg(c);
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
                int k = chunk_add_string(c->chunk, name);
                emit_ABx(c, OP_SETGLOBAL, (uint8_t)target, (uint16_t)k);
            }
        } else if (lhs->kind == EXPR_FIELD_ACCESS) {
            int obj = alloc_reg(c);
            compile_expr_into(c, lhs->data.field_access.obj, obj);
            compile_expr_into(c, rhs, target);
            int fk = chunk_add_string(c->chunk, lhs->data.field_access.field);
            if (fk > 255) {
                int temp = alloc_reg(c);
                emit_ABx(c, OP_LOADK, (uint8_t)temp, (uint16_t)fk);
                emit_ABC(c, OP_MEMBERSET, (uint8_t)obj, (uint8_t)target, (uint8_t)temp);
                free_reg(c);
            } else {
                emit_ABC(c, OP_MEMBERSET, (uint8_t)obj, (uint8_t)target, (uint8_t)fk);
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
                    int k = chunk_add_string(c->chunk, vname);
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
                    int k = chunk_add_string(c->chunk, vname);
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
        else if (strcmp(op_str, "/=") == 0) op = OP_DIV;
        else if (strcmp(op_str, "%=") == 0) op = OP_MOD;
        else if (strcmp(op_str, "&=") == 0) op = OP_BAND;
        else if (strcmp(op_str, "|=") == 0) op = OP_BOR;
        else if (strcmp(op_str, "^=") == 0) op = OP_BXOR;

        if (lhs->kind == EXPR_IDENTIFIER) {
            /* simple identifiers can be re-evaluated cleanly */
            Expr fake_rhs = *rhs;
            char bin_op[3] = {op_str[0], '\0'};
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
            
            int fk = chunk_add_string(c->chunk, lhs->data.field_access.field);
            int temp_val = alloc_reg(c);
            
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

        int lk = chunk_add_string(c->chunk, "length");
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
        int ck = chunk_add_string(c->chunk, expr->data.new_expr.class_name);
        emit_ABx(c, OP_NEW, (uint8_t)target, (uint16_t)ck);
        int nargs = expr->data.new_expr.arg_count;
        int saved_base = c->temp_base;
        c->temp_base = target + nargs + 2;
        for (int i = 0; i < nargs; i++) {
            compile_expr_into(c, expr->data.new_expr.arguments[i], target + 2 + i);
        }
        c->temp_base = saved_base;
        /*
         * Always emit OP_INVOKE("_init") — don't peek at runtime state.
         * OP_INVOKE already handles missing _init (returns nil, see
         * vm_opcodes.inc strcmp("_init")==0 branch).
         *
         * Register layout: r_target=instance, r_target+1="_init",
         * r_target+2..=args, r_target+nargs+1=scratch (nil return lands here,
         * NOT on the instance).
         */
        int mk = chunk_add_string(c->chunk, "_init");
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
        int *temps = (int *)malloc(m * sizeof(int));
        bool *direct = (bool *)calloc(m, sizeof(bool));
        for (int i = 0; i < m; i++) {
            ExprKind vk = expr->data.multi_assign.values[i]->kind;
            bool is_literal = (vk == EXPR_INTEGER || vk == EXPR_FLOAT ||
                               vk == EXPR_STRING || vk == EXPR_BOOL ||
                               vk == EXPR_NULL || vk == EXPR_CHAR);
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
                        int k = chunk_add_string(c->chunk, lhs->data.identifier.name);
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
        free(temps);
        free(direct);
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
                        int k = chunk_add_string(c->chunk, var_name);
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
                        int k = chunk_add_string(c->chunk, var_name);
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
                int k = chunk_add_string(c->chunk, name);
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
        int j = emit_jump(c, OP_JMP, 0);
        loop_record_break(c, j);
        break;
    }

    case STMT_CONTINUE: {
        if (!c->loop) {
            fprintf(stderr, "compiler: continue outside loop\n");
            break;
        }
        if (c->loop->continue_ip >= 0) {
            /* for-loop: jump to increment */
            int sbx = c->loop->continue_ip - (c->chunk->count + 1);
            emit_AsBx(c, OP_JMP, 0, sbx);
        } else {
            /* while-loop: target not known yet if body still compiling,
               but for while we know start_ip immediately. */
            int sbx = c->loop->start_ip - (c->chunk->count + 1);
            emit_AsBx(c, OP_JMP, 0, sbx);
        }
        break;
    }

    case STMT_IF: {
        int cond = alloc_reg(c);
        compile_expr_into(c, stmt->data.if_stmt.condition, cond);
        int jz = emit_jump(c, OP_JZ, (uint8_t)cond);
        free_reg(c);

        scope_enter(c);
        for (int i = 0; i < stmt->data.if_stmt.then_count; i++)
            compile_stmt(c, stmt->data.if_stmt.then_body[i]);
        scope_exit(c);

        if (stmt->data.if_stmt.else_count > 0) {
            int j = emit_jump(c, OP_JMP, 0);
            patch_jump(c, jz);
            scope_enter(c);
            for (int i = 0; i < stmt->data.if_stmt.else_count; i++)
                compile_stmt(c, stmt->data.if_stmt.else_body[i]);
            scope_exit(c);
            patch_jump(c, j);
        } else {
            patch_jump(c, jz);
        }
        break;
    }

    case STMT_WHILE: {
        int loop_start = c->chunk->count;
        loop_push(c, loop_start);

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

            if (cmp_op != (OpCode)-1) {
                int right = alloc_reg(c);
                compile_expr_into(c, cond->data.binary.right, right);
                int left = alloc_reg(c);
                compile_expr_into(c, cond->data.binary.left, left);
                jz = chunk_emit_ABC(c->chunk, c->line, cmp_op, (uint8_t)left, (uint8_t)right, 0);
                free_reg(c);
                free_reg(c);
                fused = true;
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
        loop_pop(c);
        break;
    }

    case STMT_FOR: {
        scope_enter(c);

        int iter_expr = alloc_reg(c);
        compile_expr_into(c, stmt->data.for_stmt.iterable, iter_expr);

        int iter_base = alloc_reg(c);
        alloc_reg(c);
        int var = alloc_reg(c);
        
        emit_ABC(c, OP_GETITER, (uint8_t)iter_base, (uint8_t)iter_expr, 0);
        
        add_local(c, stmt->data.for_stmt.variable, var);

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
        emit_AsBx(c, OP_FORLOOP, (uint8_t)iter_base, sbx);

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

        if (stmt->data.try_stmt.catch_count > 0) {
            try_idx = emit_jump(c, OP_TRY, (uint8_t)exc_reg);
        }

        scope_enter(c);
        for (int i = 0; i < stmt->data.try_stmt.try_count; i++)
            compile_stmt(c, stmt->data.try_stmt.try_body[i]);
        scope_exit(c);

        if (stmt->data.try_stmt.catch_count > 0) {
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

        if (stmt->data.try_stmt.finally_count > 0) {
            scope_enter(c);
            for (int i = 0; i < stmt->data.try_stmt.finally_count; i++)
                compile_stmt(c, stmt->data.try_stmt.finally_body[i]);
            scope_exit(c);
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
        int mod_k = chunk_add_string(c->chunk, decl->data.import_decl.module_name);
        emit_ABx(c, OP_IMPORT, (uint8_t)mod_reg, (uint16_t)mod_k);

        if (decl->data.import_decl.import_all) {
            /* from X import * — store the module object itself */
            emit_ABx(c, OP_SETGLOBAL, (uint8_t)mod_reg, (uint16_t)mod_k);
        } else if (decl->data.import_decl.items && decl->data.import_decl.item_count > 0) {
            /* from X import a, b */
            for (int i = 0; i < decl->data.import_decl.item_count; i++) {
                int item_reg = alloc_reg(c);
                int item_k = chunk_add_string(c->chunk, decl->data.import_decl.items[i]);
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
        .parent      = c,
        .upvalue_count = 0
    };

    scope_enter(&sub);
    emit_enter(&sub, (uint16_t)param_count);

    for (int i = 0; i < param_count; i++) {
        add_local(&sub, params[i].name, i);
    }
    sub.temp_base = param_count;

    /* Emit default value prologues */
    for (int i = 0; i < param_count; i++) {
        if (params[i].default_value) {
            /* Emit OP_DEFAULT i, 0 (placeholder) */
            int placeholder = chunk_emit_AsBx(sub.chunk, sub.line, OP_DEFAULT, (uint8_t)i, 0);
            /* Compile default expression into register i */
            compile_expr_into(&sub, params[i].default_value, i);
            /* Patch placeholder with skip offset */
            int skip = sub.chunk->count - placeholder - 1;
            chunk_patch_sBx(sub.chunk, placeholder, skip);
        }
    }
    /* Emit kwargs remapping (populates params from kwargs dict) */
    emit_ABC(&sub, OP_KWARGS, 0, 0, 0);

    for (int i = 0; i < body_count; i++) {
        compile_stmt(&sub, body[i]);
    }

    emit_loadnull(&sub, 0);
    emit_ret(&sub, 0);

    scope_exit(&sub);

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
        int k = chunk_add_string(c->chunk, name);
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
            retain_obj((Object*)parent);

            /* Copy parent's prototype fields to new prototype */
            if (parent->prototype) {
                ObjInstance *pp = parent->prototype;
                cls->prototype = new_instance(cls, pp->field_capacity > 4 ? pp->field_capacity : 4);
                for (int i = 0; i < pp->field_count; i++) {
                    /* Already cloned by new_instance since klass->prototype was set */
                }
            }

            /* Copy parent methods */
            if (parent->method_count > 0) {
                cls->methods = malloc(sizeof(ObjFunction*) * parent->method_count);
                cls->method_names = malloc(sizeof(char*) * parent->method_count);
                memcpy(cls->methods, parent->methods, sizeof(ObjFunction*) * parent->method_count);
                for (int i = 0; i < parent->method_count; i++) {
                    cls->method_names[i] = strdup(parent->method_names[i]);
                    if (cls->methods[i]) retain_obj((Object*)cls->methods[i]);
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

    /* Fields */
    for (int i = 0; i < decl->data.class_decl.field_count; i++) {
        instance_set_field(cls->prototype, decl->data.class_decl.fields[i].name, make_null());
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
            .loop        = NULL
        };
        scope_enter(&sub);
        emit_enter(&sub, (uint16_t)(m->data.function.param_count + 1));
        /* self in reg 0, params in 1..n */
        add_local(&sub, "self", 0);
        for (int j = 0; j < m->data.function.param_count; j++) {
            add_local(&sub, m->data.function.params[j].name, j + 1);
        }
        sub.temp_base = m->data.function.param_count + 1;
        /* Emit default value prologues for method params (reg j+1 because self is reg 0) */
        for (int j = 0; j < m->data.function.param_count; j++) {
            if (m->data.function.params[j].default_value) {
                int placeholder = chunk_emit_AsBx(sub.chunk, sub.line, OP_DEFAULT, (uint8_t)(j + 1), 0);
                compile_expr_into(&sub, m->data.function.params[j].default_value, j + 1);
                int skip = sub.chunk->count - placeholder - 1;
                chunk_patch_sBx(sub.chunk, placeholder, skip);
            }
        }
        emit_ABC(&sub, OP_KWARGS, 0, 0, 0);

        for (int j = 0; j < m->data.function.body_count; j++)
            compile_stmt(&sub, m->data.function.body[j]);
        emit_loadnull(&sub, 0);
        emit_ret(&sub, 0);
        scope_exit(&sub);

        mchunk.max_registers = sub.max_temp_base;
        ObjFunction *mf = new_function(m->data.function.name);
        mf->chunk       = malloc(sizeof(Chunk));
        *mf->chunk      = mchunk;
        mf->param_count = m->data.function.param_count + 1; /* +self */
        mf->param_names = malloc(sizeof(char*) * mf->param_count);
        mf->param_name_objs = malloc(sizeof(ObjString*) * mf->param_count);
        mf->param_names[0] = strdup("self");
        mf->param_name_objs[0] = new_string("self", 4);
        for (int j = 0; j < m->data.function.param_count; j++) {
            mf->param_names[j + 1] = strdup(m->data.function.params[j].name);
            mf->param_name_objs[j + 1] = new_string(
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
        retain_obj((Object*)mf);
        cls->method_count++;
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
                int k = chunk_add_string(c.chunk, "_");
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
