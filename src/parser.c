/* Native C Parser for Luna Programming Language.
 * Converts tokens into an Abstract Syntax Tree.
 * Dynamic VM variant — type annotations are optional hints.
 *
 * This file contains shared helper functions and the public API.
 * Expression, statement, declaration, and f-string parsing are
 * in separate modules: parse_expr.c, parse_stmt.c, parse_decl.c, fstring.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "parser.h"
#include "parse_expr.h"
#include "parse_stmt.h"
#include "parse_decl.h"

/* ============== Helper functions ============== */

Token *peek(Parser *parser) {
    return parser->current;
}

Token *advance(Parser *parser) {
    Token *prev = parser->current;
    if (parser->current->type != TOK_EOF) {
        parser->current = parser->current->next;
    }
    return prev;
}

Token *peek_ahead(Parser *parser, int n) {
    Token *current = parser->current;
    for (int i = 0; i < n && current != NULL && current->type != TOK_EOF; i++) {
        current = current->next;
    }
    return current;
}

bool match(Parser *parser, TokenType type) {
    return peek(parser)->type == type;
}

bool match2(Parser *parser, TokenType t1, TokenType t2) {
    return peek(parser)->type == t1 || peek(parser)->type == t2;
}

Token *expect(Parser *parser, TokenType type, const char *msg) {
    if (!match(parser, type)) {
        fprintf(stderr, "Parse error at line %d col %d: %s (got %s)\n",
                peek(parser)->line, peek(parser)->column, msg,
                token_type_name(peek(parser)->type));
        parser->had_error = 1;
        return peek(parser);
    }
    return advance(parser);
}

void skip_newlines(Parser *parser) {
    while (match_eol(parser)) {
        advance(parser);
    }
}

void skip_whitespace_in_literal(Parser *parser) {
    while (match_eol(parser) || match(parser, TOK_INDENT) || match(parser, TOK_DEDENT)) {
        advance(parser);
    }
}

void expect_newline(Parser *parser) {
    if (match_eol(parser)) {
        advance(parser);
    }
}

/* ============== Backtracking helpers ============== */

ParserState save_state(Parser *parser) {
    ParserState s;
    s.current   = parser->current;
    s.had_error = parser->had_error;
    return s;
}

void restore_state(Parser *parser, ParserState s) {
    parser->current   = s.current;
    parser->had_error = s.had_error;
}

/* ============== Memory allocation helpers ============== */

Expr *make_expr(ExprKind kind, int line) {
    Expr *expr = (Expr *)calloc(1, sizeof(Expr));
    if (!expr) return NULL;
    expr->kind = kind;
    expr->line = line;
    return expr;
}

Stmt *make_stmt(StmtKind kind, int line) {
    Stmt *stmt = (Stmt *)calloc(1, sizeof(Stmt));
    if (!stmt) return NULL;
    stmt->kind = kind;
    stmt->line = line;
    return stmt;
}

Decl *make_decl(DeclKind kind, int line) {
    Decl *decl = (Decl *)calloc(1, sizeof(Decl));
    if (!decl) return NULL;
    decl->kind = kind;
    decl->line = line;
    return decl;
}

/* ============== Type hint tokens ============== */

bool is_type_hint_token(TokenType t) {
    return t == TOK_IDENTIFIER;
}

bool is_type_hint_start(Parser *parser) {
    Token *tok = peek(parser);
    if (!is_type_hint_token(tok->type)) return false;

    int offset = 1;
    Token *next = peek_ahead(parser, offset);
    if (tok->type == TOK_IDENTIFIER &&
        (strcmp(tok->value, "list") == 0 || strcmp(tok->value, "dict") == 0)) {
        if (next && next->type == TOK_LT) {
            int depth = 1;
            offset++;
            next = peek_ahead(parser, offset);
            while (next && depth > 0) {
                if (next->type == TOK_LT) depth++;
                else if (next->type == TOK_GT) depth--;
                offset++;
                next = peek_ahead(parser, offset);
            }
        }
    }
    if (next && next->type == TOK_LBRACKET) {
        offset++;
        next = peek_ahead(parser, offset);
        if (next && next->type == TOK_INTEGER_LITERAL) {
            offset++;
            next = peek_ahead(parser, offset);
        }
        if (next && next->type == TOK_RBRACKET) {
            offset++;
            next = peek_ahead(parser, offset);
        }
    }

    if (next && next->type == TOK_COLON) {
        offset++;
        Token *after_colon = peek_ahead(parser, offset);
        while (after_colon && (after_colon->type == TOK_NEWLINE || after_colon->type == TOK_SEMICOLON || after_colon->type == TOK_INDENT || after_colon->type == TOK_DEDENT)) {
            offset++;
            after_colon = peek_ahead(parser, offset);
        }
        if (after_colon && after_colon->type == TOK_IDENTIFIER) {
            offset++;
            Token *after_name = peek_ahead(parser, offset);
            if (after_name && (after_name->type == TOK_ASSIGN || after_name->type == TOK_NEWLINE ||
                               after_name->type == TOK_SEMICOLON ||
                               after_name->type == TOK_COLON || after_name->type == TOK_EOF)) {
                return true;
            }
        }
    }

    if (next && next->type == TOK_IDENTIFIER) {
        offset++;
        Token *after_name = peek_ahead(parser, offset);
        if (after_name && (after_name->type == TOK_ASSIGN || after_name->type == TOK_NEWLINE ||
                           after_name->type == TOK_SEMICOLON ||
                           after_name->type == TOK_COLON || after_name->type == TOK_EOF ||
                           after_name->type == TOK_COMMA || after_name->type == TOK_RPAREN)) {
            return true;
        }
    }

    return false;
}

void skip_type_hint(Parser *parser) {
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

/* ============== Pattern validation ============== */

bool is_valid_pattern(Expr *expr) {
    if (!expr) return false;
    if (expr->kind == EXPR_LIST_LITERAL) {
        for (int i = 0; i < expr->data.list_literal.element_count; i++) {
            Expr *e = expr->data.list_literal.elements[i];
            if (e->kind != EXPR_IDENTIFIER) return false;
        }
        return true;
    }
    if (expr->kind == EXPR_DICT_LITERAL) {
        for (int i = 0; i < expr->data.dict_literal.entry_count; i++) {
            Expr *v = expr->data.dict_literal.entries[i].value;
            if (v->kind != EXPR_IDENTIFIER) return false;
        }
        return true;
    }
    return false;
}

/* ============== Public API ============== */

Parser *parser_new(TokenList *tokens, const char *source, const char *filepath) {
    Parser *parser = (Parser *)malloc(sizeof(Parser));
    if (!parser) return NULL;
    parser->tokens = tokens;
    parser->current = tokens->head;
    parser->had_error = 0;
    parser->source = source;
    parser->filepath = filepath;
    return parser;
}

void parser_free(Parser *parser) {
    if (parser) free(parser);
}

void parser_error(Parser *parser, const char *fmt, ...) {
    Token *tok = peek(parser);
    int line = tok ? tok->line : 0;
    int col = tok ? tok->column : 0;
    const char *file = parser->filepath ? parser->filepath : "<unknown>";

    fprintf(stderr, "%s:%d:%d: error: ", file, line, col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");

    if (parser->source && line > 0) {
        const char *line_start = parser->source;
        for (int i = 1; i < line && *line_start; i++) {
            while (*line_start && *line_start != '\n') line_start++;
            if (*line_start == '\n') line_start++;
        }
        const char *line_end = line_start;
        while (*line_end && *line_end != '\n') line_end++;

        int line_len = (int)(line_end - line_start);
        fprintf(stderr, "    ");
        fwrite(line_start, 1, line_len, stderr);
        fprintf(stderr, "\n");

        fprintf(stderr, "    ");
        for (int i = 0; i < col && i < line_len; i++) {
            fputc(line_start[i] == '\t' ? '\t' : ' ', stderr);
        }
        fprintf(stderr, "^\n");
    }

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
