/* F-string desugaring for Luna interpreter.
 * Converts an f-string template into a tree of concatenation expressions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "fstring.h"
#include "parse_expr.h"

/* Forward declarations for lexer functions used by the sub-parser */
typedef struct Lexer Lexer;
Lexer *lexer_new(const char *source);
void lexer_free(Lexer *lexer);
TokenList *lexer_tokenize(Lexer *lexer);

/* ============== F-string part concatenation ============== */

static Expr *append_fstring_part(Expr *current, Expr *part) {
    if (!current) {
        if (part->kind != EXPR_STRING) {
            Expr *empty = make_expr(EXPR_STRING);
            empty->data.string.value = strdup("");
            empty->data.string.length = 0;
            Expr *bin = make_expr(EXPR_BINARY);
            bin->data.binary.left = empty;
            bin->data.binary.operator = strdup("+");
            bin->data.binary.right = part;
            return bin;
        }
        return part;
    }

    Expr *bin = make_expr(EXPR_BINARY);
    bin->data.binary.left = current;
    bin->data.binary.operator = strdup("+");
    bin->data.binary.right = part;
    return bin;
}

/* ============== Sub-expression parsing for f-string interpolation ============== */

static Expr *parse_sub_expression(const char *source) {
    Lexer *sub_lexer = lexer_new(source);
    TokenList *sub_tokens = lexer_tokenize(sub_lexer);
    Parser *sub_parser = parser_new(sub_tokens);
    Program *sub_program = parser_parse(sub_parser);

    Expr *result = NULL;
    if (sub_program && !sub_parser->had_error && sub_program->stmt_count > 0) {
        Stmt *stmt = sub_program->statements[0];
        if (stmt->kind == STMT_EXPRESSION && stmt->data.expression.expression) {
            result = stmt->data.expression.expression;
            stmt->data.expression.expression = NULL;
        }
    }

    if (sub_program) free_program(sub_program);
    parser_free(sub_parser);
    token_list_free(sub_tokens);
    lexer_free(sub_lexer);

    return result;
}

/* ============== F-string desugaring ============== */

Expr *desugar_fstring_len(const char *template, int len) {
    int i = 0;
    Expr *result = NULL;

    while (i < len) {
        int brace_start = -1;
        for (int j = i; j < len; j++) {
            if (template[j] == '{') {
                brace_start = j;
                break;
            }
        }

        int text_len = (brace_start == -1) ? len - i : brace_start - i;
        if (text_len > 0) {
            Expr *text = make_expr(EXPR_STRING);
            text->data.string.value = malloc(text_len + 1);
            memcpy(text->data.string.value, template + i, text_len);
            text->data.string.value[text_len] = '\0';
            text->data.string.length = text_len;
            result = append_fstring_part(result, text);
        }

        if (brace_start == -1) break;

        int depth = 1;
        int brace_end = -1;
        for (int j = brace_start + 1; j < len; j++) {
            if (template[j] == '{') depth++;
            else if (template[j] == '}') {
                depth--;
                if (depth == 0) {
                    brace_end = j;
                    break;
                }
            }
        }

        if (brace_end == -1) {
            int rest_len = len - brace_start;
            Expr *text = make_expr(EXPR_STRING);
            text->data.string.value = malloc(rest_len + 1);
            memcpy(text->data.string.value, template + brace_start, rest_len + 1);
            text->data.string.length = rest_len;
            result = append_fstring_part(result, text);
            break;
        }

        int expr_len = brace_end - brace_start - 1;
        char *expr_source = malloc(expr_len + 2);
        memcpy(expr_source, template + brace_start + 1, expr_len);
        expr_source[expr_len] = '\n';
        expr_source[expr_len + 1] = '\0';

        Expr *expr = parse_sub_expression(expr_source);
        free(expr_source);

        if (!expr) {
            if (expr_len == 0) {
                fprintf(stderr, "f-string: empty expression not allowed\n");
            } else {
                fprintf(stderr, "f-string: failed to parse expression '");
                fwrite(template + brace_start + 1, 1, expr_len, stderr);
                fprintf(stderr, "'\n");
            }
            if (result) free_expr(result);
            return NULL;
        }

        result = append_fstring_part(result, expr);
        i = brace_end + 1;
    }

    if (!result) {
        result = make_expr(EXPR_STRING);
        result->data.string.value = strdup("");
        result->data.string.length = 0;
    }

    return result;
}

Expr *desugar_fstring(const char *template) {
    return desugar_fstring_len(template, (int)strlen(template));
}
