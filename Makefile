# Makefile for Luna C Interpreter
# Supports: Linux, macOS, Windows (MinGW)

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -g
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
SOURCES = main.c value.c environment.c ast_free.c \
    eval_expr.c eval_call.c eval_stmt.c eval_decl.c \
    lexer.c parser.c

OBJECTS = $(SOURCES:.c=.o)

# Header files
HEADERS = ast.h value.h environment.h eval.h lexer.h parser.h

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

# Run tests
test: $(TARGET)
	@echo "Running interpreter tests..."
	@echo '{"declarations":[],"statements":[]}' | ./$(TARGET) --json -

# Debug build
debug: CFLAGS = -Wall -Wextra -std=c99 -O0 -g -DDEBUG
debug: $(TARGET)

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
