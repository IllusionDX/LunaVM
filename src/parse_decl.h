/* Declaration parsing for Luna interpreter.
 * Internal header — not part of the public API.
 */

#ifndef LUNA_PARSE_DECL_H
#define LUNA_PARSE_DECL_H

#include "ast.h"

/* Forward declaration */
typedef struct Parser Parser;

Decl *parse_declaration(Parser *parser);
bool is_declaration_start(Parser *parser);
FunctionParam *parse_parameters(Parser *parser, int *count);

#endif /* LUNA_PARSE_DECL_H */
