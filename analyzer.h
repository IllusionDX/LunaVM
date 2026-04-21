/* Semantic Analyzer for Luna programming language.
 * Performs type checking, scope management, and optimizations
 * before execution.
 */

#ifndef LUNA_ANALYZER_H
#define LUNA_ANALYZER_H

#include "ast.h"
#include "environment.h"

typedef struct Analyzer Analyzer;

typedef enum {
    ANALYZER_OK,
    ANALYZER_ERROR_UNDEFINED_IDENTIFIER,
    ANALYZER_ERROR_TYPE_MISMATCH,
    ANALYZER_ERROR_INVALID_OPERATION,
    ANALYZER_ERROR_AMBIGUOUS_TYPE,
    ANALYZER_ERROR_INVALID_ARGUMENT_COUNT,
    ANALYZER_ERROR_INVALID_RETURN_TYPE,
    ANALYZER_ERROR_REDEFINITION
} AnalyzerResult;

Analyzer *analyzer_new(void);
void analyzer_free(Analyzer *analyzer);
AnalyzerResult analyze_program(Analyzer *analyzer, Program *program);

const char *analyzer_get_error(Analyzer *analyzer);
int analyzer_get_error_line(Analyzer *analyzer);
const char *analyzer_get_error_file(Analyzer *analyzer);

Type *duplicate_type(Type *type);
bool types_equal(Type *a, Type *b);
bool is_object_type(Type *type);
bool is_primitive_type(Type *type);
int get_type_size(Type *type);

#endif /* LUNA_ANALYZER_H */