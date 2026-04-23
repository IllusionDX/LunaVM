/* Native C Parser for Luna Programming Language.
 * Converts tokens into an Abstract Syntax Tree.
 * Dynamic VM variant — type annotations are optional hints.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

/* ============== Forward declarations ============== */

static bool is_declaration_start(Parser *parser);
static Decl *parse_declaration(Parser *parser);
static Decl *parse_function_declaration(Parser *parser);
static Stmt *parse_statement(Parser *parser);
static Stmt **parse_block(Parser *parser, int *count);
static Expr *parse_expression(Parser *parser);
static Expr *parse_assignment(Parser *parser);
static Expr *parse_primary(Parser *parser);
static Expr *parse_anonymous_function(Parser *parser);
static Expr *parse_postfix(Parser *parser);
static Expr *desugar_fstring(const char *template);

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
        return peek(parser);
    }
    return advance(parser);
}

static void skip_newlines(Parser *parser) {
    while (match(parser, TOK_NEWLINE)) {
        advance(parser);
    }
}

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

/* ============== Type hint tokens ============== */

static bool is_type_hint_token(TokenType t) {
    return t == TOK_INT_TYPE || t == TOK_FLOAT_TYPE || t == TOK_DOUBLE_TYPE ||
           t == TOK_BOOL_TYPE || t == TOK_CHAR_TYPE || t == TOK_STRING_TYPE ||
           t == TOK_LIST_TYPE || t == TOK_DICT || t == TOK_IDENTIFIER;
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

static Expr *parse_dict_literal(Parser *parser) {
    expect(parser, TOK_LBRACE, "Expected '{'");
    skip_whitespace_in_literal(parser);

    int capacity = 4;
    DictEntry *entries = (DictEntry *)malloc(capacity * sizeof(DictEntry));
    int count = 0;

    if (!match(parser, TOK_RBRACE)) {
        while (true) {
            skip_whitespace_in_literal(parser);
            if (match(parser, TOK_RBRACE) || match(parser, TOK_EOF)) break;

            if (count >= capacity) { capacity *= 2; entries = realloc(entries, capacity * sizeof(DictEntry)); }

            entries[count].key = parse_expression(parser);
            skip_whitespace_in_literal(parser);
            expect(parser, TOK_COLON, "Expected ':' in dict literal");
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
                fprintf(stderr, "Parse error at line %d: Expected ',' or '}' in dict literal\n",
                        peek(parser)->line);
                parser->had_error = 1;
                break;
            }
        }
    } else {
        advance(parser);
    }

    Expr *expr = make_expr(EXPR_DICT_LITERAL);
    expr->data.dict_literal.entries = entries;
    expr->data.dict_literal.entry_count = count;
    return expr;
}

static bool looks_like_dict_literal(Parser *parser) {
    Token *tok = peek(parser)->next;
    while (tok && (tok->type == TOK_NEWLINE || tok->type == TOK_INDENT || tok->type == TOK_DEDENT))
        tok = tok->next;
    if (!tok) return false;
    if (tok->type == TOK_RBRACE) return true;

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

/* ============== F-string desugaring ============== */

static Expr *append_fstring_part(Expr *current, Expr *part) {
    if (!current) {
        if (part->kind != EXPR_STRING) {
            Expr *empty = make_expr(EXPR_STRING);
            empty->data.string.value = strdup("");
            Expr *bin = make_expr(EXPR_BINARY);
            bin->data.binary.left = empty;
            bin->data.binary.operator = strdup("+");
            bin->data.binary.right = part;
            return bin;
        }
        return part;
    }

    Expr *bin = make_expr(EXPR_BINARY);
    bin->data.binary.left = current;
    bin->data.binary.operator = strdup("+");
    bin->data.binary.right = part;
    return bin;
}

static Expr *parse_sub_expression(const char *source) {
    Lexer *sub_lexer = lexer_new(source);
    TokenList *sub_tokens = lexer_tokenize(sub_lexer);
    Parser *sub_parser = parser_new(sub_tokens);
    Program *sub_program = parser_parse(sub_parser);

    Expr *result = NULL;
    if (sub_program && !sub_parser->had_error && sub_program->stmt_count > 0) {
        Stmt *stmt = sub_program->statements[0];
        if (stmt->kind == STMT_EXPRESSION && stmt->data.expression.expression) {
            result = stmt->data.expression.expression;
            stmt->data.expression.expression = NULL;
        }
    }

    if (sub_program) free_program(sub_program);
    parser_free(sub_parser);
    token_list_free(sub_tokens);
    lexer_free(sub_lexer);

    return result;
}

static Expr *desugar_fstring(const char *template) {
    int len = (int)strlen(template);
    int i = 0;
    Expr *result = NULL;

    while (i < len) {
        int brace_start = -1;
        for (int j = i; j < len; j++) {
            if (template[j] == '{') {
                brace_start = j;
                break;
            }
        }

        int text_len = (brace_start == -1) ? len - i : brace_start - i;
        if (text_len > 0) {
            Expr *text = make_expr(EXPR_STRING);
            text->data.string.value = malloc(text_len + 1);
            memcpy(text->data.string.value, template + i, text_len);
            text->data.string.value[text_len] = '\0';
            result = append_fstring_part(result, text);
        }

        if (brace_start == -1) break;

        int depth = 1;
        int brace_end = -1;
        for (int j = brace_start + 1; j < len; j++) {
            if (template[j] == '{') depth++;
            else if (template[j] == '}') {
                depth--;
                if (depth == 0) {
                    brace_end = j;
                    break;
                }
            }
        }

        if (brace_end == -1) {
            int rest_len = len - brace_start;
            Expr *text = make_expr(EXPR_STRING);
            text->data.string.value = malloc(rest_len + 1);
            memcpy(text->data.string.value, template + brace_start, rest_len + 1);
            result = append_fstring_part(result, text);
            break;
        }

        int expr_len = brace_end - brace_start - 1;
        char *expr_source = malloc(expr_len + 2);
        memcpy(expr_source, template + brace_start + 1, expr_len);
        expr_source[expr_len] = '\n';
        expr_source[expr_len + 1] = '\0';

        Expr *expr = parse_sub_expression(expr_source);
        free(expr_source);

        if (!expr) {
            if (expr_len == 0) {
                fprintf(stderr, "f-string: empty expression not allowed\n");
            } else {
                fprintf(stderr, "f-string: failed to parse expression '");
                fwrite(template + brace_start + 1, 1, expr_len, stderr);
                fprintf(stderr, "'\n");
            }
            if (result) free_expr(result);
            return NULL;
        }

        result = append_fstring_part(result, expr);
        i = brace_end + 1;
    }

    if (!result) {
        result = make_expr(EXPR_STRING);
        result->data.string.value = strdup("");
    }

    return result;
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
    case TOK_FSTRING_LITERAL: {
        advance(parser);
        Expr *expr = desugar_fstring(tok->value);
        if (!expr) {
            parser->had_error = 1;
            return make_expr(EXPR_NULL);
        }
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
        if (looks_like_dict_literal(parser)) {
            return parse_dict_literal(parser);
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

    case TOK_DEF:
        return parse_anonymous_function(parser);

    default:
        fprintf(stderr, "Parse error at line %d: Unexpected token %s in expression\n",
                tok->line, token_type_name(tok->type));
        parser->had_error = 1;
        advance(parser);
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

static bool is_type_hint_start(Parser *parser) {
    Token *tok = peek(parser);
    if (!is_type_hint_token(tok->type)) return false;

    Token *next = tok->next;
    if (tok->type == TOK_LIST_TYPE || tok->type == TOK_DICT) {
        if (next && next->type == TOK_LT) {
            int depth = 1;
            next = next->next;
            while (next && depth > 0) {
                if (next->type == TOK_LT) depth++;
                else if (next->type == TOK_GT) depth--;
                next = next->next;
            }
        }
    }
    if (next && next->type == TOK_LBRACKET) {
        next = next->next;
        if (next && next->type == TOK_INTEGER_LITERAL) next = next->next;
        if (next && next->type == TOK_RBRACKET) next = next->next;
    }

    if (next && next->type == TOK_COLON) {
        Token *after_colon = next->next;
        while (after_colon && (after_colon->type == TOK_NEWLINE || after_colon->type == TOK_INDENT || after_colon->type == TOK_DEDENT))
            after_colon = after_colon->next;
        if (after_colon && after_colon->type == TOK_IDENTIFIER) {
            Token *after_name = after_colon->next;
            if (after_name && (after_name->type == TOK_ASSIGN || after_name->type == TOK_NEWLINE ||
                               after_name->type == TOK_COLON || after_name->type == TOK_EOF)) {
                return true;
            }
        }
    }

    if (next && next->type == TOK_IDENTIFIER) {
        Token *after_name = next->next;
        if (after_name && (after_name->type == TOK_ASSIGN || after_name->type == TOK_NEWLINE ||
                           after_name->type == TOK_COLON || after_name->type == TOK_EOF ||
                           after_name->type == TOK_COMMA || after_name->type == TOK_RPAREN)) {
            return true;
        }
    }

    return false;
}

static void skip_type_hint(Parser *parser) {
    Token *tok = peek(parser);
    if (!is_type_hint_token(tok->type)) return;

    advance(parser);

    if (match(parser, TOK_LT)) {
        advance(parser);
        int depth = 1;
        while (depth > 0 && !match(parser, TOK_EOF)) {
            if (match(parser, TOK_LT)) depth++;
            else if (match(parser, TOK_GT)) { depth--; if (depth <= 0) { advance(parser); break; } }
            advance(parser);
        }
    }

    if (match(parser, TOK_LBRACKET)) {
        advance(parser);
        if (!match(parser, TOK_RBRACKET)) {
            if (match(parser, TOK_INTEGER_LITERAL)) advance(parser);
        }
        if (match(parser, TOK_RBRACKET)) advance(parser);
    }
}

static Stmt *parse_var_decl(Parser *parser, bool is_const, bool use_var) {
    char *name = NULL;

    if (use_var) {
        Token *name_tok = expect(parser, TOK_IDENTIFIER, "Expected variable name after 'var'");
        name = strdup(name_tok ? name_tok->value : "");
        if (match(parser, TOK_COLON)) {
            advance(parser);
            skip_type_hint(parser);
        }
        expect(parser, TOK_ASSIGN, "Expected '=' after var declaration");
        Expr *initializer = parse_expression(parser);
        Stmt *stmt = make_stmt(STMT_VAR_DECL);
        stmt->data.var_decl.is_const = false;
        stmt->data.var_decl.name = name;
        stmt->data.var_decl.initializer = initializer;
        return stmt;
    }

    if (is_const) {
        if (is_type_hint_start(parser)) {
            skip_type_hint(parser);
        }
        Token *name_tok = expect(parser, TOK_IDENTIFIER, "Expected variable name");
        name = strdup(name_tok ? name_tok->value : "");
        if (match(parser, TOK_COLON)) {
            advance(parser);
            skip_type_hint(parser);
        }
    } else {
        skip_type_hint(parser);
        Token *name_tok = expect(parser, TOK_IDENTIFIER, "Expected variable name");
        name = strdup(name_tok ? name_tok->value : "");
        if (match(parser, TOK_COLON)) {
            advance(parser);
            skip_type_hint(parser);
        }
    }

    Expr *initializer = NULL;
    if (match(parser, TOK_ASSIGN)) {
        advance(parser);
        initializer = parse_expression(parser);
    }

    Stmt *stmt = make_stmt(STMT_VAR_DECL);
    stmt->data.var_decl.is_const = is_const;
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
        if (match(parser, TOK_COLON)) {
            advance(parser);
        }
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

    if (is_declaration_start(parser)) {
        Decl *decl = parse_declaration(parser);
        if (decl) {
            Stmt *stmt = make_stmt(STMT_DECLARATION);
            stmt->data.declaration.decl = decl;
            return stmt;
        }
        return NULL;
    }

    if (match(parser, TOK_CONST)) {
        advance(parser);
        return parse_var_decl(parser, true, false);
    }

    if (match(parser, TOK_VAR)) {
        advance(parser);
        return parse_var_decl(parser, false, true);
    }

    if (is_type_hint_start(parser)) {
        return parse_var_decl(parser, false, false);
    }

    if (match(parser, TOK_IF)) return parse_if_statement(parser);
    if (match(parser, TOK_WHILE)) return parse_while_statement(parser);
    if (match(parser, TOK_FOR)) return parse_for_statement(parser);
    if (match(parser, TOK_SWITCH)) return parse_switch_statement(parser);
    if (match(parser, TOK_THROW)) return parse_throw_statement(parser);
    if (match(parser, TOK_TRY)) return parse_try_statement(parser);
    if (match(parser, TOK_RETURN)) return parse_return_statement(parser);

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

        Token *name = expect(parser, TOK_IDENTIFIER, "Expected parameter name");
        params[*count].name = strdup(name ? name->value : "");

        if (match(parser, TOK_COLON)) {
            advance(parser);
            skip_type_hint(parser);
        }

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

    if (match(parser, TOK_ARROW)) {
        advance(parser);
        skip_type_hint(parser);
    }

    expect(parser, TOK_COLON, "Expected ':' after function declaration");
    expect_newline(parser);

    int body_count = 0;
    Stmt **body = parse_block(parser, &body_count);

    Decl *decl = make_decl(DECL_FUNCTION);
    decl->data.function.name = strdup(name ? name->value : "");
    decl->data.function.params = params;
    decl->data.function.param_count = param_count;
    decl->data.function.body = body;
    decl->data.function.body_count = body_count;
    return decl;
}

static Expr *parse_anonymous_function(Parser *parser) {
    expect(parser, TOK_DEF, "Expected 'def'");

    expect(parser, TOK_LPAREN, "Expected '(' after 'def'");
    int param_count = 0;
    FunctionParam *params = parse_parameters(parser, &param_count);
    expect(parser, TOK_RPAREN, "Expected ')' after parameters");

    if (match(parser, TOK_ARROW)) {
        advance(parser);
        skip_type_hint(parser);
    }

    expect(parser, TOK_COLON, "Expected ':' after anonymous function header");
    expect_newline(parser);

    int body_count = 0;
    Stmt **body = parse_block(parser, &body_count);

    Expr *expr = make_expr(EXPR_FUNCTION);
    expr->data.function.name = NULL;
    expr->data.function.params = params;
    expr->data.function.param_count = param_count;
    expr->data.function.body = body;
    expr->data.function.body_count = body_count;
    return expr;
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
    ClassField *fields = (ClassField *)malloc(field_cap * sizeof(ClassField));
    Decl **methods = (Decl **)malloc(method_cap * sizeof(Decl *));
    int field_count = 0, method_count = 0;

    while (!match2(parser, TOK_DEDENT, TOK_EOF)) {
        if (match(parser, TOK_NEWLINE)) { advance(parser); continue; }

        if (match(parser, TOK_DEF)) {
            if (method_count >= method_cap) { method_cap *= 2; methods = realloc(methods, method_cap * sizeof(Decl *)); }
            methods[method_count++] = parse_function_declaration(parser);
            if (match(parser, TOK_NEWLINE)) advance(parser);
        } else if (match(parser, TOK_VAR) || match(parser, TOK_IDENTIFIER)) {
            if (field_count >= field_cap) { field_cap *= 2; fields = realloc(fields, field_cap * sizeof(ClassField)); }
            if (match(parser, TOK_VAR)) advance(parser);
            Token *fname = expect(parser, TOK_IDENTIFIER, "Expected field name");
            fields[field_count].name = strdup(fname ? fname->value : "");
            if (match(parser, TOK_COLON)) {
                advance(parser);
                skip_type_hint(parser);
            }
            field_count++;
            expect_newline(parser);
        } else {
            parser_error(parser, "Expected field name or method definition in class body");
            advance(parser); /* skip unknown token to avoid infinite loop */
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
    return t == TOK_DEF || t == TOK_CLASS ||
           t == TOK_IMPORT || t == TOK_FROM || t == TOK_ENUM;
}

static Decl *parse_declaration(Parser *parser) {
    if (match(parser, TOK_DEF)) return parse_function_declaration(parser);
    if (match(parser, TOK_CLASS)) return parse_class_declaration(parser);
    if (match(parser, TOK_ENUM)) return parse_enum_declaration(parser);
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
    program->statements = (Stmt **)malloc(stmt_capacity * sizeof(Stmt *));

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
