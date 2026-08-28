# Makefile for Luna C Interpreter
# Supports: Linux, macOS, Windows (MinGW)

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O3 -DNDEBUG -flto -Isrc -Isrc/luna -Isrc/luna/stdlib
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
SOURCES = src/value.c src/chunk.c src/vm.c \
    src/luna/main.c src/luna/luna.c \
    src/luna/lexer.c src/luna/parser.c src/luna/compiler.c \
    src/luna/parse_expr.c src/luna/parse_stmt.c src/luna/parse_decl.c \
    src/luna/fstring.c src/luna/module.c src/luna/ast_free.c \
    src/luna/stdlib/stdlib_math.c src/luna/stdlib/stdlib_random.c src/luna/stdlib/stdlib_noise.c \
    src/luna/stdlib/stdlib_io.c src/luna/stdlib/stdlib_time.c src/luna/stdlib/stdlib_os.c \
    src/luna/stdlib/stdlib_buffer.c src/luna/stdlib/stdlib_string.c \
    src/luna/stdlib/stdlib_net.c src/luna/stdlib/stdlib_json.c \
    src/luna/stdlib/vm_builtins.c

OBJECTS = $(SOURCES:.c=.o)
LIB_OBJECTS = $(filter-out src/main.o, $(OBJECTS))

# Header files
HEADERS = src/value.h src/chunk.h src/vm.h src/opcode.h src/vm_opcodes.inc \
    src/luna/ast.h src/luna/lexer.h src/luna/parser.h src/luna/compiler.h \
    src/luna/luna.h src/luna/parse_expr.h src/luna/parse_stmt.h src/luna/parse_decl.h \
    src/luna/fstring.h src/luna/module.h \
    src/luna/stdlib/stdlib_math.h src/luna/stdlib/stdlib_random.h src/luna/stdlib/stdlib_noise.h \
    src/luna/stdlib/stdlib_io.h src/luna/stdlib/stdlib_time.h src/luna/stdlib/stdlib_os.h \
    src/luna/stdlib/stdlib_buffer.h src/luna/stdlib/stdlib_string.h \
    src/luna/stdlib/stdlib_net.h src/luna/stdlib/stdlib_json.h

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
