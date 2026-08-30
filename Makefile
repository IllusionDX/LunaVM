# Makefile for Luna C Interpreter
# Supports: Linux, macOS, Windows (MinGW)

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O3 -DNDEBUG -flto -Isrc -Isrc/luna -Isrc/luna/stdlib
LDFLAGS = -lm -flto

# Platform detection
ifeq ($(OS),Windows_NT)
    TARGET = luna.exe
    PY_TARGET = pyluna.exe
    RM = del /Q
    EXE_EXT = .exe
    SHARED_LIB = luna.dll
    SHARED_IMPLIB = luna.lib
    LDFLAGS += -lws2_32
else
    TARGET = luna
    PY_TARGET = pyluna
    RM = rm -f
    EXE_EXT =
    SHARED_LIB = libluna.so
    SHARED_IMPLIB =
endif

# Source files
SOURCES = src/value.c src/chunk.c src/vm.c \
    src/luna/object.c \
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

# Language-agnostic core shared by both frontends
CORE_OBJECTS = src/value.o src/chunk.o src/vm.o

# Python-subset frontend (src/py) — shares the core above
PY_SOURCES = src/py/object.c \
    src/py/main.c src/py/luna.c \
    src/py/lexer.c src/py/parser.c src/py/compiler.c \
    src/py/parse_expr.c src/py/parse_stmt.c src/py/parse_decl.c \
    src/py/fstring.c src/py/module.c src/py/ast_free.c \
    src/py/stdlib/stdlib_math.c src/py/stdlib/stdlib_random.c src/py/stdlib/stdlib_noise.c \
    src/py/stdlib/stdlib_io.c src/py/stdlib/stdlib_time.c src/py/stdlib/stdlib_os.c \
    src/py/stdlib/stdlib_buffer.c src/py/stdlib/stdlib_string.c \
    src/py/stdlib/stdlib_net.c src/py/stdlib/stdlib_json.c \
    src/py/stdlib/vm_builtins.c

PY_OBJECTS = $(PY_SOURCES:.c=.o)
PY_HEADERS = $(wildcard src/py/*.h src/py/stdlib/*.h)

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
all: $(TARGET) $(PY_TARGET)

# Build executables
$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

$(PY_TARGET): $(CORE_OBJECTS) $(PY_OBJECTS)
	$(CC) $(CORE_OBJECTS) $(PY_OBJECTS) -o $(PY_TARGET) $(LDFLAGS)

# Compile source files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

src/py/%.o: src/py/%.c $(HEADERS) $(PY_HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	cmd /c "del /Q $(subst /,\,$(OBJECTS)) $(subst /,\,$(PY_OBJECTS)) $(TARGET) $(PY_TARGET) $(SHARED_LIB) $(SHARED_IMPLIB) 2>nul" || true

# Rebuild
rebuild: clean all

# Run regression tests
test: $(TARGET)
ifeq ($(OS),Windows_NT)
	powershell -ExecutionPolicy Bypass -File run_tests.ps1
else
	bash run_tests.sh
endif

# Run Python-subset regression tests
test-py: $(PY_TARGET)
ifeq ($(OS),Windows_NT)
	powershell -ExecutionPolicy Bypass -File run_tests_py.ps1
else
	bash run_tests_py.sh
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

.PHONY: all clean rebuild test test-py debug analyze format install
