/* Main entry point for standalone Luna interpreter.
 * Can read Luna source files and execute them.
 *
 * Usage:
 *   luna <source_file.luna>
 *   luna --version
 *   luna --help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "eval.h"
#include "lexer.h"
#include "parser.h"
#include "analyzer.h"

static void print_usage(const char *program) {
    fprintf(stderr, "Usage: %s [options] <source_file>\n", program);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -u          Unbuffered stdout/stderr\n");
    fprintf(stderr, "  --version   Show version information\n");
    fprintf(stderr, "  --help      Show this help message\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s program.luna    # Run Luna source file\n", program);
    fprintf(stderr, "  %s -u program.luna # Run with unbuffered output\n", program);
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "Could not open file: %s\n", path);
        return NULL;
    }

    fseek(file, 0L, SEEK_END);
    size_t file_size = ftell(file);
    rewind(file);

    char *buffer = (char *)malloc(file_size + 1);
    if (!buffer) {
        fprintf(stderr, "Not enough memory to read file: %s\n", path);
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, file_size, file);
    buffer[bytes_read] = '\0';
    fclose(file);

    return buffer;
}

static int execute_native_program(const char *source) {
    Lexer *lexer = lexer_new(source);
    TokenList *tokens = lexer_tokenize(lexer);

    if (!tokens) {
        fprintf(stderr, "Lexer error: Failed to tokenize source\n");
        lexer_free(lexer);
        return 1;
    }

    Parser *parser = parser_new(tokens);
    Program *program = parser_parse(parser);

    if (!program || parser->had_error) {
        fprintf(stderr, "Parser error: Failed to parse source\n");
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }

    Analyzer *analyzer = analyzer_new();
    AnalyzerResult result = analyze_program(analyzer, program);

    if (result != ANALYZER_OK) {
        fprintf(stderr, "Semantic error at line %d: %s\n",
                analyzer_get_error_line(analyzer),
                analyzer_get_error(analyzer));
        analyzer_free(analyzer);
        free_program(program);
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }

    analyzer_free(analyzer);

    execute_program(program);

    free_program(program);
    parser_free(parser);
    token_list_free(tokens);
    lexer_free(lexer);

    return 0;
}

int main(int argc, char *argv[]) {
    int unbuffered = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) {
            unbuffered = 1;
        }
    }

    if (unbuffered) {
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    int file_idx = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) continue;

        if (strcmp(argv[i], "--version") == 0) {
            printf("Luna interpreter v1.0.0\n");
            printf("Tree-walking interpreter with ARC memory management\n");
            printf("Native C lexer and parser\n");
            return 0;
        }

        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (argv[i][0] != '-') {
            file_idx = i;
        }
    }

    if (file_idx >= argc) {
        fprintf(stderr, "Error: No input file specified\n");
        print_usage(argv[0]);
        return 1;
    }

    char *source = read_file(argv[file_idx]);
    if (!source) {
        return 1;
    }

    int result = execute_native_program(source);
    free(source);
    return result;
}