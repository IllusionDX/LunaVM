/* Expression parsing for Luna interpreter.
 * Internal header — not part of the public API.
 */

#ifndef LUNA_PARSE_EXPR_H
#define LUNA_PARSE_EXPR_H

#include "ast.h"

/* Forward declaration */
typedef struct Parser Parser;

Expr *parse_expression(Parser *parser);
Expr *parse_list_literal(Parser *parser);
Expr *parse_dict_literal(Parser *parser);

#endif /* LUNA_PARSE_EXPR_H */
