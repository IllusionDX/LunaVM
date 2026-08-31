/* main.c — Standalone CLI for the Luna VM (language-independent).
 *
 * The language is chosen at link time: the build links one frontend, which
 * exposes `g_frontend` (a FrontendDef).  This file drives the VM purely via
 * that FrontendDef and the embeddable C API (api.h).
 *
 * Usage:
 *   luna <source_file>
 *   luna -e <code>
 *   luna [no args]        # REPL
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

#include "api.h"
#include "vm.h"
#include "chunk.h"
#include "version.h"

extern const FrontendDef g_frontend;

static void print_usage(const char *program) {
    fprintf(stderr, "Usage: %s [options] <source_file>\n", program);
    fprintf(stderr, "       %s -e <code>   # Execute inline code\n", program);
    fprintf(stderr, "       %s            # Run interactive REPL\n", program);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -e <code>   Execute inline source code\n");
    fprintf(stderr, "  -u          Unbuffered stdout/stderr\n");
    fprintf(stderr, "  --dump-bytecode  Print disassembled bytecode\n");
    fprintf(stderr, "  --version   Show version information\n");
    fprintf(stderr, "  --help      Show this help message\n");
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "Could not open file: %s\n", path);
        return NULL;
    }
    fseek(file, 0L, SEEK_END);
    size_t file_size = (size_t)ftell(file);
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

/* Compile source into a callable, then run it.  Returns 0 on success. */
static int run_source(VM *vm, const char *source, const char *path, bool dump_bytecode) {
    Value fn_val;
    const char *err = g_frontend.compile_source
        ? g_frontend.compile_source(vm, source, path, false, &fn_val) : "no compiler";
    if (err) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (dump_bytecode && IS_OBJ(fn_val) && AS_OBJ(fn_val)->type &&
        AS_OBJ(fn_val)->type->get_chunk) {
        chunk_disassemble(AS_OBJ(fn_val)->type->get_chunk(fn_val));
    }

    Value result;
    VMResult r = vm_call_value(vm, fn_val, NULL, 0, &result);
    if (r == VM_EXCEPTION) {
        char trace[2048];
        char *exc_str = value_to_string(vm->last_exception);
        vm_format_stack_trace(vm, trace, sizeof(trace), exc_str);
        fprintf(stderr, "%s\n", trace);
        free(exc_str);
        return 1;
    }
    return 0;
}

static void repl(void) {
    VM vm;
    vm_init(&vm);
    vm_install_frontend(&vm, &g_frontend);
    vm_set_process_args(&vm, 0, NULL);

    bool is_tty = _isatty(_fileno(stdin));
    if (is_tty) {
        printf("%s %s REPL\n", g_frontend.name, LUNA_VERSION_STRING);
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
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        if (line[0] == '\0') {
            if (!has_input) continue;
        } else {
            size_t buf_len = strlen(buffer);
            size_t line_len = strlen(line);
            char *new_buf = realloc(buffer, buf_len + line_len + 2);
            if (!new_buf) { free(buffer); buffer = malloc(1); if (!buffer) break; buffer[0] = '\0'; continue; }
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
                if (ends_with_colon || open_brackets != 0) continue;
            } else {
                continue; /* multi-line: require empty line to submit */
            }
        }

        if (strcmp(buffer, "exit") == 0 || strcmp(buffer, "quit") == 0) break;

        vm_set_global(&vm, "_", make_null(), false);
        Value fn_val;
        if (g_frontend.compile_source &&
            !g_frontend.compile_source(&vm, buffer, NULL, true, &fn_val)) {
            Value result;
            VMResult r = vm_call_value(&vm, fn_val, NULL, 0, &result);
            if (r == VM_EXCEPTION) {
                char trace[2048];
                char *exc_str = value_to_string(vm.last_exception);
                vm_format_stack_trace(&vm, trace, sizeof(trace), exc_str);
                fprintf(stderr, "%s\n", trace);
                free(exc_str);
                vm.frame_count = 0;
                vm.stack_count = 0;
            }
        }

        Value last;
        if (vm_get_global(&vm, "_", &last) && !IS_NIL(last)) {
            char *s = value_to_string(last);
            printf("%s\n", s);
            free(s);
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
    int unbuffered = 0;
    int dump_bytecode = 0;
    int file_idx = -1;
    char *inline_code = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) { unbuffered = 1; continue; }
        if (strcmp(argv[i], "-e") == 0) {
            if (i + 1 < argc) inline_code = argv[++i];
            else { fprintf(stderr, "Error: -e requires an argument\n"); print_usage(argv[0]); return 1; }
            continue;
        }
        if (strcmp(argv[i], "--dump-bytecode") == 0) { dump_bytecode = 1; continue; }
        if (strcmp(argv[i], "--version") == 0) {
            printf("%s interpreter %s\n", g_frontend.name, LUNA_VERSION_STRING);
            printf("Register-based bytecode VM with GC memory management\n");
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0) { print_usage(argv[0]); return 0; }
        if (file_idx < 0 && argv[i][0] != '-') { file_idx = i; break; }
    }

    if (unbuffered) {
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    if (argc < 2) { repl(); return 0; }

    VM vm;
    vm_init(&vm);
    vm_install_frontend(&vm, &g_frontend);

    int result = 0;
    if (inline_code) {
        vm_set_process_args(&vm, argc, argv);
        result = run_source(&vm, inline_code, "<command>", dump_bytecode);
    } else if (file_idx >= 0 && file_idx < argc) {
        char *source = read_file(argv[file_idx]);
        if (!source) { vm_free(&vm); return 1; }
        vm_set_process_args(&vm, argc, argv);
        result = run_source(&vm, source, argv[file_idx], dump_bytecode);
        free(source);
    } else {
        fprintf(stderr, "Error: No input file specified\n");
        print_usage(argv[0]);
        vm_free(&vm);
        return 1;
    }

    vm_free(&vm);
    return result;
}