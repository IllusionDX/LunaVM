/* AST memory management implementation.
 * Frees all AST nodes and associated memory.
 * Dynamic VM variant — no Type nodes to free.
 */

#include <stdlib.h>
#include "ast.h"

/* ============== Expression freeing ============== */

void free_expr(Expr *expr) {
    if (!expr) return;

    switch (expr->kind) {
    case EXPR_INTEGER:
        free(expr->data.integer.value);
        break;

    case EXPR_FLOAT:
        free(expr->data.float_lit.value);
        break;

    case EXPR_STRING:
        free(expr->data.string.value);
        break;

    case EXPR_IDENTIFIER:
        free(expr->data.identifier.name);
        break;

    case EXPR_BINARY:
        free_expr(expr->data.binary.left);
        free_expr(expr->data.binary.right);
        free(expr->data.binary.operator);
        break;

    case EXPR_UNARY:
        free_expr(expr->data.unary.operand);
        free(expr->data.unary.operator);
        break;

    case EXPR_CALL:
        free_expr(expr->data.call.callee);
        for (int i = 0; i < expr->data.call.arg_count; i++) {
            free_expr(expr->data.call.arguments[i]);
        }
        free(expr->data.call.arguments);
        break;

    case EXPR_FIELD_ACCESS:
        free_expr(expr->data.field_access.obj);
        free(expr->data.field_access.field);
        break;

    case EXPR_INDEX_ACCESS:
        free_expr(expr->data.index_access.obj);
        free_expr(expr->data.index_access.index);
        break;

    case EXPR_ASSIGNMENT:
        free_expr(expr->data.assignment.target);
        free_expr(expr->data.assignment.value);
        break;

    case EXPR_COMPOUND_ASSIGN:
        free_expr(expr->data.compound_assign.target);
        free(expr->data.compound_assign.operator);
        free_expr(expr->data.compound_assign.value);
        break;

    case EXPR_TERNARY:
        free_expr(expr->data.ternary.condition);
        free_expr(expr->data.ternary.then_expr);
        free_expr(expr->data.ternary.else_expr);
        break;

    case EXPR_LIST_LITERAL:
        for (int i = 0; i < expr->data.list_literal.element_count; i++) {
            free_expr(expr->data.list_literal.elements[i]);
        }
        free(expr->data.list_literal.elements);
        break;

    case EXPR_DICT_LITERAL:
        for (int i = 0; i < expr->data.dict_literal.entry_count; i++) {
            free_expr(expr->data.dict_literal.entries[i].key);
            free_expr(expr->data.dict_literal.entries[i].value);
        }
        free(expr->data.dict_literal.entries);
        break;

    case EXPR_NEW:
        free(expr->data.new_expr.class_name);
        for (int i = 0; i < expr->data.new_expr.arg_count; i++) {
            free_expr(expr->data.new_expr.arguments[i]);
        }
        free(expr->data.new_expr.arguments);
        break;

    case EXPR_FUNCTION:
        if (expr->data.function.name) free(expr->data.function.name);
        for (int i = 0; i < expr->data.function.param_count; i++) {
            free(expr->data.function.params[i].name);
        }
        free(expr->data.function.params);
        for (int i = 0; i < expr->data.function.body_count; i++) {
            free_stmt(expr->data.function.body[i]);
        }
        free(expr->data.function.body);
        break;

    default:
        break;
    }

    free(expr);
}

/* ============== Statement freeing ============== */

void free_stmt(Stmt *stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
    case STMT_EXPRESSION:
        free_expr(stmt->data.expression.expression);
        break;

    case STMT_VAR_DECL:
        free(stmt->data.var_decl.name);
        if (stmt->data.var_decl.initializer) {
            free_expr(stmt->data.var_decl.initializer);
        }
        break;

    case STMT_DECLARATION:
        free_decl(stmt->data.declaration.decl);
        break;

    case STMT_RETURN:
        if (stmt->data.return_stmt.value) {
            free_expr(stmt->data.return_stmt.value);
        }
        break;

    case STMT_IF:
        free_expr(stmt->data.if_stmt.condition);
        for (int i = 0; i < stmt->data.if_stmt.then_count; i++) {
            free_stmt(stmt->data.if_stmt.then_body[i]);
        }
        free(stmt->data.if_stmt.then_body);
        for (int i = 0; i < stmt->data.if_stmt.else_count; i++) {
            free_stmt(stmt->data.if_stmt.else_body[i]);
        }
        free(stmt->data.if_stmt.else_body);
        break;

    case STMT_WHILE:
        free_expr(stmt->data.while_stmt.condition);
        for (int i = 0; i < stmt->data.while_stmt.body_count; i++) {
            free_stmt(stmt->data.while_stmt.body[i]);
        }
        free(stmt->data.while_stmt.body);
        break;

    case STMT_FOR:
        free(stmt->data.for_stmt.variable);
        free_expr(stmt->data.for_stmt.iterable);
        for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
            free_stmt(stmt->data.for_stmt.body[i]);
        }
        free(stmt->data.for_stmt.body);
        break;

    case STMT_SWITCH:
        free_expr(stmt->data.switch_stmt.expression);
        for (int i = 0; i < stmt->data.switch_stmt.case_count; i++) {
            if (stmt->data.switch_stmt.cases[i].value) {
                free_expr(stmt->data.switch_stmt.cases[i].value);
            }
            for (int j = 0; j < stmt->data.switch_stmt.cases[i].body_count; j++) {
                free_stmt(stmt->data.switch_stmt.cases[i].body[j]);
            }
            free(stmt->data.switch_stmt.cases[i].body);
        }
        free(stmt->data.switch_stmt.cases);
        break;

    case STMT_THROW:
        free_expr(stmt->data.throw_stmt.expression);
        break;

    case STMT_TRY:
        for (int i = 0; i < stmt->data.try_stmt.try_count; i++) {
            free_stmt(stmt->data.try_stmt.try_body[i]);
        }
        free(stmt->data.try_stmt.try_body);

        for (int i = 0; i < stmt->data.try_stmt.catch_count; i++) {
            free(stmt->data.try_stmt.catch_clauses[i].variable);
            for (int j = 0; j < stmt->data.try_stmt.catch_clauses[i].body_count; j++) {
                free_stmt(stmt->data.try_stmt.catch_clauses[i].body[j]);
            }
            free(stmt->data.try_stmt.catch_clauses[i].body);
        }
        free(stmt->data.try_stmt.catch_clauses);

        if (stmt->data.try_stmt.finally_body) {
            for (int i = 0; i < stmt->data.try_stmt.finally_count; i++) {
                free_stmt(stmt->data.try_stmt.finally_body[i]);
            }
            free(stmt->data.try_stmt.finally_body);
        }
        break;

    default:
        break;
    }

    free(stmt);
}

/* ============== Declaration freeing ============== */

void free_decl(Decl *decl) {
    if (!decl) return;

    switch (decl->kind) {
    case DECL_FUNCTION:
        free(decl->data.function.name);
        for (int i = 0; i < decl->data.function.param_count; i++) {
            free(decl->data.function.params[i].name);
        }
        free(decl->data.function.params);
        for (int i = 0; i < decl->data.function.body_count; i++) {
            free_stmt(decl->data.function.body[i]);
        }
        free(decl->data.function.body);
        break;

    case DECL_CLASS:
        free(decl->data.class_decl.name);
        if (decl->data.class_decl.base_class) {
            free(decl->data.class_decl.base_class);
        }
        for (int i = 0; i < decl->data.class_decl.field_count; i++) {
            free(decl->data.class_decl.fields[i].name);
        }
        free(decl->data.class_decl.fields);
        for (int i = 0; i < decl->data.class_decl.method_count; i++) {
            free_decl(decl->data.class_decl.methods[i]);
        }
        free(decl->data.class_decl.methods);
        break;

    case DECL_ENUM:
        free(decl->data.enum_decl.name);
        for (int i = 0; i < decl->data.enum_decl.variant_count; i++) {
            free(decl->data.enum_decl.variants[i].name);
        }
        free(decl->data.enum_decl.variants);
        break;

    case DECL_IMPORT:
        free(decl->data.import_decl.module_name);
        for (int i = 0; i < decl->data.import_decl.item_count; i++) {
            free(decl->data.import_decl.items[i]);
        }
        free(decl->data.import_decl.items);
        break;
    }

    free(decl);
}

/* ============== Program freeing ============== */

void free_program(Program *program) {
    if (!program) return;

    for (int i = 0; i < program->decl_count; i++) {
        free_decl(program->declarations[i]);
    }
    free(program->declarations);

    for (int i = 0; i < program->stmt_count; i++) {
        free_stmt(program->statements[i]);
    }
    free(program->statements);

    free(program);
}
