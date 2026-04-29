/* Native C Parser for Luna Programming Language.
 * Converts tokens into an Abstract Syntax Tree.
 */

#ifndef LUNA_PARSER_H
#define LUNA_PARSER_H

#include <stdarg.h>
#include <stdbool.h>
#include "lexer.h"
#include "ast.h"

/* Parser state */
typedef struct Parser {
    TokenList *tokens;
    Token *current;
    int had_error;
} Parser;

/* Snapshot used by save_state / restore_state for backtracking */
typedef struct {
    Token *current;
    int    had_error;
} ParserState;

/* ============== Public API ============== */

/* Create a new parser */
Parser *parser_new(TokenList *tokens);

/* Free parser resources */
void parser_free(Parser *parser);

/* Parse the entire program */
Program *parser_parse(Parser *parser);

/* Variadic error reporter — always includes the current line number */
void parser_error(Parser *parser, const char *fmt, ...);

/* ============== Shared helpers (used across parser sub-modules) ============== */

Token *peek(Parser *parser);
Token *advance(Parser *parser);
Token *peek_ahead(Parser *parser, int n);
bool match(Parser *parser, TokenType type);
bool match2(Parser *parser, TokenType t1, TokenType t2);
Token *expect(Parser *parser, TokenType type, const char *msg);
void skip_newlines(Parser *parser);
void skip_whitespace_in_literal(Parser *parser);
void expect_newline(Parser *parser);

ParserState save_state(Parser *parser);
void restore_state(Parser *parser, ParserState s);

Expr *make_expr(ExprKind kind, int line);
Stmt *make_stmt(StmtKind kind, int line);
Decl *make_decl(DeclKind kind, int line);

bool is_type_hint_token(TokenType t);
bool is_type_hint_start(Parser *parser);
void skip_type_hint(Parser *parser);
bool is_valid_pattern(Expr *expr);

#endif /* LUNA_PARSER_H */
