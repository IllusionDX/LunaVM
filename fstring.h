/* F-string desugaring for Luna interpreter.
 * Internal header — not part of the public API.
 */

#ifndef LUNA_FSTRING_H
#define LUNA_FSTRING_H

#include "ast.h"

Expr *desugar_fstring(const char *template);

#endif /* LUNA_FSTRING_H */
