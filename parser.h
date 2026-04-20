/* Native C Parser for Luna Programming Language.
 * Converts tokens into an Abstract Syntax Tree.
 */

#ifndef LUNA_PARSER_H
#define LUNA_PARSER_H

#include "lexer.h"
#include "ast.h"

/* Parser state */
typedef struct Parser {
    TokenList *tokens;
    Token *current;
    int had_error;
} Parser;

/* ============== Parser functions ============== */

/* Create a new parser */
Parser *parser_new(TokenList *tokens);

/* Free parser resources */
void parser_free(Parser *parser);

/* Parse the entire program */
Program *parser_parse(Parser *parser);

/* ============== Error handling ============== */

void parser_error(Parser *parser, const char *message);

#endif /* LUNA_PARSER_H */
