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
#include <io.h>
#include "ast.h"
#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"
#include "version.h"

static void print_usage(const char *program) {
    fprintf(stderr, "Usage: %s [options] <source_file>\n", program);
    fprintf(stderr, "       %s            # Run interactive REPL\n", program);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -u          Unbuffered stdout/stderr\n");
    fprintf(stderr, "  --version   Show version information\n");
    fprintf(stderr, "  --help      Show this help message\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s program.luna    # Run Luna source file\n", program);
    fprintf(stderr, "  %s -u program.luna # Run with unbuffered output\n", program);
    fprintf(stderr, "  %s                 # Start REPL\n", program);
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

static int execute_native_program(const char *source, const char *filepath, int argc, char *argv[]) {
#ifdef DEBUG
    fprintf(stderr, "DEBUG: execute_native_program start\n");
#endif
    Lexer *lexer = lexer_new(source);
    TokenList *tokens = lexer_tokenize(lexer);

    if (!tokens) {
        fprintf(stderr, "Lexer error: Failed to tokenize source\n");
        lexer_free(lexer);
        return 1;
    }

    /* Check for lexer errors (e.g. unsupported encoding) before parsing */
    Token *t = tokens->head;
    while (t) {
        if (t->type == TOK_ERROR) {
            fprintf(stderr, "Lexer error: %s\n", t->value ? t->value : "Unknown lexer error");
            token_list_free(tokens);
            lexer_free(lexer);
            return 1;
        }
        t = t->next;
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

    VM vm;
    vm_init(&vm);
    vm_set_process_args(&vm, argc, argv);
#ifdef DEBUG
    fprintf(stderr, "DEBUG: vm_init done\n");
#endif

    Chunk chunk;
    if (!compile_program(program, &chunk, &vm, false, false)) {
        fprintf(stderr, "Compiler error: Failed to compile program\n");
        chunk_free(&chunk);
        vm_free(&vm);
        free_program(program);
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }
    chunk.source_path = filepath ? strdup(filepath) : NULL;

#ifdef DEBUG
    fprintf(stderr, "DEBUG: about to run chunk\n");
#endif
    VMResult result = vm_run_chunk(&vm, &chunk);
#ifdef DEBUG
    fprintf(stderr, "DEBUG: vm_run_chunk returned %d\n", result);
#endif
    if (result == VM_EXCEPTION) {
        fprintf(stderr, "Uncaught exception:");
        char trace[2048];
        vm_format_stack_trace(&vm, trace, sizeof(trace));
        fprintf(stderr, "%s", trace);
        char *exc_str = value_to_string(vm.last_exception);
        fprintf(stderr, "\n%s\n", exc_str);
        free(exc_str);
        chunk_free(&chunk);
        vm_free(&vm);
        free_program(program);
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }

    chunk_free(&chunk);
    vm_free(&vm);
    free_program(program);
    parser_free(parser);
    token_list_free(tokens);
    lexer_free(lexer);

    return 0;
}

static int execute_repl_line(VM *vm, const char *source) {
    Lexer *lexer = lexer_new(source);
    TokenList *tokens = lexer_tokenize(lexer);

    if (!tokens) {
        fflush(stdout);
        fprintf(stderr, "Lexer error: Failed to tokenize source\n");
        lexer_free(lexer);
        return 1;
    }

    Parser *parser = parser_new(tokens);
    Program *program = parser_parse(parser);

    if (!program || parser->had_error) {
        fflush(stdout);
        fprintf(stderr, "Parser error: Failed to parse source\n");
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }

    Chunk chunk;
    if (!compile_program(program, &chunk, vm, true, false)) {
        fflush(stdout);
        fprintf(stderr, "Compiler error: Failed to compile program\n");
        chunk_free(&chunk);
        free_program(program);
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }

    VMResult result = vm_run_chunk(vm, &chunk);
    if (result == VM_EXCEPTION) {
        fflush(stdout);
        fprintf(stderr, "Uncaught exception:");
        char trace[2048];
        vm_format_stack_trace(vm, trace, sizeof(trace));
        fprintf(stderr, "%s", trace);
        char *exc_str = value_to_string(vm->last_exception);
        fprintf(stderr, "\n%s\n", exc_str);
        free(exc_str);
        chunk_free(&chunk);
        free_program(program);
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }

    chunk_free(&chunk);
    free_program(program);
    parser_free(parser);
    token_list_free(tokens);
    lexer_free(lexer);

    return 0;
}

static void repl(void) {
    VM vm;
    vm_init(&vm);
    vm_set_process_args(&vm, 0, NULL);

    bool is_tty = _isatty(_fileno(stdin));

    if (is_tty) {
        printf("LunaScript %s REPL\n", LUNA_VERSION_STRING);
        printf("Type 'exit' or press Ctrl+D to quit.\n\n");
    }

    char *buffer = malloc(1);
    if (!buffer) return;
    buffer[0] = '\0';

    while (1) {
        bool has_input = buffer[0] != '\0';
        if (is_tty) {
            printf("%s", has_input ? "... " : ">>> ");
            fflush(stdout);
        }

        char line[1024];
        if (!fgets(line, sizeof(line), stdin)) {
            if (has_input) printf("\n");
            break;
        }

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        if (line[0] == '\0') {
            if (!has_input) {
                continue;
            }
            /* Empty line terminates multi-line input */
        } else {
            size_t buf_len = strlen(buffer);
            size_t line_len = strlen(line);
            char *new_buf = realloc(buffer, buf_len + line_len + 2);
            if (!new_buf) {
                free(buffer);
                buffer = malloc(1);
                if (!buffer) break;
                buffer[0] = '\0';
                continue;
            }
            buffer = new_buf;
            if (buf_len > 0) {
                buffer[buf_len] = '\n';
                memcpy(buffer + buf_len + 1, line, line_len + 1);
            } else {
                memcpy(buffer, line, line_len + 1);
            }

            bool has_newline = strchr(buffer, '\n') != NULL;
            bool ends_with_colon = buffer[0] && buffer[strlen(buffer) - 1] == ':';
            int open_brackets = 0;
            for (char *p = buffer; *p; p++) {
                if (*p == '(' || *p == '[' || *p == '{') open_brackets++;
                else if (*p == ')' || *p == ']' || *p == '}') open_brackets--;
            }

            if (!has_newline) {
                if (ends_with_colon || open_brackets != 0) {
                    continue;
                }
            } else {
                /* Multi-line: require empty line to submit */
                continue;
            }
        }

        /* Execute buffer */
        if (strcmp(buffer, "exit") == 0 || strcmp(buffer, "quit") == 0) {
            break;
        }

        vm_set_global(&vm, "_", make_null(), false);
        int result = execute_repl_line(&vm, buffer);
        if (result == 0) {
            Value last;
            if (vm_get_global(&vm, "_", &last) && !IS_NIL(last)) {
                char *s = value_to_string(last);
                printf("%s\n", s);
                free(s);
                vm_set_global(&vm, "_", make_null(), false);
            }
        }

        free(buffer);
        buffer = malloc(1);
        if (!buffer) break;
        buffer[0] = '\0';
    }

    free(buffer);
    vm_free(&vm);
}

int main(int argc, char *argv[]) {
#ifdef DEBUG
    fprintf(stderr, "DEBUG: main start\n");
#endif
    int unbuffered = 0;
    int file_idx = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) {
            unbuffered = 1;
            continue;
        }
        if (file_idx < 0 && argv[i][0] != '-') {
            file_idx = i;
            break;
        }
    }

    if (unbuffered) {
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    if (argc < 2) {
        repl();
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) continue;
        if (argv[i][0] != '-') break;
        if (strcmp(argv[i], "--version") == 0) {
            printf("LunaScript interpreter %s\n", LUNA_VERSION_STRING);
            printf("Register-based bytecode VM with ARC memory management\n");
            printf("Native C lexer, parser, and compiler\n");
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (file_idx < 0 || file_idx >= argc) {
        fprintf(stderr, "Error: No input file specified\n");
        print_usage(argv[0]);
        return 1;
    }

#ifdef DEBUG
    fprintf(stderr, "DEBUG: about to read file %s\n", argv[file_idx]);
#endif
    char *source = read_file(argv[file_idx]);
    if (!source) {
#ifdef DEBUG
        fprintf(stderr, "DEBUG: read_file failed\n");
#endif
        return 1;
    }
#ifdef DEBUG
    fprintf(stderr, "DEBUG: read_file succeeded, calling execute_native_program\n");
#endif

    int result = execute_native_program(source, argv[file_idx], argc, argv);
    free(source);
    return result;
}
