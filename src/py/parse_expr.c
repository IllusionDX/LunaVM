/* Expression parsing for Luna interpreter.
 * Functions for parsing all expression forms: primaries, postfix,
 * unary, binary, and assignment expressions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "parse_expr.h"
#include "parse_stmt.h"
#include "parse_decl.h"
#include "fstring.h"

/* ============== List literal parsing ============== */

Expr *parse_list_literal(Parser *parser) {
    expect(parser, TOK_LBRACKET, "Expected '['");
    skip_whitespace_in_literal(parser);

    /* Empty list */
    if (match(parser, TOK_RBRACKET)) {
        advance(parser);
        Expr *expr = make_expr(EXPR_LIST_LITERAL, peek(parser)->line);
        expr->data.list_literal.elements = NULL;
        expr->data.list_literal.element_count = 0;
        return expr;
    }

    Expr *first = parse_expression(parser);
    skip_whitespace_in_literal(parser);

    /* List comprehension: [expr for x in iterable if cond] */
    if (match(parser, TOK_FOR)) {
        advance(parser);
        Token *var_tok = expect(parser, TOK_IDENTIFIER, "Expected variable name after 'for'");
        expect(parser, TOK_IN, "Expected 'in' after comprehension variable");
        Expr *iterable = parse_expression(parser);
        skip_whitespace_in_literal(parser);

        Expr *condition = NULL;
        if (match(parser, TOK_IF)) {
            advance(parser);
            condition = parse_expression(parser);
            skip_whitespace_in_literal(parser);
        }

        expect(parser, TOK_RBRACKET, "Expected ']' after list comprehension");

        Expr *expr = make_expr(EXPR_LIST_COMPREHENSION, peek(parser)->line);
        expr->data.list_comprehension.element = first;
        expr->data.list_comprehension.variable = strdup(var_tok ? var_tok->value : "");
        expr->data.list_comprehension.iterable = iterable;
        expr->data.list_comprehension.condition = condition;
        return expr;
    }

    /* Regular list literal */
    int capacity = 4;
    Expr **elements = (Expr **)malloc(capacity * sizeof(Expr *));
    int count = 0;
    elements[count++] = first;
    skip_whitespace_in_literal(parser);
    if (match(parser, TOK_COMMA)) advance(parser);

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

    Expr *expr = make_expr(EXPR_LIST_LITERAL, peek(parser)->line);
    expr->data.list_literal.elements = elements;
    expr->data.list_literal.element_count = count;
    return expr;
}

/* ============== Dict literal parsing ============== */

Expr *parse_dict_literal(Parser *parser) {
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
                parser_error(parser, "Expected ',' or '}' in dict literal");
                break;
            }
        }
    } else {
        advance(parser);
    }

    Expr *expr = make_expr(EXPR_DICT_LITERAL, peek(parser)->line);
    expr->data.dict_literal.entries = entries;
    expr->data.dict_literal.entry_count = count;
    return expr;
}

/* Forward declarations for functions used before their definition */
static Expr *parse_dict_literal_bt(Parser *parser);
static Expr *parse_anonymous_function(Parser *parser);

/* looks_like_dict_literal() has been replaced by backtracking in parse_primary(). */

/* ============== Primary expression parsing ============== */

static Expr *parse_primary(Parser *parser) {
    Token *tok = peek(parser);

    switch (tok->type) {
    case TOK_INTEGER_LITERAL: {
        advance(parser);
        Expr *expr = make_expr(EXPR_INTEGER, peek(parser)->line);
        expr->data.integer.value = strdup(tok->value);
        return expr;
    }
    case TOK_FLOAT_LITERAL: {
        advance(parser);
        Expr *expr = make_expr(EXPR_FLOAT, peek(parser)->line);
        expr->data.float_lit.value = strdup(tok->value);
        return expr;
    }
    case TOK_STRING_LITERAL: {
        advance(parser);
        Expr *expr = make_expr(EXPR_STRING, peek(parser)->line);
        expr->data.string.length = tok->length;
        expr->data.string.value = (char *)malloc((size_t)tok->length + 1);
        if (!expr->data.string.value) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        memcpy(expr->data.string.value, tok->value, (size_t)tok->length);
        expr->data.string.value[tok->length] = '\0';
        return expr;
    }
    case TOK_FSTRING_LITERAL: {
        advance(parser);
        Expr *expr = desugar_fstring_len(tok->value, tok->length);
        if (!expr) {
            parser->had_error = 1;
            return make_expr(EXPR_NULL, peek(parser)->line);
        }
        return expr;
    }
    case TOK_TRUE: {
        advance(parser);
        Expr *expr = make_expr(EXPR_BOOL, peek(parser)->line);
        expr->data.boolean.value = true;
        return expr;
    }
    case TOK_FALSE: {
        advance(parser);
        Expr *expr = make_expr(EXPR_BOOL, peek(parser)->line);
        expr->data.boolean.value = false;
        return expr;
    }
    case TOK_NULL: {
        advance(parser);
        return make_expr(EXPR_NULL, peek(parser)->line);
    }

    case TOK_NEW: {
        advance(parser);
        Token *class_tok = expect(parser, TOK_IDENTIFIER, "Expected class name after 'new'");
        /* Build qualified name (e.g. net.Socket) */
        int name_len = (int)strlen(class_tok->value);
        char *name = (char*)malloc(name_len + 1);
        memcpy(name, class_tok->value, name_len + 1);
        while (match(parser, TOK_DOT)) {
            advance(parser);
            Token *next = expect(parser, TOK_IDENTIFIER, "Expected identifier after '.'");
            int next_len = (int)strlen(next->value);
            char *new_name = (char*)malloc(name_len + 1 + next_len + 1);
            memcpy(new_name, name, name_len);
            new_name[name_len] = '.';
            memcpy(new_name + name_len + 1, next->value, next_len + 1);
            free(name);
            name = new_name;
            name_len = name_len + 1 + next_len;
        }
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

        Expr *expr = make_expr(EXPR_NEW, peek(parser)->line);
        expr->data.new_expr.class_name = name;
        expr->data.new_expr.arguments = args;
        expr->data.new_expr.arg_count = arg_count;
        return expr;
    }

    case TOK_SELF: {
        advance(parser);
        Expr *expr = make_expr(EXPR_IDENTIFIER, peek(parser)->line);
        expr->data.identifier.name = strdup("self");
        return expr;
    }
    case TOK_SUPER: {
        advance(parser);
        Expr *expr = make_expr(EXPR_IDENTIFIER, peek(parser)->line);
        expr->data.identifier.name = strdup("super");
        return expr;
    }

    case TOK_LAMBDA: {
        advance(parser);
        FunctionParam *params = NULL;
        int param_count = 0;
        if (match(parser, TOK_LPAREN)) {
            advance(parser);
            params = parse_parameters(parser, &param_count);
            expect(parser, TOK_RPAREN, "Expected ')' after lambda parameters");
        } else {
            int cap = 4;
            params = malloc(sizeof(FunctionParam) * cap);
            while (match(parser, TOK_IDENTIFIER)) {
                if (param_count >= cap) {
                    cap *= 2;
                    params = realloc(params, sizeof(FunctionParam) * cap);
                }
                Token *pname = advance(parser);
                params[param_count].name = strdup(pname ? pname->value : "");
                params[param_count].default_value = NULL;
                if (match(parser, TOK_ASSIGN)) {
                    advance(parser);
                    params[param_count].default_value = parse_expression(parser);
                }
                param_count++;
                if (match(parser, TOK_COMMA)) advance(parser);
                else break;
            }
        }
        expect(parser, TOK_COLON, "Expected ':' after lambda parameters");
        Expr *body_expr = parse_expression(parser);
        Stmt *ret = make_stmt(STMT_RETURN, peek(parser)->line);
        ret->data.return_stmt.value = body_expr;
        Expr *expr = make_expr(EXPR_FUNCTION, peek(parser)->line);
        expr->data.function.name = NULL;
        expr->data.function.params = params;
        expr->data.function.param_count = param_count;
        expr->data.function.body = malloc(sizeof(Stmt *));
        expr->data.function.body[0] = ret;
        expr->data.function.body_count = 1;
        return expr;
    }

    case TOK_LPAREN: {
        advance(parser);

        /* Lookahead for lambda: (params) => expr */
        int offset = 0;
        Token *t = peek_ahead(parser, offset);
        int is_lambda = 0;

        /* Empty params: () => expr */
        if (t && t->type == TOK_RPAREN) {
            Token *after = peek_ahead(parser, offset + 1);
            if (after && after->type == TOK_LAMBDA) is_lambda = 1;
        } else if (t && t->type == TOK_IDENTIFIER) {
            /* Try: id (, id)* ) => */
            is_lambda = 1;
            offset++;
            t = peek_ahead(parser, offset);
            while (t && t->type != TOK_RPAREN && t->type != TOK_EOF) {
                if (t->type == TOK_COMMA) {
                    offset++;
                    t = peek_ahead(parser, offset);
                    if (!t || t->type != TOK_IDENTIFIER) { is_lambda = 0; break; }
                    offset++;
                    t = peek_ahead(parser, offset);
                } else {
                    is_lambda = 0;
                    break;
                }
            }
            if (is_lambda && t && t->type == TOK_RPAREN) {
                Token *after = peek_ahead(parser, offset + 1);
                if (!after || after->type != TOK_LAMBDA) is_lambda = 0;
            } else {
                is_lambda = 0;
            }
        }

        if (is_lambda) {
            int param_count = 0;
            FunctionParam *params = parse_parameters(parser, &param_count);
            expect(parser, TOK_RPAREN, "Expected ')' after lambda parameters");
            advance(parser); /* consume => */
            Expr *body_expr = parse_expression(parser);
            Stmt *ret = make_stmt(STMT_RETURN, peek(parser)->line);
            ret->data.return_stmt.value = body_expr;
            Expr *expr = make_expr(EXPR_FUNCTION, peek(parser)->line);
            expr->data.function.name = NULL;
            expr->data.function.params = params;
            expr->data.function.param_count = param_count;
            expr->data.function.body = malloc(sizeof(Stmt *));
            expr->data.function.body[0] = ret;
            expr->data.function.body_count = 1;
            return expr;
        }

        Expr *expr = parse_expression(parser);
        expect(parser, TOK_RPAREN, "Expected ')' after expression");
        return expr;
    }

    case TOK_LBRACKET:
        return parse_list_literal(parser);

    case TOK_LBRACE:
        return parse_dict_literal_bt(parser);

    case TOK_IDENTIFIER: {
        advance(parser);
        Expr *expr = make_expr(EXPR_IDENTIFIER, peek(parser)->line);
        expr->data.identifier.name = strdup(tok->value);
        return expr;
    }

    case TOK_DEF:
        return parse_anonymous_function(parser);

    default:
        parser_error(parser, "Unexpected token '%s' in expression",
                     token_type_name(tok->type));
        advance(parser);
        return make_expr(EXPR_NULL, peek(parser)->line);
    }
}

/* ============== Backtracking dict-literal parser ============== */
/* Try to parse a dict literal at '{'.  If the first entry does not have a
 * ':' at depth-0 (i.e. it looks like a block, not a dict), restore the
 * parser state and return an error node.  This eliminates the old O(n)
 * looks_like_dict_literal() token-stream scan. */
static Expr *parse_dict_literal_bt(Parser *parser) {
    ParserState snap = save_state(parser);
    advance(parser);  /* consume '{' */
    skip_whitespace_in_literal(parser);

    /* Empty braces -> empty dict */
    if (match(parser, TOK_RBRACE)) {
        advance(parser);
        Expr *expr = make_expr(EXPR_DICT_LITERAL, peek(parser)->line);
        expr->data.dict_literal.entries = malloc(sizeof(DictEntry));
        expr->data.dict_literal.entry_count = 0;
        return expr;
    }

    /* Speculatively parse the first key expression, then look for ':' */
    ParserState before_key = save_state(parser);
    /* Suppress errors while probing */
    int saved_error = parser->had_error;
    parse_expression(parser);          /* consume key candidate */
    skip_whitespace_in_literal(parser);
    bool has_colon = match(parser, TOK_COLON);
    /* Restore to right after '{', ignoring speculative parse */
    restore_state(parser, before_key);
    parser->had_error = saved_error;   /* discard any speculative errors */

    if (!has_colon) {
        /* Not a dict -- restore fully and report */
        restore_state(parser, snap);
        parser_error(parser, "Unexpected '{' in expression context");
        advance(parser);
        return make_expr(EXPR_NULL, peek(parser)->line);
    }

    /* It is a dict: restore to after '{' and call the normal parser */
    restore_state(parser, snap);
    return parse_dict_literal(parser);
}

/* ============== Postfix expression parsing ============== */

static Expr *parse_postfix(Parser *parser) {
    Expr *expr = parse_primary(parser);

    while (1) {
        if (match(parser, TOK_LPAREN)) {
            advance(parser);
            int capacity = 4;
            Expr **args = (Expr **)malloc(capacity * sizeof(Expr *));
            char **arg_names = (char **)malloc(capacity * sizeof(char *));
            int arg_count = 0;
            bool seen_kwarg = false;

            while (!match(parser, TOK_RPAREN) && !match(parser, TOK_EOF)) {
                if (arg_count >= capacity) { capacity *= 2; args = realloc(args, capacity * sizeof(Expr *)); arg_names = realloc(arg_names, capacity * sizeof(char *)); }
                /* Check for keyword argument: identifier = expr */
                Token *next = peek(parser);
                Token *after = peek_ahead(parser, 1);
                if (next && next->type == TOK_IDENTIFIER && after && after->type == TOK_ASSIGN) {
                    if (seen_kwarg == false) seen_kwarg = true;
                    arg_names[arg_count] = strdup(next->value);
                    advance(parser); /* consume identifier */
                    advance(parser); /* consume = */
                    args[arg_count] = parse_expression(parser);
                } else {
                    if (seen_kwarg) {
                        parser_error(parser, "Positional argument after keyword argument");
                    }
                    arg_names[arg_count] = NULL;
                    args[arg_count] = parse_expression(parser);
                }
                arg_count++;
                if (match(parser, TOK_COMMA)) advance(parser);
                else break;
            }
            expect(parser, TOK_RPAREN, "Expected ')' after arguments");

            /* Check for duplicate keyword names */
            if (seen_kwarg) {
                for (int i = 0; i < arg_count; i++) {
                    if (!arg_names[i]) continue;
                    for (int j = i + 1; j < arg_count; j++) {
                        if (arg_names[j] && strcmp(arg_names[i], arg_names[j]) == 0) {
                            parser_error(parser, "Duplicate keyword argument '%s'", arg_names[i]);
                        }
                    }
                }
            }

            Expr *call = make_expr(EXPR_CALL, peek(parser)->line);
            call->data.call.callee = expr;
            call->data.call.arguments = args;
            call->data.call.arg_count = arg_count;
            call->data.call.arg_names = arg_names;
            expr = call;
        }
        else if (match(parser, TOK_DOT)) {
            advance(parser);
            Token *field = expect(parser, TOK_IDENTIFIER, "Expected field name after '.'");
            Expr *access = make_expr(EXPR_FIELD_ACCESS, peek(parser)->line);
            access->data.field_access.obj = expr;
            access->data.field_access.field = strdup(field ? field->value : "");
            access->data.field_access.optional = false;
            expr = access;
        }
        else if (match(parser, TOK_LBRACKET)) {
            advance(parser);
            if (match(parser, TOK_COLON)) {
                /* Slice with omitted start: [:stop] or [:stop:step] */
                advance(parser);
                Expr *slice = make_expr(EXPR_SLICE, peek(parser)->line);
                slice->data.slice.obj = expr;
                slice->data.slice.start = NULL;
                slice->data.slice.stop = NULL;
                slice->data.slice.step = NULL;
                slice->data.slice.optional = false;
                if (!match(parser, TOK_COLON) && !match(parser, TOK_RBRACKET)) {
                    slice->data.slice.stop = parse_expression(parser);
                }
                if (match(parser, TOK_COLON)) {
                    advance(parser);
                    if (!match(parser, TOK_RBRACKET)) {
                        slice->data.slice.step = parse_expression(parser);
                    }
                }
                expect(parser, TOK_RBRACKET, "Expected ']' after slice");
                expr = slice;
            } else {
                Expr *first = parse_expression(parser);
                if (match(parser, TOK_COLON)) {
                    /* Slice with start: [start:stop] or [start:stop:step] */
                    advance(parser);
                    Expr *slice = make_expr(EXPR_SLICE, peek(parser)->line);
                    slice->data.slice.obj = expr;
                    slice->data.slice.start = first;
                    slice->data.slice.stop = NULL;
                    slice->data.slice.step = NULL;
                    slice->data.slice.optional = false;
                    if (!match(parser, TOK_COLON) && !match(parser, TOK_RBRACKET)) {
                        slice->data.slice.stop = parse_expression(parser);
                    }
                    if (match(parser, TOK_COLON)) {
                        advance(parser);
                        if (!match(parser, TOK_RBRACKET)) {
                            slice->data.slice.step = parse_expression(parser);
                        }
                    }
                    expect(parser, TOK_RBRACKET, "Expected ']' after slice");
                    expr = slice;
                } else {
                    /* Regular index access */
                    Expr *index_node = make_expr(EXPR_INDEX_ACCESS, peek(parser)->line);
                    index_node->data.index_access.obj = expr;
                    index_node->data.index_access.index = first;
                    index_node->data.index_access.optional = false;
                    expect(parser, TOK_RBRACKET, "Expected ']' after index");
                    expr = index_node;
                }
            }
        }
        else {
            break;
        }
    }

    return expr;
}

/* ============== Unary expression parsing ============== */

static Expr *parse_unary(Parser *parser);

/* Python `power`: atom_expr ['**' factor] — right-associative and binds
 * tighter than unary minus so `-2**2` == `-(2**2)`. The right operand goes
 * through `parse_unary` (factor), which recurses back here for `2**3**2`. */
static Expr *parse_power(Parser *parser) {
    Expr *left = parse_postfix(parser);
    if (match(parser, TOK_STAR_STAR)) {
        Token *op = advance(parser);
        Expr *right = parse_unary(parser);
        Expr *expr = make_expr(EXPR_BINARY, peek(parser)->line);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_unary(Parser *parser) {
    if (match(parser, TOK_MINUS) || match(parser, TOK_NOT) ||
        match(parser, TOK_TILDE) || match(parser, TOK_PLUS)) {
        Token *op = advance(parser);
        Expr *operand = parse_unary(parser);
        Expr *expr = make_expr(EXPR_UNARY, peek(parser)->line);
        expr->data.unary.operator = strdup(op->value);
        expr->data.unary.operand = operand;
        return expr;
    }
    return parse_power(parser);
}

/* ============== Binary expression parsing (precedence climbing) ============== */

static Expr *parse_multiplicative(Parser *parser) {
    Expr *left = parse_unary(parser);
    while (match(parser, TOK_STAR) || match(parser, TOK_SLASH) || match(parser, TOK_SLASH_SLASH) || match(parser, TOK_PERCENT)) {
        Token *op = advance(parser);
        Expr *right = parse_unary(parser);
        Expr *expr = make_expr(EXPR_BINARY, peek(parser)->line);
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
        Expr *expr = make_expr(EXPR_BINARY, peek(parser)->line);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_shift(Parser *parser) {
    Expr *left = parse_additive(parser);
    while (match(parser, TOK_LSHIFT) || match(parser, TOK_RSHIFT)) {
        Token *op = advance(parser);
        Expr *right = parse_additive(parser);
        Expr *expr = make_expr(EXPR_BINARY, peek(parser)->line);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_bitand(Parser *parser) {
    Expr *left = parse_shift(parser);
    while (match(parser, TOK_AMPERSAND)) {
        Token *op = advance(parser);
        Expr *right = parse_shift(parser);
        Expr *expr = make_expr(EXPR_BINARY, peek(parser)->line);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_bitxor(Parser *parser) {
    Expr *left = parse_bitand(parser);
    while (match(parser, TOK_CARET)) {
        Token *op = advance(parser);
        Expr *right = parse_bitand(parser);
        Expr *expr = make_expr(EXPR_BINARY, peek(parser)->line);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_bitor(Parser *parser) {
    Expr *left = parse_bitxor(parser);
    while (match(parser, TOK_PIPE)) {
        Token *op = advance(parser);
        Expr *right = parse_bitxor(parser);
        Expr *expr = make_expr(EXPR_BINARY, peek(parser)->line);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_comparison(Parser *parser) {
    Expr *left = parse_bitor(parser);
    while (match(parser, TOK_LT) || match(parser, TOK_GT) ||
           match(parser, TOK_LE) || match(parser, TOK_GE)) {
        Token *op = advance(parser);
        Expr *right = parse_bitor(parser);
        Expr *expr = make_expr(EXPR_BINARY, peek(parser)->line);
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
        Expr *expr = make_expr(EXPR_BINARY, peek(parser)->line);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_membership(Parser *parser) {
    Expr *left = parse_equality(parser);
    while (1) {
        const char *op_str = NULL;
        if (match(parser, TOK_IN)) {
            op_str = "in";
            advance(parser);
        } else if (match(parser, TOK_NOT)) {
            Token *next = peek_ahead(parser, 1);
            if (next && next->type == TOK_IN) {
                advance(parser);
                advance(parser);
                op_str = "not in";
            } else {
                break;
            }
        } else if (match(parser, TOK_IS)) {
            advance(parser);
            if (match(parser, TOK_NOT)) {
                advance(parser);
                op_str = "is not";
            } else {
                op_str = "is";
            }
        } else {
            break;
        }
        Expr *right = parse_equality(parser);
        Expr *expr = make_expr(EXPR_BINARY, peek(parser)->line);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op_str);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_and(Parser *parser) {
    Expr *left = parse_membership(parser);
    while (match(parser, TOK_AND)) {
        Token *op = advance(parser);
        Expr *right = parse_membership(parser);
        Expr *expr = make_expr(EXPR_BINARY, peek(parser)->line);
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
        Expr *expr = make_expr(EXPR_BINARY, peek(parser)->line);
        expr->data.binary.left = left;
        expr->data.binary.operator = strdup(op->value);
        expr->data.binary.right = right;
        left = expr;
    }
    return left;
}

static Expr *parse_ternary(Parser *parser) {
    Expr *then_expr = parse_or(parser);
    if (match(parser, TOK_IF)) {
        ParserState state = save_state(parser);
        advance(parser); /* consume 'if' */
        Expr *condition = parse_or(parser);
        if (match(parser, TOK_ELSE)) {
            advance(parser); /* consume 'else' */
            Expr *else_expr = parse_ternary(parser);
            Expr *expr = make_expr(EXPR_TERNARY, peek(parser)->line);
            expr->data.ternary.then_expr = then_expr;
            expr->data.ternary.condition = condition;
            expr->data.ternary.else_expr = else_expr;
            return expr;
        }
        /* No 'else' found — not a ternary, restore and return then_expr */
        restore_state(parser, state);
    }
    return then_expr;
}

/* ============== Assignment expression parsing ============== */

static bool is_multi_assignment(Parser *parser) {
    if (!match(parser, TOK_COMMA)) return false;
    int offset = 1;
    while (true) {
        Token *t1 = peek_ahead(parser, offset);
        if (!t1 || t1->type != TOK_IDENTIFIER) return false;
        offset++;

        Token *t2 = peek_ahead(parser, offset);
        if (!t2) return false;
        if (t2->type == TOK_ASSIGN || t2->type == TOK_PLUS_ASSIGN ||
            t2->type == TOK_MINUS_ASSIGN || t2->type == TOK_STAR_ASSIGN ||
            t2->type == TOK_SLASH_ASSIGN || t2->type == TOK_STAR_STAR_ASSIGN ||
            t2->type == TOK_SLASH_SLASH_ASSIGN) {
            return true;
        }
        if (t2->type != TOK_COMMA) return false;
        offset++;
    }
}

static Expr *parse_assignment(Parser *parser) {
    Expr *first = parse_ternary(parser);

    /* Multi-target assignment (a, b = c, d). */
    if (first->kind == EXPR_IDENTIFIER && is_multi_assignment(parser)) {
        advance(parser); /* consume comma */

        int target_cap = 4;
        int target_count = 1;
        Expr **targets = (Expr **)malloc(target_cap * sizeof(Expr *));
        targets[0] = first;

        targets[target_count++] = parse_or(parser);

        while (match(parser, TOK_COMMA)) {
            advance(parser);
            if (target_count >= target_cap) {
                target_cap *= 2;
                targets = (Expr **)realloc(targets, target_cap * sizeof(Expr *));
            }
            targets[target_count++] = parse_or(parser);
        }

        if (match(parser, TOK_ASSIGN)) {
            advance(parser);

            int value_cap = 4;
            int value_count = 0;
            Expr **values = (Expr **)malloc(value_cap * sizeof(Expr *));

            values[value_count++] = parse_ternary(parser);
            while (match(parser, TOK_COMMA)) {
                advance(parser);
                if (value_count >= value_cap) {
                    value_cap *= 2;
                    values = (Expr **)realloc(values, value_cap * sizeof(Expr *));
                }
                values[value_count++] = parse_ternary(parser);
            }

            Expr *multi = make_expr(EXPR_MULTI_ASSIGN, peek(parser)->line);
            multi->data.multi_assign.targets = targets;
            multi->data.multi_assign.target_count = target_count;
            multi->data.multi_assign.values = values;
            multi->data.multi_assign.value_count = value_count;
            return multi;
        }

        if (match(parser, TOK_PLUS_ASSIGN) || match(parser, TOK_MINUS_ASSIGN) ||
            match(parser, TOK_STAR_ASSIGN) || match(parser, TOK_SLASH_ASSIGN) ||
            match(parser, TOK_STAR_STAR_ASSIGN) || match(parser, TOK_SLASH_SLASH_ASSIGN)) {
            parser_error(parser,
                "compound assignment does not support multiple targets");
            for (int i = 1; i < target_count; i++) free_expr(targets[i]);
            free(targets);
            return first;
        }

        /* Should not reach here */
        for (int i = 1; i < target_count; i++) free_expr(targets[i]);
        free(targets);
        return first;
    }

    if (match(parser, TOK_ASSIGN)) {
        advance(parser);
        Expr *value = parse_assignment(parser);

        /* Expression-level destructuring: [a, b] = rhs  or  {"k": v} = rhs */
        if (first->kind == EXPR_LIST_LITERAL || first->kind == EXPR_DICT_LITERAL) {
            if (!is_valid_pattern(first)) {
                parser_error(parser,
                    "Invalid destructuring pattern in assignment (expected identifiers only)");
                free_expr(value);
                return first;
            }
            /* Re-use the var-decl AST node so the compiler handles it for free */
            Stmt *dummy = make_stmt(STMT_VAR_DECL, peek(parser)->line);
            dummy->data.var_decl.is_const = false;
            dummy->data.var_decl.name = NULL;
            dummy->data.var_decl.pattern = first;
            dummy->data.var_decl.initializer = value;
            /* Wrap in a block-expression: emit the pattern assignment,
             * then evaluate to null so it is usable as a statement-expression. */
            Expr *assign = make_expr(EXPR_ASSIGNMENT, peek(parser)->line);
            assign->data.assignment.target = first;
            assign->data.assignment.value  = value;
            free(dummy); /* dummy was just for the comment above; use EXPR_ASSIGNMENT directly */
            return assign;
        }

        Expr *assign = make_expr(EXPR_ASSIGNMENT, peek(parser)->line);
        assign->data.assignment.target = first;
        assign->data.assignment.value = value;
        return assign;
    }
    if (match(parser, TOK_PLUS_ASSIGN) || match(parser, TOK_MINUS_ASSIGN) ||
        match(parser, TOK_STAR_ASSIGN) || match(parser, TOK_SLASH_ASSIGN) ||
        match(parser, TOK_STAR_STAR_ASSIGN) || match(parser, TOK_SLASH_SLASH_ASSIGN)) {
        Token *op = advance(parser);
        Expr *value = parse_assignment(parser);
        Expr *assign = make_expr(EXPR_COMPOUND_ASSIGN, peek(parser)->line);
        assign->data.compound_assign.target = first;
        assign->data.compound_assign.operator = strdup(op->value);
        assign->data.compound_assign.value = value;
        return assign;
    }

    return first;
}

Expr *parse_expression(Parser *parser) {
    return parse_assignment(parser);
}

/* ============== Anonymous function ============== */

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

    int body_count = 0;
    Stmt **body = NULL;
    if (match_eol(parser)) {
        advance(parser);
        body = parse_block(parser, &body_count);
    } else {
        body = (Stmt **)malloc(sizeof(Stmt *));
        body[0] = parse_statement(parser);
        body_count = 1;
    }

    Expr *expr = make_expr(EXPR_FUNCTION, peek(parser)->line);
    expr->data.function.name = NULL;
    expr->data.function.params = params;
    expr->data.function.param_count = param_count;
    expr->data.function.body = body;
    expr->data.function.body_count = body_count;
    return expr;
}
