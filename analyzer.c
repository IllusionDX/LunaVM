/* Semantic Analyzer implementation for Luna programming language.
 * Performs type checking, scope management, and optimizations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "analyzer.h"
#include "ast.h"
#include "value.h"

#define INITIAL_SCOPE_CAPACITY 4
#define INITIAL_SYMBOL_CAPACITY 16

static Type *make_type_base(const char *name) {
    Type *type = (Type *)malloc(sizeof(Type));
    if (!type) return NULL;
    type->kind = TYPE_BASE;
    type->data.base.name = strdup(name);
    return type;
}

typedef struct Symbol {
    char *name;
    Type *type;
    int scope_level;
    bool is_const;
    bool is_inferred;
    struct Symbol *next;
} Symbol;

typedef struct Scope {
    int level;
    Symbol *symbols;
    struct Scope *enclosing;
} Scope;

struct Analyzer {
    Scope *current_scope;
    int scope_count;
    AnalyzerResult result;
    char error_message[512];
    int error_line;
    char error_file[256];
    Decl **decls;
    int decl_count;
};

static void scope_enter(Analyzer *analyzer);
static void scope_exit(Analyzer *analyzer);
static Symbol *scope_lookup(Analyzer *analyzer, const char *name);
static Symbol *scope_lookup_current(Analyzer *analyzer, const char *name);
static void scope_define(Analyzer *analyzer, const char *name, Type *type, bool is_const, bool is_inferred);
static void analyze_declaration(Analyzer *analyzer, Decl *decl);
static void analyze_statement(Analyzer *analyzer, Stmt *stmt);
static void analyze_expression(Analyzer *analyzer, Expr *expr);
static void validate_assignment(Analyzer *analyzer, Expr *target, Type *value_type);
static bool types_compatible(Type *expected, Type *actual);
static void map_struct_fields(Decl *decl);
static int get_field_index(Decl *decl, const char *field_name);
static void mark_retain_release(Analyzer *analyzer, Stmt **body, int body_count);

Type *duplicate_type(Type *type) {
    if (!type) return NULL;

    Type *dup = (Type *)malloc(sizeof(Type));
    if (!dup) return NULL;
    dup->kind = type->kind;

    switch (type->kind) {
        case TYPE_BASE:
            dup->data.base.name = strdup(type->data.base.name);
            break;
        case TYPE_ARRAY:
            dup->data.array.element_type = duplicate_type(type->data.array.element_type);
            dup->data.array.size = type->data.array.size;
            dup->data.array.has_size = type->data.array.has_size;
            break;
        case TYPE_LIST:
            dup->data.list.element_type = duplicate_type(type->data.list.element_type);
            break;
        case TYPE_MAP:
            dup->data.map.key_type = duplicate_type(type->data.map.key_type);
            dup->data.map.value_type = duplicate_type(type->data.map.value_type);
            break;
        case TYPE_GENERIC:
            dup->data.generic.base = strdup(type->data.generic.base);
            dup->data.generic.type_arg_count = type->data.generic.type_arg_count;
            if (type->data.generic.type_args && type->data.generic.type_arg_count > 0) {
                dup->data.generic.type_args = (Type **)malloc(sizeof(Type *) * type->data.generic.type_arg_count);
                for (int i = 0; i < type->data.generic.type_arg_count; i++) {
                    dup->data.generic.type_args[i] = duplicate_type(type->data.generic.type_args[i]);
                }
            }
            break;
    }
    return dup;
}

bool types_equal(Type *a, Type *b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        case TYPE_BASE:
            return strcmp(a->data.base.name, b->data.base.name) == 0;
        case TYPE_ARRAY:
            return types_equal(a->data.array.element_type, b->data.array.element_type);
        case TYPE_LIST:
            return types_equal(a->data.list.element_type, b->data.list.element_type);
        case TYPE_MAP:
            return types_equal(a->data.map.key_type, b->data.map.key_type) &&
                   types_equal(a->data.map.value_type, b->data.map.value_type);
        case TYPE_GENERIC:
            if (strcmp(a->data.generic.base, b->data.generic.base) != 0) return false;
            if (a->data.generic.type_arg_count != b->data.generic.type_arg_count) return false;
            for (int i = 0; i < a->data.generic.type_arg_count; i++) {
                if (!types_equal(a->data.generic.type_args[i], b->data.generic.type_args[i])) {
                    return false;
                }
            }
            return true;
        default:
            return false;
    }
}

bool is_object_type(Type *type) {
    if (!type) return false;
    if (type->kind == TYPE_BASE) {
        const char *name = type->data.base.name;
        return strcmp(name, "string") == 0 ||
               strcmp(name, "list") == 0 ||
               strcmp(name, "map") == 0 ||
               strcmp(name, "object") == 0 ||
               type->kind == TYPE_LIST ||
               type->kind == TYPE_MAP;
    }
    return type->kind == TYPE_LIST || type->kind == TYPE_MAP;
}

bool is_primitive_type(Type *type) {
    if (!type) return false;
    if (type->kind == TYPE_BASE) {
        const char *name = type->data.base.name;
        return strcmp(name, "int") == 0 ||
               strcmp(name, "float") == 0 ||
               strcmp(name, "double") == 0 ||
               strcmp(name, "bool") == 0 ||
               strcmp(name, "char") == 0 ||
               strcmp(name, "byte") == 0 ||
               strcmp(name, "long") == 0 ||
               strcmp(name, "null") == 0;
    }
    return false;
}

int get_type_size(Type *type) {
    if (!type) return sizeof(void *);
    if (type->kind == TYPE_BASE) {
        const char *name = type->data.base.name;
        if (strcmp(name, "int") == 0) return sizeof(int32_t);
        if (strcmp(name, "long") == 0) return sizeof(int64_t);
        if (strcmp(name, "float") == 0) return sizeof(float);
        if (strcmp(name, "double") == 0) return sizeof(double);
        if (strcmp(name, "bool") == 0) return sizeof(bool);
        if (strcmp(name, "char") == 0) return sizeof(char);
        if (strcmp(name, "byte") == 0) return sizeof(uint8_t);
    }
    return sizeof(void *);
}

static void set_error(Analyzer *analyzer, AnalyzerResult result, const char *message, int line, const char *file) {
    analyzer->result = result;
    strncpy(analyzer->error_message, message, sizeof(analyzer->error_message) - 1);
    analyzer->error_line = line;
    strncpy(analyzer->error_file, file ? file : "<unknown>", sizeof(analyzer->error_file) - 1);
}

static void define_builtins(Analyzer *analyzer) {
    const char *builtins[] = {"print", "len", "str", "int", "float", "bool", "type", "input", "read", "write"};
    for (int i = 0; i < 10; i++) {
        Type *t = make_type_base("function");
        scope_define(analyzer, builtins[i], t, false, false);
    }
}

Analyzer *analyzer_new(void) {
    Analyzer *analyzer = (Analyzer *)malloc(sizeof(Analyzer));
    if (!analyzer) return NULL;

    analyzer->current_scope = (Scope *)malloc(sizeof(Scope));
    if (!analyzer->current_scope) {
        free(analyzer);
        return NULL;
    }

    analyzer->current_scope->level = 0;
    analyzer->current_scope->symbols = NULL;
    analyzer->current_scope->enclosing = NULL;
    analyzer->scope_count = 1;
    analyzer->result = ANALYZER_OK;
    analyzer->error_message[0] = '\0';
    analyzer->error_line = 0;
    analyzer->error_file[0] = '\0';
    analyzer->decls = NULL;
    analyzer->decl_count = 0;

    return analyzer;
}

void analyzer_free(Analyzer *analyzer) {
    if (!analyzer) return;

    while (analyzer->current_scope) {
        Scope *scope = analyzer->current_scope;
        Symbol *sym = scope->symbols;
        while (sym) {
            Symbol *next = sym->next;
            free(sym->name);
            if (sym->type) free_type(sym->type);
            free(sym);
            sym = next;
        }
        analyzer->current_scope = scope->enclosing;
        free(scope);
    }

    if (analyzer->decls) free(analyzer->decls);
    free(analyzer);
}

const char *analyzer_get_error(Analyzer *analyzer) {
    return analyzer->error_message;
}

int analyzer_get_error_line(Analyzer *analyzer) {
    return analyzer->error_line;
}

const char *analyzer_get_error_file(Analyzer *analyzer) {
    return analyzer->error_file;
}

static void scope_enter(Analyzer *analyzer) {
    Scope *new_scope = (Scope *)malloc(sizeof(Scope));
    if (!new_scope) return;

    new_scope->level = analyzer->scope_count++;
    new_scope->symbols = NULL;
    new_scope->enclosing = analyzer->current_scope;
    analyzer->current_scope = new_scope;
}

static void scope_exit(Analyzer *analyzer) {
    if (!analyzer->current_scope || !analyzer->current_scope->enclosing) return;

    Scope *scope = analyzer->current_scope;
    Symbol *sym = scope->symbols;
    while (sym) {
        Symbol *next = sym->next;
        free(sym->name);
        if (sym->type) free_type(sym->type);
        free(sym);
        sym = next;
    }

    analyzer->current_scope = scope->enclosing;
    free(scope);
}

static Symbol *scope_lookup(Analyzer *analyzer, const char *name) {
    Scope *scope = analyzer->current_scope;
    while (scope) {
        Symbol *sym = scope->symbols;
        while (sym) {
            if (strcmp(sym->name, name) == 0) {
                return sym;
            }
            sym = sym->next;
        }
        scope = scope->enclosing;
    }
    return NULL;
}

static Symbol *scope_lookup_current(Analyzer *analyzer, const char *name) {
    Symbol *sym = analyzer->current_scope->symbols;
    while (sym) {
        if (strcmp(sym->name, name) == 0) {
            return sym;
        }
        sym = sym->next;
    }
    return NULL;
}

static void scope_define(Analyzer *analyzer, const char *name, Type *type, bool is_const, bool is_inferred) {
    if (scope_lookup_current(analyzer, name)) {
        return;
    }

    Symbol *sym = (Symbol *)malloc(sizeof(Symbol));
    if (!sym) return;

    sym->name = strdup(name);
    sym->type = type ? duplicate_type(type) : NULL;
    sym->scope_level = analyzer->current_scope->level;
    sym->is_const = is_const;
    sym->is_inferred = is_inferred;
    sym->next = analyzer->current_scope->symbols;
    analyzer->current_scope->symbols = sym;
}

static int get_field_index(Decl *decl, const char *field_name) {
    if (!decl) return -1;

    int count = 0;
    StructField *fields = NULL;

    if (decl->kind == DECL_STRUCT) {
        fields = decl->data.struct_decl.fields;
        count = decl->data.struct_decl.field_count;
    } else if (decl->kind == DECL_CLASS) {
        fields = decl->data.class_decl.fields;
        count = decl->data.class_decl.field_count;
    }

    for (int i = 0; i < count; i++) {
        if (fields && strcmp(fields[i].name, field_name) == 0) {
            return i;
        }
    }
    return -1;
}

static void map_struct_fields(Decl *decl) {
    if (!decl) return;

    int field_count = 0;
    StructField *fields = NULL;

    if (decl->kind == DECL_STRUCT) {
        field_count = decl->data.struct_decl.field_count;
        fields = decl->data.struct_decl.fields;
    } else if (decl->kind == DECL_CLASS) {
        field_count = decl->data.class_decl.field_count;
        fields = decl->data.class_decl.fields;
    } else {
        return;
    }

    if (field_count <= 0) return;

    decl->field_offsets = (int *)malloc(sizeof(int) * field_count);
    if (!decl->field_offsets) return;

    int offset = 0;
    for (int i = 0; i < field_count; i++) {
        decl->field_offsets[i] = offset;
        offset += get_type_size(fields[i].field_type);
    }
}

static bool types_compatible(Type *expected, Type *actual) {
    if (!expected || !actual) return false;
    if (types_equal(expected, actual)) return true;
    if (expected->kind == TYPE_BASE &&
        strcmp(expected->data.base.name, "null") == 0) {
        return true;
    }
    if (actual->kind == TYPE_BASE &&
        strcmp(actual->data.base.name, "null") == 0) {
        return true;
    }
    return false;
}

static void validate_assignment(Analyzer *analyzer, Expr *target, Type *value_type) {
    if (!target || target->kind != EXPR_IDENTIFIER) return;

    Symbol *sym = scope_lookup(analyzer, target->data.identifier.name);
    if (!sym) {
        set_error(analyzer, ANALYZER_ERROR_UNDEFINED_IDENTIFIER,
                 "Undefined identifier", 0, NULL);
        return;
    }

    if (sym->is_const) {
        set_error(analyzer, ANALYZER_ERROR_TYPE_MISMATCH,
                 "Cannot assign to constant", 0, NULL);
        return;
    }

    if (sym->type && value_type && !types_compatible(sym->type, value_type)) {
        set_error(analyzer, ANALYZER_ERROR_TYPE_MISMATCH,
                 "Type mismatch in assignment", 0, NULL);
    }

    if (sym->is_inferred && sym->type && value_type &&
        !types_equal(sym->type, value_type)) {
        set_error(analyzer, ANALYZER_ERROR_TYPE_MISMATCH,
                 "Type mismatch: cannot reassign inferred type", 0, NULL);
    }
}

static void analyze_expression(Analyzer *analyzer, Expr *expr) {
    if (!expr) return;

    switch (expr->kind) {
        case EXPR_IDENTIFIER: {
            Symbol *sym = scope_lookup(analyzer, expr->data.identifier.name);
            if (!sym) {
                set_error(analyzer, ANALYZER_ERROR_UNDEFINED_IDENTIFIER,
                         "Undefined identifier", 0, NULL);
            } else if (sym->type) {
                expr->inferred_type = duplicate_type(sym->type);
                expr->needs_retain = is_object_type(sym->type);
            }
            break;
        }
        case EXPR_BINARY: {
            analyze_expression(analyzer, expr->data.binary.left);
            analyze_expression(analyzer, expr->data.binary.right);

            Type *left_type = expr->data.binary.left->inferred_type;
            Type *right_type = expr->data.binary.right->inferred_type;

            const char *op = expr->data.binary.operator;
            if (strcmp(op, ">") == 0 || strcmp(op, "<") == 0 ||
                strcmp(op, ">=") == 0 || strcmp(op, "<=") == 0 ||
                strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
                expr->inferred_type = make_type_base("bool");
            } else if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                     strcmp(op, "*") == 0 || strcmp(op, "/") == 0) {
                if (left_type && right_type) {
                    if (!types_compatible(left_type, right_type) &&
                        strcmp(op, "+") != 0) {
                        set_error(analyzer, ANALYZER_ERROR_INVALID_OPERATION,
                                 "Invalid binary operation", 0, NULL);
                    }
                    expr->inferred_type = duplicate_type(left_type);
                }
            }

            if (is_object_type(left_type) || is_object_type(right_type)) {
                if (left_type) expr->needs_retain = true;
                if (right_type) expr->needs_retain = true;
            }
            break;
        }
        case EXPR_CALL: {
            analyze_expression(analyzer, expr->data.call.callee);
            for (int i = 0; i < expr->data.call.arg_count; i++) {
                analyze_expression(analyzer, expr->data.call.arguments[i]);
            }

            if (expr->data.call.callee->kind == EXPR_IDENTIFIER) {
                const char *func_name = expr->data.call.callee->data.identifier.name;
                Symbol *sym = scope_lookup(analyzer, func_name);

                if (sym && sym->type) {
                    expr->inferred_type = duplicate_type(sym->type);
                }
            } else if (expr->data.call.callee->kind == EXPR_FIELD_ACCESS) {
                expr->inferred_type = make_type_base("object");
            }
            break;
        }
        case EXPR_FIELD_ACCESS: {
            analyze_expression(analyzer, expr->data.field_access.obj);
            if (expr->data.field_access.obj->inferred_type &&
                expr->data.field_access.obj->inferred_type->kind == TYPE_BASE) {
                const char *type_name = expr->data.field_access.obj->inferred_type->data.base.name;
                for (int i = 0; i < analyzer->decl_count; i++) {
                    Decl *decl = analyzer->decls[i];
                    const char *decl_name = NULL;
                    if (decl->kind == DECL_STRUCT) {
                        decl_name = decl->data.struct_decl.name;
                    } else if (decl->kind == DECL_CLASS) {
                        decl_name = decl->data.class_decl.name;
                    }
                    if (decl_name && strcmp(decl_name, type_name) == 0) {
                        int field_idx = get_field_index(decl, expr->data.field_access.field);
                        if (field_idx >= 0) {
                            expr->data.field_access.field_index = field_idx;
                            if (decl->field_offsets && field_idx < (decl->kind == DECL_STRUCT ?
                                decl->data.struct_decl.field_count : decl->data.class_decl.field_count)) {
                                expr->data.field_access.field_offset = decl->field_offsets[field_idx];
                            }
                            StructField *fields = (decl->kind == DECL_STRUCT) ?
                                decl->data.struct_decl.fields : decl->data.class_decl.fields;
                            if (fields && fields[field_idx].field_type) {
                                expr->inferred_type = duplicate_type(fields[field_idx].field_type);
                                expr->field_offset = expr->data.field_access.field_offset;
                            }
                        }
                    }
                }
            }
            break;
        }
        case EXPR_INDEX_ACCESS: {
            analyze_expression(analyzer, expr->data.index_access.obj);
            analyze_expression(analyzer, expr->data.index_access.index);
            if (expr->data.index_access.obj->inferred_type) {
                if (expr->data.index_access.obj->inferred_type->kind == TYPE_LIST) {
                    expr->inferred_type = duplicate_type(
                        expr->data.index_access.obj->inferred_type->data.list.element_type);
                } else if (expr->data.index_access.obj->inferred_type->kind == TYPE_ARRAY) {
                    expr->inferred_type = duplicate_type(
                        expr->data.index_access.obj->inferred_type->data.array.element_type);
                } else if (expr->data.index_access.obj->inferred_type->kind == TYPE_MAP) {
                    expr->inferred_type = duplicate_type(
                        expr->data.index_access.obj->inferred_type->data.map.value_type);
                }
            }
            break;
        }
        case EXPR_ASSIGNMENT: {
            analyze_expression(analyzer, expr->data.assignment.value);
            Type *value_type = expr->data.assignment.value->inferred_type;
            validate_assignment(analyzer, expr->data.assignment.target, value_type);

            if (expr->data.assignment.target->kind == EXPR_IDENTIFIER) {
                Symbol *sym = scope_lookup(analyzer, expr->data.assignment.target->data.identifier.name);
                if (sym && sym->type && value_type) {
                    expr->needs_retain = is_object_type(value_type);
                }
            }
            break;
        }
        case EXPR_LIST_LITERAL: {
            for (int i = 0; i < expr->data.list_literal.element_count; i++) {
                analyze_expression(analyzer, expr->data.list_literal.elements[i]);
            }
            if (expr->data.list_literal.element_count > 0) {
                Type *first_type = expr->data.list_literal.elements[0]->inferred_type;
                if (first_type) {
                    Type *list_type = (Type *)malloc(sizeof(Type));
                    list_type->kind = TYPE_LIST;
                    list_type->data.list.element_type = first_type;
                    expr->inferred_type = list_type;
                }
            }
            break;
        }
        case EXPR_NEW: {
            for (int i = 0; i < expr->data.new_expr.arg_count; i++) {
                analyze_expression(analyzer, expr->data.new_expr.arguments[i]);
            }
            expr->inferred_type = make_type_base(expr->data.new_expr.class_name);
            expr->needs_retain = true;
            break;
        }
        case EXPR_TERNARY: {
            analyze_expression(analyzer, expr->data.ternary.condition);
            analyze_expression(analyzer, expr->data.ternary.then_expr);
            analyze_expression(analyzer, expr->data.ternary.else_expr);
            break;
        }
        case EXPR_COMPOUND_ASSIGN: {
            analyze_expression(analyzer, expr->data.compound_assign.target);
            analyze_expression(analyzer, expr->data.compound_assign.value);
            break;
        }
        default:
            break;
    }
}

static void analyze_statement(Analyzer *analyzer, Stmt *stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case STMT_VAR_DECL: {
            char *name = stmt->data.var_decl.name;
            Type *explicit_type = stmt->data.var_decl.var_type;

            if (explicit_type) {
                scope_define(analyzer, name, explicit_type,
                         stmt->data.var_decl.is_const, false);
                stmt->inferred_type = duplicate_type(explicit_type);
            } else if (stmt->data.var_decl.initializer) {
                analyze_expression(analyzer, stmt->data.var_decl.initializer);
                Type *inferred = stmt->data.var_decl.initializer->inferred_type;
                if (inferred) {
                    scope_define(analyzer, name, inferred, false, true);
                    stmt->inferred_type = duplicate_type(inferred);
                    stmt->needs_retain = is_object_type(inferred);
                } else {
                    scope_define(analyzer, name, NULL, false, false);
                }
            } else {
                scope_define(analyzer, name, NULL, false, false);
            }
            break;
        }
        case STMT_RETURN: {
            if (stmt->data.return_stmt.value) {
                analyze_expression(analyzer, stmt->data.return_stmt.value);
            }
            break;
        }
        case STMT_EXPRESSION: {
            if (stmt->data.expression.expression) {
                analyze_expression(analyzer, stmt->data.expression.expression);
            }
            break;
        }
        case STMT_IF: {
            if (stmt->data.if_stmt.condition) {
                analyze_expression(analyzer, stmt->data.if_stmt.condition);
            }

            scope_enter(analyzer);
            for (int i = 0; i < stmt->data.if_stmt.then_count; i++) {
                analyze_statement(analyzer, stmt->data.if_stmt.then_body[i]);
            }
            scope_exit(analyzer);

            if (stmt->data.if_stmt.else_body) {
                scope_enter(analyzer);
                for (int i = 0; i < stmt->data.if_stmt.else_count; i++) {
                    analyze_statement(analyzer, stmt->data.if_stmt.else_body[i]);
                }
                scope_exit(analyzer);
            }
            break;
        }
        case STMT_WHILE: {
            if (stmt->data.while_stmt.condition) {
                analyze_expression(analyzer, stmt->data.while_stmt.condition);
            }

            scope_enter(analyzer);
            for (int i = 0; i < stmt->data.while_stmt.body_count; i++) {
                analyze_statement(analyzer, stmt->data.while_stmt.body[i]);
            }
            scope_exit(analyzer);
            break;
        }
        case STMT_FOR: {
            if (stmt->data.for_stmt.iterable) {
                analyze_expression(analyzer, stmt->data.for_stmt.iterable);
            }

            scope_enter(analyzer);
            char *var_name = stmt->data.for_stmt.variable;
            if (var_name) {
                Symbol *sym = scope_lookup(analyzer, var_name);
                if (sym && stmt->data.for_stmt.iterable && stmt->data.for_stmt.iterable->inferred_type) {
                    Type *elem_type = NULL;
                    Type *iter_type = stmt->data.for_stmt.iterable->inferred_type;
                    if (iter_type && iter_type->kind == TYPE_LIST) {
                        elem_type = iter_type->data.list.element_type;
                    } else if (iter_type && iter_type->kind == TYPE_ARRAY) {
                        elem_type = iter_type->data.array.element_type;
                    }
                    if (elem_type) {
                        scope_define(analyzer, var_name, elem_type, false, false);
                    }
                }
            }
            for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
                analyze_statement(analyzer, stmt->data.for_stmt.body[i]);
            }
            scope_exit(analyzer);
            break;
        }
        default:
            break;
    }
}

static void analyze_declaration(Analyzer *analyzer, Decl *decl) {
    if (!decl) return;

    switch (decl->kind) {
        case DECL_FUNCTION: {
            char *name = decl->data.function.name;
            Type *return_type = decl->data.function.return_type;

            scope_define(analyzer, name, return_type, false, false);

            scope_enter(analyzer);
            for (int i = 0; i < decl->data.function.param_count; i++) {
                FunctionParam *param = &decl->data.function.params[i];
                if (param->name && param->param_type) {
                    scope_define(analyzer, param->name, param->param_type, false, false);
                }
            }

            for (int i = 0; i < decl->data.function.body_count; i++) {
                analyze_statement(analyzer, decl->data.function.body[i]);
            }

            mark_retain_release(analyzer, decl->data.function.body, decl->data.function.body_count);

            scope_exit(analyzer);
            break;
        }
        case DECL_STRUCT: {
            map_struct_fields(decl);
            break;
        }
        case DECL_CLASS: {
            map_struct_fields(decl);
            break;
        }
        default:
            break;
    }
}

static void mark_retain_release(Analyzer *analyzer, Stmt **body, int body_count) {
    (void)analyzer;
    for (int i = 0; i < body_count; i++) {
        Stmt *stmt = body[i];
        if (!stmt) continue;

        if (stmt->kind == STMT_IF) {
            mark_retain_release(analyzer, stmt->data.if_stmt.then_body, stmt->data.if_stmt.then_count);
            if (stmt->data.if_stmt.else_body) {
                mark_retain_release(analyzer, stmt->data.if_stmt.else_body, stmt->data.if_stmt.else_count);
            }
        } else if (stmt->kind == STMT_WHILE) {
            mark_retain_release(analyzer, stmt->data.while_stmt.body, stmt->data.while_stmt.body_count);
        } else if (stmt->kind == STMT_FOR) {
            mark_retain_release(analyzer, stmt->data.for_stmt.body, stmt->data.for_stmt.body_count);
        }
    }
}

AnalyzerResult analyze_program(Analyzer *analyzer, Program *program) {
    if (!analyzer || !program) {
        return ANALYZER_OK;
    }

    define_builtins(analyzer);

    analyzer->decls = program->declarations;
    analyzer->decl_count = program->decl_count;

    for (int i = 0; i < program->decl_count; i++) {
        Decl *decl = program->declarations[i];
        if (decl->kind == DECL_STRUCT || decl->kind == DECL_CLASS) {
            analyze_declaration(analyzer, decl);
        }
    }

    for (int i = 0; i < program->decl_count; i++) {
        Decl *decl = program->declarations[i];
        if (decl->kind == DECL_FUNCTION) {
            char *name = decl->data.function.name;
            Type *func_type = (Type *)malloc(sizeof(Type));
            func_type->kind = TYPE_BASE;
            func_type->data.base.name = strdup("function");
            scope_define(analyzer, name, func_type, false, false);
        }
    }

    for (int i = 0; i < program->decl_count; i++) {
        Decl *decl = program->declarations[i];
        if (decl->kind == DECL_FUNCTION) {
            if (analyzer->result != ANALYZER_OK) break;
            analyze_declaration(analyzer, decl);
        }
    }

    for (int i = 0; i < program->stmt_count; i++) {
        Stmt *stmt = program->statements[i];
        if (analyzer->result != ANALYZER_OK) break;
        analyze_statement(analyzer, stmt);
    }

    return analyzer->result;
}