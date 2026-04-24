# Makefile for Luna C Interpreter
# Supports: Linux, macOS, Windows (MinGW)

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O3 -DNDEBUG
LDFLAGS = -lm

# Platform detection
ifeq ($(OS),Windows_NT)
    TARGET = luna.exe
    RM = del /Q
    EXE_EXT = .exe
else
    TARGET = luna
    RM = rm -f
    EXE_EXT =
endif

# Source files
SOURCES = main.c value.c ast_free.c \
    lexer.c parser.c chunk.c vm.c vm_builtins.c compiler.c

OBJECTS = $(SOURCES:.c=.o)

# Header files
HEADERS = ast.h value.h lexer.h parser.h chunk.h vm.h opcode.h compiler.h

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
	$(RM) $(OBJECTS) $(TARGET)

# Rebuild
rebuild: clean all

# Run regression tests
test: $(TARGET)
	powershell -ExecutionPolicy Bypass -File run_tests.ps1

# Debug build
debug: CFLAGS = -Wall -Wextra -std=c99 -O0 -g -DDEBUG
debug: $(TARGET)

# Release build
release: CFLAGS = -Wall -Wextra -std=c99 -O3 -DNDEBUG
release: $(TARGET)

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
