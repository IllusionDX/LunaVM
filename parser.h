/* Native C Parser for Luna Programming Language.
 * Converts tokens into an Abstract Syntax Tree.
 */

#ifndef LUNA_PARSER_H
#define LUNA_PARSER_H

#include <stdarg.h>
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

/* ============== Parser functions ============== */

/* Create a new parser */
Parser *parser_new(TokenList *tokens);

/* Free parser resources */
void parser_free(Parser *parser);

/* Parse the entire program */
Program *parser_parse(Parser *parser);

/* ============== Error handling ============== */

/* Variadic error reporter — always includes the current line number */
void parser_error(Parser *parser, const char *fmt, ...);

#endif /* LUNA_PARSER_H */
