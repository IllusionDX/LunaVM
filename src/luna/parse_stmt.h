/* Statement parsing for Luna interpreter.
 * Internal header — not part of the public API.
 */

#ifndef LUNA_PARSE_STMT_H
#define LUNA_PARSE_STMT_H

#include "ast.h"

/* Forward declaration */
typedef struct Parser Parser;

Stmt *parse_statement(Parser *parser);
Stmt **parse_block(Parser *parser, int *count);

#endif /* LUNA_PARSE_STMT_H */
