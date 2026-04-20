/* Native C Parser for Luna Programming Language.
 * Converts tokens into an Abstract Syntax Tree.
 * Mirrors the Python parser in parser.py exactly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

/* ============== Forward declarations ============== */

static Decl *parse_declaration(Parser *parser);
static Decl *parse_function_declaration(Parser *parser);
static Stmt *parse_statement(Parser *parser);
static Stmt **parse_block(Parser *parser, int *count);
static Expr *parse_expression(Parser *parser);
static Expr *parse_assignment(Parser *parser);
static Type *parse_type(Parser *parser);
static Expr *parse_primary(Parser *parser);
static Expr *parse_postfix(Parser *parser);

/* ============== Helper functions ============== */

static Token *peek(Parser *parser) {
    return parser->current;
}

static Token *advance(Parser *parser) {
    Token *prev = parser->current;
    if (parser->current->type != TOK_EOF) {
        parser->current = parser->current->next;
    }
    return prev;
}

static bool match(Parser *parser, TokenType type) {
    return peek(parser)->type == type;
}

static bool match2(Parser *parser, TokenType t1, TokenType t2) {
    return peek(parser)->type == t1 || peek(parser)->type == t2;
}

static Token *expect(Parser *parser, TokenType type, const char *msg) {
    if (!match(parser, type)) {
        fprintf(stderr, "Parse error at line %d col %d: %s (got %s)\n",
                peek(parser)->line, peek(parser)->column, msg,
                token_type_name(peek(parser)->type));
        parser->had_error = 1;
        return peek(parser); /* don't consume, let caller handle */
    }
    return advance(parser);
}

static void skip_newlines(Parser *parser) {
    while (match(parser, TOK_NEWLINE)) {
        advance(parser);
    }
}

/* Skip NEWLINE, INDENT, DEDENT — used inside collection literals */
static void skip_whitespace_in_literal(Parser *parser) {
    while (match(parser, TOK_NEWLINE) || match(parser, TOK_INDENT) || match(parser, TOK_DEDENT)) {
        advance(parser);
    }
}

static void expect_newline(Parser *parser) {
    if (match(parser, TOK_NEWLINE)) {
        advance(parser);
    }
}

/* ============== Memory allocation helpers ============== */

static Type *make_type_base(const char *name) {
    Type *type = (Type *)malloc(sizeof(Type));
    if (!type) return NULL;
    type->kind = TYPE_BASE;
    type->data.base.name = strdup(name);
    return type;
}

static Expr *make_expr(ExprKind kind) {
    Expr *expr = (Expr *)calloc(1, sizeof(Expr));
    if (!expr) return NULL;
    expr->kind = kind;
    return expr;
}

static Stmt *make_stmt(StmtKind kind) {
    Stmt *stmt = (Stmt *)calloc(1, sizeof(Stmt));
    if (!stmt) return NULL;
    stmt->kind = kind;
    return stmt;
}

static Decl *make_decl(DeclKind kind) {
    Decl *decl = (Decl *)calloc(1, sizeof(Decl));
    if (!decl) return NULL;
    decl->kind = kind;
    return decl;
}

/* ============== Type parsing ============== */

static bool is_type_token(TokenType t) {
    return (t == TOK_IDENTIFIER ||
            (t >= TOK_VOID_TYPE && t <= TOK_MAP_TYPE));
}

static char *parse_type_base_name(Parser *parser) {
    Token *tok = peek(parser);
    if (tok->type == TOK_IDENTIFIER || (tok->type >= TOK_VOID_TYPE && tok->type <= TOK_MAP_TYPE)) {
        advance(parser);
        return strdup(tok->value);
    }
    fprintf(stderr, "Parse error at line %d: Expected type name, got %s\n",
            tok->line, token_type_name(tok->type));
    parser->had_error = 1;
    return strdup("error");
}

static Type **parse_type_arguments(Parser *parser, int *count) {
    int capacity = 4;
    Type **args = (Type **)malloc(capacity * sizeof(Type *));
    *count = 0;

    if (!match(parser, TOK_GT)) {
        args[(*count)++] = parse_type(parser);
        while (match(parser, TOK_COMMA)) {
            advance(parser);
            if (*count >= capacity) { capacity *= 2; args = realloc(args, capacity * sizeof(Type *)); }
            args[(*count)++] = parse_type(parser);
        }
    }
    return args;
}

static Type *parse_type(Parser *parser) {
    char *base_name = parse_type_base_name(parser);

    /* Generic: list<int>, map<string, int>, MyClass<T> */
    if (match(parser, TOK_LT)) {
        advance(parser); /* consume < */
        int arg_count = 0;
        Type **type_args = parse_type_arguments(parser, &arg_count);
        expect(parser, TOK_GT, "Expected '>' after type arguments");

        /* list<T> -> TYPE_LIST */
        if (strcmp(base_name, "list") == 0 && arg_count == 1) {
            Type *t = (Type *)calloc(1, sizeof(Type));
            t->kind = TYPE_LIST;
            t->data.list.element_type = type_args[0];
            free(type_args); free(base_name);
            return t;
        }
        /* map<K,V> -> TYPE_MAP */
        if (strcmp(base_name, "map") == 0 && arg_count == 2) {
            Type *t = (Type *)calloc(1, sizeof(Type));
            t->kind = TYPE_MAP;
            t->data.map.key_type = type_args[0];
            t->data.map.value_type = type_args[1];
            free(type_args); free(base_name);
            return t;
        }
        /* User-defined generic */
        Type *t = (Type *)calloc(1, sizeof(Type));
        t->kind = TYPE_GENERIC;
        t->data.generic.base = base_name;
        t->data.generic.type_args = type_args;
        t->data.generic.type_arg_count = arg_count;
        return t;
    }

    /* Array: Type[] or Type[size] */
    if (match(parser, TOK_LBRACKET)) {
        advance(parser);
        int size = 0;
        bool has_size = false;
        if (!match(parser, TOK_RBRACKET)) {
            Token *size_tok = expect(parser, TOK_INTEGER_LITERAL, "Expected array size or ']'");
            if (size_tok) { size = atoi(size_tok->value); has_size = true; }
        }
        expect(parser, TOK_RBRACKET, "Expected ']' in array type");
        Type *t = (Type *)calloc(1, sizeof(Type));
        t->kind = TYPE_ARRAY;
        t->data.array.element_type = make_type_base(base_name);
        t->data.array.size = size;
        t->data.array.has_size = has_size;
        free(base_name);
        return t;
    }

    Type *t = make_type_base(base_name);
    free(base_name);
    return t;
}

/* ============== Expression parsing ============== */

static Expr *parse_list_literal(Parser *parser) {
    expect(parser, TOK_LBRACKET, "Expected '['");
    skip_whitespace_in_literal(parser);

    int capacity = 4;
    Expr **elements = (Expr **)malloc(capacity * sizeof(Expr *));
    int count = 0;

    while (!match(parser, TOK_RBRACKET) && !match(parser, TOK_EOF)) {
        skip_whitespace_in_literal(parser);
        if (match(parser, TOK_RBRACKET)) break;
        if (count >= capacity) { capacity *= 2; elements = realloc(elements, capacity * sizeof(Expr *)); }
        elements[count++] = parse_expression(parser);
        skip_whitespace_in_literal(parser);
        if (match(parser, TOK_COMMA)) {
            advance(parser);
        } else {
            break;
        }
    }

    expect(parser, TOK_RBRACKET, "Expected ']' after list elements");

    Expr *expr = make_expr(EXPR_LIST_LITERAL);
    expr->data.list_literal.elements = elements;
    expr->data.list_literal.element_count = count;
    return expr;
}

static Expr *parse_map_literal(Parser *parser) {
    expect(parser, TOK_LBRACE, "Expected '{'");
    skip_whitespace_in_literal(parser);

    int capacity = 4;
    MapEntry *entries = (MapEntry *)malloc(capacity * sizeof(MapEntry));
    int count = 0;

    if (!match(parser, TOK_RBRACE)) {
        while (true) {
            skip_whitespace_in_literal(parser);
            if (match(parser, TOK_RBRACE) || match(parser, TOK_EOF)) break;

            if (count >= capacity) { capacity *= 2; entries = realloc(entries, capacity * sizeof(MapEntry)); }

            entries[count].key = parse_expression(parser);
            skip_whitespace_in_literal(parser);
            expect(parser, TOK_COLON, "Expected ':' in map literal");
            skip_whitespace_in_literal(parser);
            entries[count].value = parse_expression(parser);
            count++;

            skip_whitespace_in_literal(parser);
            if (match(parser, TOK_COMMA)) {
                advance(parser);
                skip_whitespace_in_literal(parser);
                if (match(parser, TOK_RBRACE)) { advance(parser); break; }
            } else if (match(parser, TOK_RBRACE)) {
                advance(parser);
                break;
            } else {
                fprintf(stderr, "Parse error at line %d: Expected ',' or '}' in map literal\n",
                        peek(parser)->line);
                parser->had_error = 1;
                break;
            }
        }
    } else {
        advance(parser); /* consume } for empty map */
    }

    Expr *expr = make_expr(EXPR_MAP_LITERAL);
    expr->data.map_literal.entries = entries;
    expr->data.map_literal.entry_count = count;
    return expr;
}

/* Look ahead to decide if { starts a map literal (has expr : pattern) */
static bool looks_like_map_literal(Parser *parser) {
    /* Count LBRACE as map if empty {} or has colon after first expression.
     * Simple heuristic: peek at what follows { skipping whitespace.
     * If it's } it's an empty map. If we see a colon at bracket depth 0
     * before a }, it's a map. */
    Token *tok = peek(parser)->next; /* token after { */
    /* skip whitespace tokens */
    while (tok && (tok->type == TOK_NEWLINE || tok->type == TOK_INDENT || tok->type == TOK_DEDENT))
        tok = tok->next;
    if (!tok) return false;
    if (tok->type == TOK_RBRACE) return true; /* empty map {} */

    /* Walk forward at depth 0 looking for colon before } */
    int depth = 0;
    while (tok) {
        if (tok->type == TOK_LBRACE || tok->type == TOK_LBRACKET || tok->type == TOK_LPAREN) depth++;
        else if (tok->type == TOK_RBRACE || tok->type == TOK_RBRACKET || tok->type == TOK_RPAREN) {
            if (depth == 0) return false;
            depth--;
        } else if (tok->type == TOK_COLON && depth == 0) {
            return true;
        } else if (tok->type == TOK_EOF) {
            return false;
        }
        tok = tok->next;
    }
    return false;
}

static Expr *parse_primary(Parser *parser) {
    Token *tok = peek(parser);

    switch (tok->type) {
        case TOK_INTEGER_LITERAL: {
            advance(parser);
            Expr *expr = make_expr(EXPR_INTEGER);
            expr->data.integer.value = strdup(tok->value);
            return expr;
        }
        case TOK_FLOAT_LITERAL: {
            advance(parser);
            Expr *expr = make_expr(EXPR_FLOAT);
            expr->data.float_lit.value = strdup(tok->value);
            return expr;
        }
        case TOK_STRING_LITERAL: {
            advance(parser);
            Expr *expr = make_expr(EXPR_STRING);
            expr->data.string.value = strdup(tok->value);
            return expr;
        }
        case TOK_CHAR_LITERAL: {
            advance(parser);
            Expr *expr = make_expr(EXPR_CHAR);
            expr->data.char_lit.value = tok->value[0];
            return expr;
        }
        case TOK_TRUE: {
            advance(parser);
            Expr *expr = make_expr(EXPR_BOOL);
            expr->data.boolean.value = true;
            return expr;
        }
        case TOK_FALSE: {
            advance(parser);
            Expr *expr = make_expr(EXPR_BOOL);
            expr->data.boolean.value = false;
            return expr;
        }
        case TOK_NULL: {
            advance(parser);
            return make_expr(EXPR_NULL);
        }

        case TOK_NEW: {
            advance(parser);
            Token *class_tok = expect(parser, TOK_IDENTIFIER, "Expected class name after 'new'");
            /* Optional generic args: new list<int>() */
            if (match(parser, TOK_LT)) {
                advance(parser);
                int depth = 1;
                while (depth > 0 && !match(parser, TOK_EOF)) {
                    if (match(parser, TOK_LT)) depth++;
                    else if (match(parser, TOK_GT)) { depth--; if (depth <= 0) break; }
                    advance(parser);
                }
                expect(parser, TOK_GT, "Expected '>' after type arguments");
            }
            expect(parser, TOK_LPAREN, "Expected '(' after class name");

            int capacity = 4;
            Expr **args = (Expr **)malloc(capacity * sizeof(Expr *));
            int arg_count = 0;
            while (!match(parser, TOK_RPAREN) && !match(parser, TOK_EOF)) {
                if (arg_count >= capacity) { capacity *= 2; args = realloc(args, capacity * sizeof(Expr *)); }
                args[arg_count++] = parse_expression(parser);
                if (match(parser, TOK_COMMA)) advance(parser);
                else break;
            }
            expect(parser, TOK_RPAREN, "Expected ')' after arguments");

            Expr *expr = make_expr(EXPR_NEW);
            expr->data.new_expr.class_name = strdup(class_tok->value);
            expr->data.new_expr.arguments = args;
            expr->data.new_expr.arg_count = arg_count;
            return expr;
        }

        case TOK_SELF: {
            advance(parser);
            Expr *expr = make_expr(EXPR_IDENTIFIER);
            expr->data.identifier.name = strdup("self");
            return expr;
        }
        case TOK_SUPER: {
            advance(parser);
            Expr *expr = make_expr(EXPR_IDENTIFIER);
            expr->data.identifier.name = strdup("super");
            return expr;
        }

        case TOK_LPAREN: {
            advance(parser);
            Expr *expr = parse_expression(parser);
            expect(parser, TOK_RPAREN, "Expected ')' after expression");
            return expr;
        }

        case TOK_LBRACKET:
            return parse_list_literal(parser);

        case TOK_LBRACE:
            if (looks_like_map_literal(parser)) {
                return parse_map_literal(parser);
            }
            fprintf(stderr, "Parse error at line %d: Unexpected '{' in expression\n", tok->line);
            parser->had_error = 1;
            advance(parser);
            return make_expr(EXPR_NULL);

        case TOK_IDENTIFIER: {
            advance(parser);
            Expr *expr = make_expr(EXPR_IDENTIFIER);
            expr->data.identifier.name = strdup(tok->value);
            return expr;
        }

        default:
            fprintf(stderr, "Parse error at line %d: Unexpected token %s in expression\n",
                    tok->line, token_type_name(tok->type));
            parser->had_error = 1;
            advance(parser); /* consume to avoid infinite loop */
            return make_expr(EXPR_NULL);
    }
}

static Expr *parse_postfix(Parser *parser) {
    Expr *expr = parse_primary(parser);

    while (1) {
        if (match(parser, TOK_LPAREN)) {
            advance(parser);
            int capacity = 4;
            Expr **args = (Expr **)malloc(capacity * sizeof(Expr *));
            int arg_count = 0;

            while (!match(parser, TOK_RPAREN) && !match(parser, TOK_EOF)) {
                if (arg_count >= capacity) { capacity *= 2; args = realloc(args, capacity * sizeof(Expr *)); }
                args[arg_count++] = parse_expression(parser);
                if (match(parser, TOK_COMMA)) advance(parser);
                else break;
            }
            expect(parser, TOK_RPAREN, "Expected ')' after arguments");

            Expr *call = make_expr(EXPR_CALL);
            call->data.call.callee = expr;
            call->data.call.arguments = args;
            call->data.call.arg_count = arg_count;
            expr = call;
        }
        else if (match(parser, TOK_DOT)) {
            advance(parser);
            Token *field = expect(parser, TOK_IDENTIFIER, "Expected field name after '.'");
            Expr *access = make_expr(EXPR_FIELD_ACCESS);
            access->data.field_access.obj = expr;
            access->data.field_access.field = strdup(field ? field->value : "");
            expr = access;
        }
        else if (match(parser, TOK_LBRACKET)) {
            advance(parser);
            Expr *index_node = make_expr(EXPR_INDEX_ACCESS);
            index_node->data.index_access.obj = expr;
            index_node->data.index_access.index = parse_expression(parser);
            expect(parser, TOK_RBRACKET, "Expected ']' after index");
            expr = index_node;
        }
        else {
            break;
        }
    }

    return expr;
}

static Expr *parse_unary(Parser *parser) {
    if (match(parser, TOK_MINUS) || match(parser, TOK_NOT) ||
        match(parser, TOK_TILDE) || match(parser, TOK_PLUS)) {
        Token *op = advance(parser);
        Expr *operand = parse_unary(parser);
        Expr *expr = make_expr(EXPR_UNARY);
        expr->data.unary.operator = strdup(op->value);
        expr->data.unary.operand = operand;
        return expr;
    }
    return parse_postfix(parser);
}

static Expr *parse_multiplicative(Parser *parser) {
    Expr *left = parse_unary(parser);
    while (match(parser, TOK_STAR) || match(parser, TOK_SLASH) || match(parser, TOK_PERCENT)) {
        Token *op = advance(parser);
        Expr *right = parse_unary(parser);
        Expr *expr = make_expr(EXPR_BINARY);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_additive(Parser *parser) {
    Expr *left = parse_multiplicative(parser);
    while (match(parser, TOK_PLUS) || match(parser, TOK_MINUS)) {
        Token *op = advance(parser);
        Expr *right = parse_multiplicative(parser);
        Expr *expr = make_expr(EXPR_BINARY);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_comparison(Parser *parser) {
    Expr *left = parse_additive(parser);
    while (match(parser, TOK_LT) || match(parser, TOK_GT) ||
           match(parser, TOK_LE) || match(parser, TOK_GE)) {
        Token *op = advance(parser);
        Expr *right = parse_additive(parser);
        Expr *expr = make_expr(EXPR_BINARY);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_equality(Parser *parser) {
    Expr *left = parse_comparison(parser);
    while (match(parser, TOK_EQ) || match(parser, TOK_NE)) {
        Token *op = advance(parser);
        Expr *right = parse_comparison(parser);
        Expr *expr = make_expr(EXPR_BINARY);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_and(Parser *parser) {
    Expr *left = parse_equality(parser);
    while (match(parser, TOK_AND)) {
        Token *op = advance(parser);
        Expr *right = parse_equality(parser);
        Expr *expr = make_expr(EXPR_BINARY);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_or(Parser *parser) {
    Expr *left = parse_and(parser);
    while (match(parser, TOK_OR)) {
        Token *op = advance(parser);
        Expr *right = parse_and(parser);
        Expr *expr = make_expr(EXPR_BINARY);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_assignment(Parser *parser) {
    Expr *expr = parse_or(parser);

    if (match(parser, TOK_ASSIGN)) {
        advance(parser);
        Expr *value = parse_assignment(parser);
        Expr *assign = make_expr(EXPR_ASSIGNMENT);
        assign->data.assignment.target = expr;
        assign->data.assignment.value = value;
        return assign;
    }
    if (match(parser, TOK_PLUS_ASSIGN) || match(parser, TOK_MINUS_ASSIGN) ||
        match(parser, TOK_STAR_ASSIGN) || match(parser, TOK_SLASH_ASSIGN)) {
        Token *op = advance(parser);
        Expr *value = parse_assignment(parser);
        Expr *assign = make_expr(EXPR_COMPOUND_ASSIGN);
        assign->data.compound_assign.target = expr;
        assign->data.compound_assign.operator = strdup(op->value);
        assign->data.compound_assign.value = value;
        return assign;
    }

    return expr;
}

static Expr *parse_expression(Parser *parser) {
    return parse_assignment(parser);
}

/* ============== Statement parsing ============== */

/* Look ahead to decide if current position is a variable declaration.
 * Strategy: try to skip a type, then see if what follows is an identifier. */
static bool looks_like_declaration(Parser *parser) {
    Token *tok = peek(parser);
    if (!is_type_token(tok->type)) return false;

    /* Walk forward past the type (including generics and arrays) */
    Token *t = tok->next;

    /* If tok is a list/map type token, skip possible <...> */
    if (tok->type == TOK_LIST_TYPE || tok->type == TOK_MAP_TYPE) {
        if (t && t->type == TOK_LT) {
            int depth = 1;
            t = t->next;
            while (t && depth > 0) {
                if (t->type == TOK_LT) depth++;
                else if (t->type == TOK_GT) depth--;
                t = t->next;
            }
        }
    }
    /* Skip optional array brackets: [] or [size] */
    if (t && t->type == TOK_LBRACKET) {
        t = t->next;
        if (t && t->type == TOK_INTEGER_LITERAL) t = t->next;
        if (t && t->type == TOK_RBRACKET) t = t->next;
    }

    return (t && t->type == TOK_IDENTIFIER);
}

static Stmt *parse_var_decl(Parser *parser, bool is_const, bool use_var) {
    Type *var_type = NULL;
    char *name = NULL;

    if (use_var) {
        /* var name = value */
        Token *name_tok = expect(parser, TOK_IDENTIFIER, "Expected variable name after 'var'");
        name = strdup(name_tok ? name_tok->value : "");
        expect(parser, TOK_ASSIGN, "Expected '=' after var name");
        Expr *initializer = parse_expression(parser);
        Stmt *stmt = make_stmt(STMT_VAR_DECL);
        stmt->data.var_decl.is_const = false;
        stmt->data.var_decl.var_type = NULL;
        stmt->data.var_decl.name = name;
        stmt->data.var_decl.initializer = initializer;
        return stmt;
    }

    /* Explicit type: Type name [= value] */
    var_type = parse_type(parser);
    Token *name_tok = expect(parser, TOK_IDENTIFIER, "Expected variable name");
    name = strdup(name_tok ? name_tok->value : "");

    Expr *initializer = NULL;
    if (match(parser, TOK_ASSIGN)) {
        advance(parser);
        initializer = parse_expression(parser);
    }

    Stmt *stmt = make_stmt(STMT_VAR_DECL);
    stmt->data.var_decl.is_const = is_const;
    stmt->data.var_decl.var_type = var_type;
    stmt->data.var_decl.name = name;
    stmt->data.var_decl.initializer = initializer;
    return stmt;
}

static Stmt **parse_block(Parser *parser, int *count) {
    expect(parser, TOK_INDENT, "Expected indentation");

    int capacity = 4;
    Stmt **statements = (Stmt **)malloc(capacity * sizeof(Stmt *));
    *count = 0;

    while (!match(parser, TOK_DEDENT) && !match(parser, TOK_EOF)) {
        if (match(parser, TOK_NEWLINE)) {
            advance(parser);
            continue;
        }

        if (*count >= capacity) {
            capacity *= 2;
            statements = (Stmt **)realloc(statements, capacity * sizeof(Stmt *));
        }

        Stmt *stmt = parse_statement(parser);
        if (stmt) {
            statements[(*count)++] = stmt;
        }

        if (match(parser, TOK_NEWLINE)) {
            advance(parser);
        }
    }

    expect(parser, TOK_DEDENT, "Expected dedent");
    return statements;
}

static Stmt *parse_if_statement(Parser *parser) {
    expect(parser, TOK_IF, "Expected 'if'");
    Expr *condition = parse_expression(parser);
    expect(parser, TOK_COLON, "Expected ':' after if condition");
    expect_newline(parser);

    int then_count = 0;
    Stmt **then_body = parse_block(parser, &then_count);

    Stmt **else_body = NULL;
    int else_count = 0;

    if (match(parser, TOK_NEWLINE)) advance(parser);

    if (match(parser, TOK_ELSE)) {
        advance(parser);
        expect(parser, TOK_COLON, "Expected ':' after else");
        expect_newline(parser);
        else_body = parse_block(parser, &else_count);
    }

    Stmt *stmt = make_stmt(STMT_IF);
    stmt->data.if_stmt.condition = condition;
    stmt->data.if_stmt.then_body = then_body;
    stmt->data.if_stmt.then_count = then_count;
    stmt->data.if_stmt.else_body = else_body;
    stmt->data.if_stmt.else_count = else_count;
    return stmt;
}

static Stmt *parse_while_statement(Parser *parser) {
    expect(parser, TOK_WHILE, "Expected 'while'");
    Expr *condition = parse_expression(parser);
    expect(parser, TOK_COLON, "Expected ':' after while condition");
    expect_newline(parser);

    int body_count = 0;
    Stmt **body = parse_block(parser, &body_count);

    Stmt *stmt = make_stmt(STMT_WHILE);
    stmt->data.while_stmt.condition = condition;
    stmt->data.while_stmt.body = body;
    stmt->data.while_stmt.body_count = body_count;
    return stmt;
}

static Stmt *parse_for_statement(Parser *parser) {
    expect(parser, TOK_FOR, "Expected 'for'");
    Token *var_tok = expect(parser, TOK_IDENTIFIER, "Expected loop variable");
    expect(parser, TOK_IN, "Expected 'in'");
    Expr *iterable = parse_expression(parser);
    expect(parser, TOK_COLON, "Expected ':' after for iterable");
    expect_newline(parser);

    int body_count = 0;
    Stmt **body = parse_block(parser, &body_count);

    Stmt *stmt = make_stmt(STMT_FOR);
    stmt->data.for_stmt.variable = strdup(var_tok ? var_tok->value : "");
    stmt->data.for_stmt.iterable = iterable;
    stmt->data.for_stmt.body = body;
    stmt->data.for_stmt.body_count = body_count;
    return stmt;
}


static Stmt *parse_switch_statement(Parser *parser) {
    expect(parser, TOK_SWITCH, "Expected 'switch'");
    Expr *expr = parse_expression(parser);
    expect(parser, TOK_COLON, "Expected ':' after switch expression");
    expect_newline(parser);

    /* Expect INDENT then parse cases */
    expect(parser, TOK_INDENT, "Expected indentation in switch");

    int case_capacity = 4;
    SwitchCase *cases = (SwitchCase *)malloc(case_capacity * sizeof(SwitchCase));
    int case_count = 0;

    while (!match(parser, TOK_DEDENT) && !match(parser, TOK_EOF)) {
        if (match(parser, TOK_NEWLINE)) { advance(parser); continue; }

        Expr *case_value = NULL;
        if (match(parser, TOK_CASE)) {
            advance(parser);
            case_value = parse_expression(parser);
        } else if (match(parser, TOK_DEFAULT)) {
            advance(parser);
            case_value = NULL;
        } else {
            advance(parser);
            continue;
        }

        expect(parser, TOK_COLON, "Expected ':' after case");
        expect_newline(parser);

        int body_count = 0;
        Stmt **body = parse_block(parser, &body_count);

        if (match(parser, TOK_NEWLINE)) advance(parser);

        if (case_count >= case_capacity) { case_capacity *= 2; cases = realloc(cases, case_capacity * sizeof(SwitchCase)); }
        cases[case_count].value = case_value;
        cases[case_count].body = body;
        cases[case_count].body_count = body_count;
        case_count++;
    }

    expect(parser, TOK_DEDENT, "Expected dedent after switch");

    Stmt *stmt = make_stmt(STMT_SWITCH);
    stmt->data.switch_stmt.expression = expr;
    stmt->data.switch_stmt.cases = cases;
    stmt->data.switch_stmt.case_count = case_count;
    return stmt;
}

static Stmt *parse_throw_statement(Parser *parser) {
    expect(parser, TOK_THROW, "Expected 'throw'");
    Expr *expr = parse_expression(parser);
    Stmt *stmt = make_stmt(STMT_THROW);
    stmt->data.throw_stmt.expression = expr;
    return stmt;
}

static Stmt *parse_try_statement(Parser *parser) {
    expect(parser, TOK_TRY, "Expected 'try'");
    expect(parser, TOK_COLON, "Expected ':' after try");
    expect_newline(parser);

    int try_count = 0;
    Stmt **try_body = parse_block(parser, &try_count);

    int catch_capacity = 2;
    CatchClause *catch_clauses = (CatchClause *)malloc(catch_capacity * sizeof(CatchClause));
    int catch_count = 0;
    Stmt **finally_body = NULL;
    int finally_count = 0;

    if (match(parser, TOK_NEWLINE)) advance(parser);

    while (match(parser, TOK_CATCH)) {
        advance(parser);
        char *var_name = strdup("e");
        if (match(parser, TOK_IDENTIFIER)) {
            free(var_name);
            var_name = strdup(peek(parser)->value);
            advance(parser);
        }
        expect(parser, TOK_COLON, "Expected ':' after catch");
        expect_newline(parser);

        int catch_body_count = 0;
        Stmt **catch_body = parse_block(parser, &catch_body_count);

        if (match(parser, TOK_NEWLINE)) advance(parser);

        if (catch_count >= catch_capacity) { catch_capacity *= 2; catch_clauses = realloc(catch_clauses, catch_capacity * sizeof(CatchClause)); }
        catch_clauses[catch_count].variable = var_name;
        catch_clauses[catch_count].body = catch_body;
        catch_clauses[catch_count].body_count = catch_body_count;
        catch_count++;
    }

    if (match(parser, TOK_FINALLY)) {
        advance(parser);
        expect(parser, TOK_COLON, "Expected ':' after finally");
        expect_newline(parser);
        finally_body = parse_block(parser, &finally_count);
    }

    Stmt *stmt = make_stmt(STMT_TRY);
    stmt->data.try_stmt.try_body = try_body;
    stmt->data.try_stmt.try_count = try_count;
    stmt->data.try_stmt.catch_clauses = catch_clauses;
    stmt->data.try_stmt.catch_count = catch_count;
    stmt->data.try_stmt.finally_body = finally_body;
    stmt->data.try_stmt.finally_count = finally_count;
    return stmt;
}

static Stmt *parse_return_statement(Parser *parser) {
    expect(parser, TOK_RETURN, "Expected 'return'");
    Expr *value = NULL;
    if (!match(parser, TOK_NEWLINE) && !match(parser, TOK_DEDENT) && !match(parser, TOK_EOF)) {
        value = parse_expression(parser);
    }
    Stmt *stmt = make_stmt(STMT_RETURN);
    stmt->data.return_stmt.value = value;
    return stmt;
}

static Stmt *parse_statement(Parser *parser) {
    skip_newlines(parser);

    /* const */
    if (match(parser, TOK_CONST)) {
        advance(parser);
        return parse_var_decl(parser, true, false);
    }

    /* var */
    if (match(parser, TOK_VAR)) {
        advance(parser);
        return parse_var_decl(parser, false, true);
    }

    /* Typed variable declaration: Type name [= expr] */
    if (looks_like_declaration(parser)) {
        return parse_var_decl(parser, false, false);
    }

    if (match(parser, TOK_IF))       return parse_if_statement(parser);
    if (match(parser, TOK_WHILE))    return parse_while_statement(parser);
    if (match(parser, TOK_FOR))      return parse_for_statement(parser);
    if (match(parser, TOK_SWITCH))   return parse_switch_statement(parser);
    if (match(parser, TOK_THROW))    return parse_throw_statement(parser);
    if (match(parser, TOK_TRY))      return parse_try_statement(parser);
    if (match(parser, TOK_RETURN))   return parse_return_statement(parser);

    if (match(parser, TOK_PASS)) {
        advance(parser);
        return make_stmt(STMT_PASS);
    }
    if (match(parser, TOK_BREAK)) {
        advance(parser);
        return make_stmt(STMT_BREAK);
    }
    if (match(parser, TOK_CONTINUE)) {
        advance(parser);
        return make_stmt(STMT_CONTINUE);
    }

    /* Expression statement */
    Expr *expr = parse_expression(parser);
    Stmt *stmt = make_stmt(STMT_EXPRESSION);
    stmt->data.expression.expression = expr;
    return stmt;
}

/* ============== Declaration parsing ============== */

static FunctionParam *parse_parameters(Parser *parser, int *count) {
    *count = 0;
    if (match(parser, TOK_RPAREN)) return NULL;

    int capacity = 4;
    FunctionParam *params = (FunctionParam *)malloc(capacity * sizeof(FunctionParam));

    while (!match(parser, TOK_RPAREN) && !match(parser, TOK_EOF)) {
        if (*count >= capacity) {
            capacity *= 2;
            params = (FunctionParam *)realloc(params, capacity * sizeof(FunctionParam));
        }
        params[*count].param_type = parse_type(parser);
        Token *name = expect(parser, TOK_IDENTIFIER, "Expected parameter name");
        params[*count].name = strdup(name ? name->value : "");
        (*count)++;
        if (match(parser, TOK_COMMA)) advance(parser);
    }
    return params;
}

static Decl *parse_function_declaration(Parser *parser) {
    expect(parser, TOK_DEF, "Expected 'def'");
    Token *name = expect(parser, TOK_IDENTIFIER, "Expected function name");

    expect(parser, TOK_LPAREN, "Expected '(' after function name");
    int param_count = 0;
    FunctionParam *params = parse_parameters(parser, &param_count);
    expect(parser, TOK_RPAREN, "Expected ')' after parameters");

    Type *return_type = NULL;
    if (match(parser, TOK_ARROW)) {
        advance(parser);
        return_type = parse_type(parser);
    } else {
        return_type = make_type_base("void");
    }

    expect(parser, TOK_COLON, "Expected ':' after function declaration");
    expect_newline(parser);

    int body_count = 0;
    Stmt **body = parse_block(parser, &body_count);

    Decl *decl = make_decl(DECL_FUNCTION);
    decl->data.function.name = strdup(name ? name->value : "");
    decl->data.function.params = params;
    decl->data.function.param_count = param_count;
    decl->data.function.return_type = return_type;
    decl->data.function.body = body;
    decl->data.function.body_count = body_count;
    return decl;
}

static Decl *parse_struct_declaration(Parser *parser) {
    expect(parser, TOK_STRUCT, "Expected 'struct'");
    Token *name = expect(parser, TOK_IDENTIFIER, "Expected struct name");
    expect(parser, TOK_COLON, "Expected ':' after struct name");
    expect_newline(parser);
    expect(parser, TOK_INDENT, "Expected indentation in struct");

    int capacity = 4;
    StructField *fields = (StructField *)malloc(capacity * sizeof(StructField));
    int field_count = 0;

    while (!match2(parser, TOK_DEDENT, TOK_EOF)) {
        if (match(parser, TOK_NEWLINE)) { advance(parser); continue; }
        if (field_count >= capacity) { capacity *= 2; fields = realloc(fields, capacity * sizeof(StructField)); }
        fields[field_count].field_type = parse_type(parser);
        Token *fname = expect(parser, TOK_IDENTIFIER, "Expected field name");
        fields[field_count].name = strdup(fname ? fname->value : "");
        field_count++;
        expect_newline(parser);
    }
    expect(parser, TOK_DEDENT, "Expected dedent after struct");

    Decl *decl = make_decl(DECL_STRUCT);
    decl->data.struct_decl.name = strdup(name ? name->value : "");
    decl->data.struct_decl.fields = fields;
    decl->data.struct_decl.field_count = field_count;
    return decl;
}

static Decl *parse_class_declaration(Parser *parser) {
    expect(parser, TOK_CLASS, "Expected 'class'");
    Token *name = expect(parser, TOK_IDENTIFIER, "Expected class name");

    char *base_class = NULL;
    if (match(parser, TOK_EXTENDS)) {
        advance(parser);
        Token *base = expect(parser, TOK_IDENTIFIER, "Expected base class name");
        base_class = strdup(base ? base->value : "");
    }

    expect(parser, TOK_COLON, "Expected ':' after class name");
    expect_newline(parser);
    expect(parser, TOK_INDENT, "Expected indentation in class");

    int field_cap = 4, method_cap = 4;
    StructField *fields = (StructField *)malloc(field_cap * sizeof(StructField));
    Decl **methods = (Decl **)malloc(method_cap * sizeof(Decl *));
    int field_count = 0, method_count = 0;

    while (!match2(parser, TOK_DEDENT, TOK_EOF)) {
        if (match(parser, TOK_NEWLINE)) { advance(parser); continue; }

        if (match(parser, TOK_DEF)) {
            if (method_count >= method_cap) { method_cap *= 2; methods = realloc(methods, method_cap * sizeof(Decl *)); }
            methods[method_count++] = parse_function_declaration(parser);
            if (match(parser, TOK_NEWLINE)) advance(parser);
        } else {
            if (field_count >= field_cap) { field_cap *= 2; fields = realloc(fields, field_cap * sizeof(StructField)); }
            fields[field_count].field_type = parse_type(parser);
            Token *fname = expect(parser, TOK_IDENTIFIER, "Expected field name");
            fields[field_count].name = strdup(fname ? fname->value : "");
            field_count++;
            expect_newline(parser);
        }
    }
    expect(parser, TOK_DEDENT, "Expected dedent after class");

    Decl *decl = make_decl(DECL_CLASS);
    decl->data.class_decl.name = strdup(name ? name->value : "");
    decl->data.class_decl.base_class = base_class;
    decl->data.class_decl.fields = fields;
    decl->data.class_decl.field_count = field_count;
    decl->data.class_decl.methods = methods;
    decl->data.class_decl.method_count = method_count;
    return decl;
}

static Decl *parse_enum_declaration(Parser *parser) {
    expect(parser, TOK_ENUM, "Expected 'enum'");
    Token *name = expect(parser, TOK_IDENTIFIER, "Expected enum name");
    expect(parser, TOK_COLON, "Expected ':'");
    expect_newline(parser);
    expect(parser, TOK_INDENT, "Expected indentation in enum");

    int capacity = 4;
    EnumVariant *variants = (EnumVariant *)malloc(capacity * sizeof(EnumVariant));
    int variant_count = 0;
    int auto_value = 0;

    while (!match2(parser, TOK_DEDENT, TOK_EOF)) {
        if (match(parser, TOK_NEWLINE)) { advance(parser); continue; }
        if (variant_count >= capacity) { capacity *= 2; variants = realloc(variants, capacity * sizeof(EnumVariant)); }

        Token *vname = expect(parser, TOK_IDENTIFIER, "Expected variant name");
        variants[variant_count].name = strdup(vname ? vname->value : "");

        if (match(parser, TOK_ASSIGN)) {
            advance(parser);
            Token *val = expect(parser, TOK_INTEGER_LITERAL, "Expected integer value");
            variants[variant_count].value = val ? atoi(val->value) : 0;
            auto_value = variants[variant_count].value + 1;
            variants[variant_count].has_value = true;
        } else {
            variants[variant_count].value = auto_value++;
            variants[variant_count].has_value = false;
        }
        variant_count++;
        expect_newline(parser);
    }
    expect(parser, TOK_DEDENT, "Expected dedent after enum");

    Decl *decl = make_decl(DECL_ENUM);
    decl->data.enum_decl.name = strdup(name ? name->value : "");
    decl->data.enum_decl.variants = variants;
    decl->data.enum_decl.variant_count = variant_count;
    return decl;
}

static Decl *parse_import_declaration(Parser *parser) {
    char *module_name = NULL;
    char **items = NULL;
    int item_count = 0;
    bool import_all = false;

    if (match(parser, TOK_FROM)) {
        advance(parser);
        Token *mod = expect(parser, TOK_IDENTIFIER, "Expected module name");
        module_name = strdup(mod ? mod->value : "");
        expect(parser, TOK_IMPORT, "Expected 'import'");

        if (match(parser, TOK_STAR)) {
            advance(parser);
            import_all = true;
        } else {
            int cap = 4;
            items = (char **)malloc(cap * sizeof(char *));
            Token *item = expect(parser, TOK_IDENTIFIER, "Expected item name");
            items[item_count++] = strdup(item ? item->value : "");
            while (match(parser, TOK_COMMA)) {
                advance(parser);
                if (item_count >= cap) { cap *= 2; items = realloc(items, cap * sizeof(char *)); }
                item = expect(parser, TOK_IDENTIFIER, "Expected item name");
                items[item_count++] = strdup(item ? item->value : "");
            }
        }
    } else {
        expect(parser, TOK_IMPORT, "Expected 'import'");
        Token *mod = expect(parser, TOK_IDENTIFIER, "Expected module name");
        module_name = strdup(mod ? mod->value : "");
    }

    Decl *decl = make_decl(DECL_IMPORT);
    decl->data.import_decl.module_name = module_name;
    decl->data.import_decl.items = items;
    decl->data.import_decl.item_count = item_count;
    decl->data.import_decl.import_all = import_all;
    return decl;
}

static bool is_declaration_start(Parser *parser) {
    TokenType t = peek(parser)->type;
    return t == TOK_DEF || t == TOK_STRUCT || t == TOK_CLASS ||
           t == TOK_IMPORT || t == TOK_FROM || t == TOK_ENUM;
}

static Decl *parse_declaration(Parser *parser) {
    if (match(parser, TOK_DEF))    return parse_function_declaration(parser);
    if (match(parser, TOK_STRUCT)) return parse_struct_declaration(parser);
    if (match(parser, TOK_CLASS))  return parse_class_declaration(parser);
    if (match(parser, TOK_ENUM))   return parse_enum_declaration(parser);
    if (match(parser, TOK_IMPORT) || match(parser, TOK_FROM))
        return parse_import_declaration(parser);
    return NULL;
}

/* ============== Public API ============== */

Parser *parser_new(TokenList *tokens) {
    Parser *parser = (Parser *)malloc(sizeof(Parser));
    if (!parser) return NULL;
    parser->tokens = tokens;
    parser->current = tokens->head;
    parser->had_error = 0;
    return parser;
}

void parser_free(Parser *parser) {
    if (parser) free(parser);
}

void parser_error(Parser *parser, const char *message) {
    fprintf(stderr, "Parse error at line %d: %s\n", peek(parser)->line, message);
    parser->had_error = 1;
}

Program *parser_parse(Parser *parser) {
    Program *program = (Program *)malloc(sizeof(Program));
    if (!program) return NULL;

    program->declarations = NULL;
    program->decl_count = 0;
    program->statements = NULL;
    program->stmt_count = 0;

    int decl_capacity = 4;
    int stmt_capacity = 4;
    program->declarations = (Decl **)malloc(decl_capacity * sizeof(Decl *));
    program->statements   = (Stmt **)malloc(stmt_capacity * sizeof(Stmt *));

    skip_newlines(parser);

    while (!match(parser, TOK_EOF)) {
        if (is_declaration_start(parser)) {
            Decl *decl = parse_declaration(parser);
            if (decl) {
                if (program->decl_count >= decl_capacity) {
                    decl_capacity *= 2;
                    program->declarations = (Decl **)realloc(program->declarations, decl_capacity * sizeof(Decl *));
                }
                program->declarations[program->decl_count++] = decl;
            }
        } else {
            Stmt *stmt = parse_statement(parser);
            if (stmt) {
                if (program->stmt_count >= stmt_capacity) {
                    stmt_capacity *= 2;
                    program->statements = (Stmt **)realloc(program->statements, stmt_capacity * sizeof(Stmt *));
                }
                program->statements[program->stmt_count++] = stmt;
            }
        }
        skip_newlines(parser);
    }

    return program;
}
