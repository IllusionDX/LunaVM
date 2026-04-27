/* Statement parsing for Luna interpreter.
 * Functions for parsing all statement forms: variable declarations,
 * if/while/for/switch/throw/try/return, and the top-level statement dispatcher.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "parse_stmt.h"
#include "parse_expr.h"
#include "parse_decl.h"

/* ============== Variable declaration ============== */

static Stmt *parse_var_decl(Parser *parser, bool is_const, bool use_var) {
    char *name = NULL;
    Expr *pattern = NULL;

    if (use_var) {
        if (match(parser, TOK_LBRACKET)) {
            pattern = parse_list_literal(parser);
            if (!is_valid_pattern(pattern)) {
                parser_error(parser,
                    "Invalid destructuring pattern (expected identifiers only)");
            }
            if (match(parser, TOK_COLON)) {
                advance(parser);
                skip_type_hint(parser);
            }
            expect(parser, TOK_ASSIGN, "Expected '=' after var declaration");
            Expr *initializer = parse_expression(parser);
            Stmt *stmt = make_stmt(STMT_VAR_DECL);
            stmt->data.var_decl.is_const = false;
            stmt->data.var_decl.name = NULL;
            stmt->data.var_decl.pattern = pattern;
            stmt->data.var_decl.initializer = initializer;
            return stmt;
        } else if (match(parser, TOK_LBRACE)) {
            pattern = parse_dict_literal(parser);
            if (!is_valid_pattern(pattern)) {
                parser_error(parser,
                    "Invalid destructuring pattern (expected string keys mapped to identifiers)");
            }
            if (match(parser, TOK_COLON)) {
                advance(parser);
                skip_type_hint(parser);
            }
            expect(parser, TOK_ASSIGN, "Expected '=' after var declaration");
            Expr *initializer = parse_expression(parser);
            Stmt *stmt = make_stmt(STMT_VAR_DECL);
            stmt->data.var_decl.is_const = false;
            stmt->data.var_decl.name = NULL;
            stmt->data.var_decl.pattern = pattern;
            stmt->data.var_decl.initializer = initializer;
            return stmt;
        }

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
        stmt->data.var_decl.pattern = NULL;
        stmt->data.var_decl.initializer = initializer;
        return stmt;
    }

    if (is_const) {
        if (match(parser, TOK_LBRACKET) || match(parser, TOK_LBRACE)) {
            if (match(parser, TOK_LBRACKET)) {
                pattern = parse_list_literal(parser);
            } else {
                pattern = parse_dict_literal(parser);
            }
            if (!is_valid_pattern(pattern)) {
                parser_error(parser, "Invalid destructuring pattern");
            }
            if (match(parser, TOK_COLON)) {
                advance(parser);
                skip_type_hint(parser);
            }
            expect(parser, TOK_ASSIGN, "Expected '=' after const declaration");
            Expr *initializer = parse_expression(parser);
            Stmt *stmt = make_stmt(STMT_VAR_DECL);
            stmt->data.var_decl.is_const = true;
            stmt->data.var_decl.name = NULL;
            stmt->data.var_decl.pattern = pattern;
            stmt->data.var_decl.initializer = initializer;
            return stmt;
        }

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
    stmt->data.var_decl.pattern = NULL;
    stmt->data.var_decl.initializer = initializer;
    return stmt;
}

/* ============== Block parsing ============== */

Stmt **parse_block(Parser *parser, int *count) {
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

/* ============== If statement ============== */

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

/* ============== While statement ============== */

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

/* ============== For statement ============== */

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

/* ============== Switch statement ============== */

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

/* ============== Throw statement ============== */

static Stmt *parse_throw_statement(Parser *parser) {
    expect(parser, TOK_THROW, "Expected 'throw'");
    Expr *expr = parse_expression(parser);
    Stmt *stmt = make_stmt(STMT_THROW);
    stmt->data.throw_stmt.expression = expr;
    return stmt;
}

/* ============== Try statement ============== */

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

/* ============== Return statement ============== */

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

/* ============== Statement dispatcher ============== */

Stmt *parse_statement(Parser *parser) {
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
