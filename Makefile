# Makefile for Luna C Interpreter
# Supports: Linux, macOS, Windows (MinGW)

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O3 -DNDEBUG -flto -Isrc -Isrc/stdlib
LDFLAGS = -lm -flto

# Platform detection
ifeq ($(OS),Windows_NT)
    TARGET = luna.exe
    RM = del /Q
    EXE_EXT = .exe
    SHARED_LIB = luna.dll
    SHARED_IMPLIB = luna.lib
    LDFLAGS += -lws2_32
else
    TARGET = luna
    RM = rm -f
    EXE_EXT =
    SHARED_LIB = libluna.so
    SHARED_IMPLIB =
endif

# Source files
SOURCES = src/main.c src/value.c src/ast_free.c \
    src/luna.c \
    src/lexer.c src/parser.c src/chunk.c src/vm.c src/compiler.c \
    src/parse_expr.c src/parse_stmt.c src/parse_decl.c src/fstring.c src/module.c \
    src/stdlib/stdlib_math.c src/stdlib/stdlib_random.c src/stdlib/stdlib_noise.c \
    src/stdlib/stdlib_io.c src/stdlib/stdlib_time.c src/stdlib/stdlib_os.c \
    src/stdlib/stdlib_buffer.c src/stdlib/stdlib_string.c \
    src/stdlib/stdlib_net.c src/stdlib/stdlib_json.c \
    src/stdlib/vm_builtins.c

OBJECTS = $(SOURCES:.c=.o)
LIB_OBJECTS = $(filter-out src/main.o, $(OBJECTS))

# Header files
HEADERS = src/ast.h src/value.h src/lexer.h src/parser.h src/chunk.h src/vm.h src/opcode.h src/compiler.h \
    src/luna.h \
    src/parse_expr.h src/parse_stmt.h src/parse_decl.h src/fstring.h src/module.h \
    src/stdlib/stdlib_math.h src/stdlib/stdlib_random.h src/stdlib/stdlib_noise.h \
    src/stdlib/stdlib_io.h src/stdlib/stdlib_time.h src/stdlib/stdlib_os.h \
    src/stdlib/stdlib_buffer.h src/stdlib/stdlib_string.h \
    src/stdlib/stdlib_net.h src/stdlib/stdlib_json.h \
    src/vm_opcodes.inc

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
	cmd /c "del /Q $(subst /,\,$(OBJECTS)) $(TARGET) $(SHARED_LIB) $(SHARED_IMPLIB) 2>nul" || true

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

# Shared library (libluna.so / luna.dll)
shared: CFLAGS += -fPIC
shared: $(LIB_OBJECTS)
ifeq ($(OS),Windows_NT)
	$(CC) -shared -o $(SHARED_LIB) $(LIB_OBJECTS) $(LDFLAGS) -Wl,--out-implib,$(SHARED_IMPLIB)
else
	$(CC) -shared -o $(SHARED_LIB) $(LIB_OBJECTS) $(LDFLAGS)
endif

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
