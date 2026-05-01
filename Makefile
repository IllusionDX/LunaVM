# Makefile for Luna C Interpreter
# Supports: Linux, macOS, Windows (MinGW)

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O3 -DNDEBUG -flto -Isrc/stdlib -I.
LDFLAGS = -lm -flto

# Platform detection
ifeq ($(OS),Windows_NT)
    TARGET = luna.exe
    RM = del /Q
    EXE_EXT = .exe
    LDFLAGS += -lws2_32
else
    TARGET = luna
    RM = rm -f
    EXE_EXT =
endif

# Source files
SOURCES = main.c value.c ast_free.c \
    luna.c \
    lexer.c parser.c chunk.c vm.c compiler.c \
    parse_expr.c parse_stmt.c parse_decl.c fstring.c module.c \
    src/stdlib/stdlib_math.c src/stdlib/stdlib_random.c src/stdlib/stdlib_noise.c \
    src/stdlib/stdlib_io.c src/stdlib/stdlib_time.c src/stdlib/stdlib_os.c \
    src/stdlib/stdlib_buffer.c src/stdlib/stdlib_string.c \
    src/stdlib/stdlib_net.c src/stdlib/stdlib_json.c \
    src/stdlib/vm_builtins.c

OBJECTS = $(SOURCES:.c=.o)

# Header files
HEADERS = ast.h value.h lexer.h parser.h chunk.h vm.h opcode.h compiler.h \
    luna.h \
    parse_expr.h parse_stmt.h parse_decl.h fstring.h module.h \
    src/stdlib/stdlib_math.h src/stdlib/stdlib_random.h src/stdlib/stdlib_noise.h \
    src/stdlib/stdlib_io.h src/stdlib/stdlib_time.h src/stdlib/stdlib_os.h \
    src/stdlib/stdlib_buffer.h src/stdlib/stdlib_string.h \
    src/stdlib/stdlib_net.h src/stdlib/stdlib_json.h \
    vm_opcodes.inc

# Default target
all: $(TARGET)

# Build executable
$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

# Compile source files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	del /Q $(subst /,\,$(OBJECTS)) $(TARGET) 2>nul || true

# Rebuild
rebuild: clean all

# Run regression tests
test: $(TARGET)
ifeq ($(OS),Windows_NT)
	powershell -ExecutionPolicy Bypass -File run_tests.ps1
else
	bash run_tests.sh
endif

# Debug build
debug: CFLAGS = -Wall -Wextra -std=c99 -O0 -g -DDEBUG
debug: $(TARGET)

# Release build
release: CFLAGS = -Wall -Wextra -std=c99 -O3 -DNDEBUG
release: $(TARGET)

# Static build (self-contained executable, no DLL dependencies)
static: LDFLAGS = -lm -static
static: $(TARGET)

# Static analysis
analyze:
	clang --analyze $(SOURCES) $(HEADERS) 2>/dev/null || echo "Clang not available"

# Format code (requires clang-format)
format:
	clang-format -i $(SOURCES) $(HEADERS) 2>/dev/null || echo "clang-format not available"

# Install (optional)
install: $(TARGET)
	cp $(TARGET) /usr/local/bin/ 2>/dev/null || echo "Install requires sudo"

.PHONY: all clean rebuild test debug analyze format install
