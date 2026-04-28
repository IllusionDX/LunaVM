/* Native C Lexer for Luna Programming Language.
 * Tokenizes Luna source code into a stream of tokens.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"

/* ============== Helper functions ============== */

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_at_end(Lexer *lexer) {
    return *lexer->current == '\0';
}

static char peek(Lexer *lexer) {
    return *lexer->current;
}

static char peek_next(Lexer *lexer) {
    if (is_at_end(lexer)) return '\0';
    return lexer->current[1];
}

static char advance(Lexer *lexer) {
    char c = *lexer->current++;
    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
    return c;
}

static Token *make_token(Lexer *lexer, TokenType type, const char *start, int length) {
    Token *token = (Token *)malloc(sizeof(Token));
    if (!token) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    token->type = type;
    token->value = (char *)malloc(length + 1);
    if (!token->value) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    strncpy(token->value, start, length);
    token->value[length] = '\0';
    token->line = lexer->line;
    token->column = lexer->column - length;
    token->next = NULL;
    
    return token;
}

static void skip_whitespace(Lexer *lexer) {
    while (true) {
        char c = peek(lexer);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(lexer);
        } else {
            break;
        }
    }
}

static void skip_comment(Lexer *lexer) {
    if (peek(lexer) == '#') {
        while (!is_at_end(lexer) && peek(lexer) != '\n' && peek(lexer) != '\r') {
            advance(lexer);
        }
    }
}

static void skip_multiline_comment(Lexer *lexer) {
    /* consume first two quotes (caller already consumed the third) */
    advance(lexer);
    advance(lexer);
    while (!is_at_end(lexer)) {
        if (peek(lexer) == '"' && peek_next(lexer) == '"' &&
            lexer->current[2] == '"') {
            advance(lexer);
            advance(lexer);
            advance(lexer);
            return;
        }
        advance(lexer);
    }
}

/* ============== Keyword lookup ============== */

typedef struct {
    const char *keyword;
    TokenType type;
} Keyword;

static Keyword keywords[] = {
    {"true", TOK_TRUE},
    {"false", TOK_FALSE},
    {"null", TOK_NULL},
    {"def", TOK_DEF},
    {"const", TOK_CONST},
    {"class", TOK_CLASS},
    {"extends", TOK_EXTENDS},
    {"new", TOK_NEW},
    {"self", TOK_SELF},
    {"super", TOK_SUPER},
    {"if", TOK_IF},
    {"else", TOK_ELSE},
    {"while", TOK_WHILE},
    {"for", TOK_FOR},
    {"in", TOK_IN},
    {"break", TOK_BREAK},
    {"continue", TOK_CONTINUE},
    {"return", TOK_RETURN},
    {"pass", TOK_PASS},
    {"enum", TOK_ENUM},
    {"switch", TOK_SWITCH},
    {"case", TOK_CASE},
    {"default", TOK_DEFAULT},
    {"try", TOK_TRY},
    {"except", TOK_EXCEPT},
    {"finally", TOK_FINALLY},
    {"as", TOK_AS},
    {"throw", TOK_THROW},
    {"and", TOK_AND},
    {"or", TOK_OR},
    {"not", TOK_NOT},
    {"import", TOK_IMPORT},
    {"from", TOK_FROM},
    {"var", TOK_VAR},
    {NULL, TOK_EOF}
};

static TokenType lookup_keyword(const char *text) {
    for (int i = 0; keywords[i].keyword != NULL; i++) {
        if (strcmp(keywords[i].keyword, text) == 0) {
            return keywords[i].type;
        }
    }
    return TOK_IDENTIFIER;
}

/* ============== Token scanning ============== */

static Token *scan_identifier(Lexer *lexer) {
    const char *start = lexer->current;
    
    while (is_alpha(peek(lexer)) || is_digit(peek(lexer))) {
        advance(lexer);
    }
    
    int length = (int)(lexer->current - start);
    char *text = (char *)malloc(length + 1);
    strncpy(text, start, length);
    text[length] = '\0';
    
    TokenType type = lookup_keyword(text);
    free(text);
    
    return make_token(lexer, type, start, length);
}

static Token *scan_number(Lexer *lexer) {
    const char *start = lexer->current;
    bool is_float = false;
    
    while (is_digit(peek(lexer))) {
        advance(lexer);
    }
    
    if (peek(lexer) == '.' && is_digit(peek_next(lexer))) {
        is_float = true;
        advance(lexer);
        while (is_digit(peek(lexer))) {
            advance(lexer);
        }
    }
    
    if (peek(lexer) == 'e' || peek(lexer) == 'E') {
        is_float = true;
        advance(lexer);
        if (peek(lexer) == '+' || peek(lexer) == '-') {
            advance(lexer);
        }
        while (is_digit(peek(lexer))) {
            advance(lexer);
        }
    }
    
    int length = (int)(lexer->current - start);
    TokenType type = is_float ? TOK_FLOAT_LITERAL : TOK_INTEGER_LITERAL;
    return make_token(lexer, type, start, length);
}

static Token *scan_string(Lexer *lexer) {
    const char *start = lexer->current;
    char quote = advance(lexer);
    
    while (!is_at_end(lexer) && peek(lexer) != quote) {
        if (peek(lexer) == '\\') {
            advance(lexer);
            if (!is_at_end(lexer)) {
                advance(lexer);
            }
        } else {
            advance(lexer);
        }
    }
    
    if (is_at_end(lexer)) {
        return make_token(lexer, TOK_ERROR, start, (int)(lexer->current - start));
    }
    
    advance(lexer);
    
    int length = (int)(lexer->current - start);
    TokenType type = (quote == '"') ? TOK_STRING_LITERAL : TOK_CHAR_LITERAL;
    return make_token(lexer, type, start + 1, length - 2);
}

static Token *scan_fstring(Lexer *lexer) {
    Token *tok = scan_string(lexer);
    tok->type = TOK_FSTRING_LITERAL;
    tok->column -= 1; /* adjust for the 'f' prefix */
    return tok;
}

static Token *scan_operator(Lexer *lexer) {
    const char *start = lexer->current;
    char c = advance(lexer);
    char next = peek(lexer);
    
    switch (c) {
        case '+':
            if (next == '=') { advance(lexer); return make_token(lexer, TOK_PLUS_ASSIGN, start, 2); }
            return make_token(lexer, TOK_PLUS, start, 1);
        case '-':
            if (next == '=') { advance(lexer); return make_token(lexer, TOK_MINUS_ASSIGN, start, 2); }
            if (next == '>') { advance(lexer); return make_token(lexer, TOK_ARROW, start, 2); }
            return make_token(lexer, TOK_MINUS, start, 1);
        case '*':
            if (next == '=') { advance(lexer); return make_token(lexer, TOK_STAR_ASSIGN, start, 2); }
            return make_token(lexer, TOK_STAR, start, 1);
        case '/':
            if (next == '=') { advance(lexer); return make_token(lexer, TOK_SLASH_ASSIGN, start, 2); }
            return make_token(lexer, TOK_SLASH, start, 1);
        case '%':
            return make_token(lexer, TOK_PERCENT, start, 1);
        case '=':
            if (next == '=') { advance(lexer); return make_token(lexer, TOK_EQ, start, 2); }
            if (next == '>') { advance(lexer); return make_token(lexer, TOK_LAMBDA, start, 2); }
            return make_token(lexer, TOK_ASSIGN, start, 1);
        case '!':
            if (next == '=') { advance(lexer); return make_token(lexer, TOK_NE, start, 2); }
            return make_token(lexer, TOK_NOT, start, 1);
        case '<':
            if (next == '=') { advance(lexer); return make_token(lexer, TOK_LE, start, 2); }
            if (next == '<') { advance(lexer); return make_token(lexer, TOK_LSHIFT, start, 2); }
            return make_token(lexer, TOK_LT, start, 1);
        case '>':
            if (next == '=') { advance(lexer); return make_token(lexer, TOK_GE, start, 2); }
            if (next == '>') { advance(lexer); return make_token(lexer, TOK_RSHIFT, start, 2); }
            return make_token(lexer, TOK_GT, start, 1);
        case '&':
            return make_token(lexer, TOK_AMPERSAND, start, 1);
        case '|':
            return make_token(lexer, TOK_PIPE, start, 1);
        case '^':
            return make_token(lexer, TOK_CARET, start, 1);
        case '~':
            return make_token(lexer, TOK_TILDE, start, 1);
        case '(':
            return make_token(lexer, TOK_LPAREN, start, 1);
        case ')':
            return make_token(lexer, TOK_RPAREN, start, 1);
        case '[':
            return make_token(lexer, TOK_LBRACKET, start, 1);
        case ']':
            return make_token(lexer, TOK_RBRACKET, start, 1);
        case '{':
            return make_token(lexer, TOK_LBRACE, start, 1);
        case '}':
            return make_token(lexer, TOK_RBRACE, start, 1);
        case ':':
            return make_token(lexer, TOK_COLON, start, 1);
        case ',':
            return make_token(lexer, TOK_COMMA, start, 1);
        case '.':
            return make_token(lexer, TOK_DOT, start, 1);
        case ';':
            return make_token(lexer, TOK_SEMICOLON, start, 1);
        case '?':
            if (next == '?') { advance(lexer); return make_token(lexer, TOK_COALESCE, start, 2); }
            if (next == '.') {
                advance(lexer);
                /* Check for ?.[ (3-char token) */
                if (peek(lexer) == '[') {
                    advance(lexer);
                    return make_token(lexer, TOK_QUESTION_LBRACKET, start, 3);
                }
                return make_token(lexer, TOK_QUESTION_DOT, start, 2);
            }
            return make_token(lexer, TOK_ERROR, start, 1);
        default:
            return make_token(lexer, TOK_ERROR, start, 1);
    }
}

static Token *scan_token(Lexer *lexer) {
    // Check pending tokens
    if (lexer->pending_tokens) {
        Token *tok = lexer->pending_tokens;
        lexer->pending_tokens = tok->next;
        tok->next = NULL;
        return tok;
    }

    if (lexer->at_line_start) {
        if (peek(lexer) == '\n' || peek(lexer) == '\r' || peek(lexer) == '\0') {
            lexer->at_line_start = true;
            if (peek(lexer) == '\n' || peek(lexer) == '\r') {
                if (peek(lexer) == '\r' && peek_next(lexer) == '\n') advance(lexer);
                advance(lexer);
                return scan_token(lexer);
            }
        } else if (peek(lexer) == '#') {
            // skip comment check at line start, handled later
        } else {
            int indent = 0;
            while (peek(lexer) == ' ') { advance(lexer); indent++; }
            while (peek(lexer) == '\t') { advance(lexer); indent += 4; }
            
            if (peek(lexer) == '\n' || peek(lexer) == '\r' || peek(lexer) == '\0' || peek(lexer) == '#') {
                lexer->at_line_start = true;
                return scan_token(lexer);
            }
            
            lexer->at_line_start = false;
            int current_indent = lexer->indent_stack[lexer->indent_stack_size - 1];
            
            if (indent > current_indent) {
                if (lexer->indent_stack_size >= lexer->indent_stack_capacity) {
                    lexer->indent_stack_capacity *= 2;
                    lexer->indent_stack = (int *)realloc(lexer->indent_stack, sizeof(int) * lexer->indent_stack_capacity);
                }
                lexer->indent_stack[lexer->indent_stack_size++] = indent;
                return make_token(lexer, TOK_INDENT, lexer->current, 0);
            } else if (indent < current_indent) {
                Token *head = NULL;
                Token *tail = NULL;
                while (lexer->indent_stack_size > 1 && indent < lexer->indent_stack[lexer->indent_stack_size - 1]) {
                    lexer->indent_stack_size--;
                    Token *dedent = make_token(lexer, TOK_DEDENT, lexer->current, 0);
                    if (!head) { head = dedent; tail = dedent; }
                    else { tail->next = dedent; tail = dedent; }
                }
                if (indent != lexer->indent_stack[lexer->indent_stack_size - 1]) {
                    fprintf(stderr, "Indentation error at line %d\n", lexer->line);
                }
                if (head) {
                    lexer->pending_tokens = head->next;
                    head->next = NULL;
                    return head;
                }
            }
        }
    }

    skip_whitespace(lexer);
    
    if (is_at_end(lexer)) {
        if (lexer->indent_stack_size > 1) {
            Token *head = NULL;
            Token *tail = NULL;
            while (lexer->indent_stack_size > 1) {
                lexer->indent_stack_size--;
                Token *dedent = make_token(lexer, TOK_DEDENT, lexer->current, 0);
                if (!head) { head = dedent; tail = dedent; }
                else { tail->next = dedent; tail = dedent; }
            }
            Token *eof_tok = make_token(lexer, TOK_EOF, lexer->current, 0);
            tail->next = eof_tok;
            lexer->pending_tokens = head->next;
            head->next = NULL;
            return head;
        }
        return make_token(lexer, TOK_EOF, lexer->current, 0);
    }
    
    char c = peek(lexer);
    
    if (c == '#') {
        skip_comment(lexer);
        return scan_token(lexer);
    }
    
    if (c == '\n' || c == '\r') {
        lexer->at_line_start = true;
        if (c == '\r' && peek_next(lexer) == '\n') {
            advance(lexer); /* consume \r */
            advance(lexer); /* consume \n */
        } else {
            advance(lexer);
        }
        return make_token(lexer, TOK_NEWLINE, "\\n", 1);
    }
    
    if (c == 'f' && (peek_next(lexer) == '"' || peek_next(lexer) == '\'')) {
        advance(lexer); /* consume 'f' */
        return scan_fstring(lexer);
    }
    if (is_alpha(c)) { return scan_identifier(lexer); }
    if (is_digit(c) || (c == '.' && is_digit(peek_next(lexer)))) { return scan_number(lexer); }
    if (c == '"' && peek_next(lexer) == '"' && lexer->current[2] == '"') {
        skip_multiline_comment(lexer);
        return scan_token(lexer);
    }
    if (c == '"' || c == '\'') { return scan_string(lexer); }
    
    return scan_operator(lexer);
}

/* ============== Public API ============== */

Lexer *lexer_new(const char *source) {
    Lexer *lexer = (Lexer *)malloc(sizeof(Lexer));
    if (!lexer) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    lexer->source = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->column = 1;
    lexer->indent_stack_capacity = 8;
    lexer->indent_stack_size = 1;
    lexer->indent_stack = (int *)malloc(sizeof(int) * 8);
    lexer->indent_stack[0] = 0;
    lexer->at_line_start = true;
    lexer->pending_tokens = NULL;
    
    return lexer;
}

void lexer_free(Lexer *lexer) {
    if (lexer) {
        free(lexer->indent_stack);
        free(lexer);
    }
}

TokenList *lexer_tokenize(Lexer *lexer) {
    TokenList *list = (TokenList *)malloc(sizeof(TokenList));
    if (!list) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    list->head = NULL;
    list->tail = NULL;
    list->count = 0;

    /* Handle BOMs */
    const unsigned char *src = (const unsigned char *)lexer->source;
    if (src[0] == 0xEF && src[1] == 0xBB && src[2] == 0xBF) {
        /* UTF-8 BOM — skip it */
        lexer->current += 3;
    } else if ((src[0] == 0xFF && src[1] == 0xFE) || (src[0] == 0xFE && src[1] == 0xFF)) {
        /* UTF-16 BOM — not supported */
        Token *err = (Token *)malloc(sizeof(Token));
        err->type = TOK_ERROR;
        err->value = malloc(64);
        strcpy(err->value, "UTF-16 encoding is not supported; please use UTF-8");
        err->line = 1;
        err->column = 1;
        err->next = NULL;
        list->head = err;
        list->tail = err;
        list->count = 1;
        return list;
    }

    while (true) {
        Token *token = scan_token(lexer);
        
        if (list->tail) {
            list->tail->next = token;
        } else {
            list->head = token;
        }
        list->tail = token;
        list->count++;
        
        if (token->type == TOK_EOF) {
            break;
        }
        if (token->type == TOK_ERROR) {
            Token *eof_token = make_token(lexer, TOK_EOF, lexer->current, 0);
            list->tail->next = eof_token;
            list->tail = eof_token;
            list->count++;
            break;
        }
    }
    
    return list;
}

void token_free(Token *token) {
    if (token) {
        free(token->value);
        free(token);
    }
}

void token_list_free(TokenList *list) {
    if (list) {
        Token *current = list->head;
        while (current) {
            Token *next = current->next;
            token_free(current);
            current = next;
        }
        free(list);
    }
}

const char *token_type_name(TokenType type) {
    switch (type) {
        case TOK_INTEGER_LITERAL: return "INTEGER";
        case TOK_FLOAT_LITERAL: return "FLOAT";
        case TOK_STRING_LITERAL: return "STRING";
        case TOK_FSTRING_LITERAL: return "FSTRING";
        case TOK_CHAR_LITERAL: return "CHAR";
        case TOK_TRUE: return "TRUE";
        case TOK_FALSE: return "FALSE";
        case TOK_NULL: return "NULL";
        case TOK_DEF: return "DEF";
        case TOK_CONST: return "CONST";
        case TOK_CLASS: return "CLASS";
        case TOK_EXTENDS: return "EXTENDS";
        case TOK_NEW: return "NEW";
        case TOK_SELF: return "SELF";
        case TOK_SUPER: return "SUPER";
        case TOK_IF: return "IF";
        case TOK_WHILE: return "WHILE";
        case TOK_FOR: return "FOR";
        case TOK_IN: return "IN";
        case TOK_BREAK: return "BREAK";
        case TOK_CONTINUE: return "CONTINUE";
        case TOK_RETURN: return "RETURN";
        case TOK_PASS: return "PASS";
        case TOK_ELSE: return "ELSE";
        case TOK_ENUM: return "ENUM";
        case TOK_SWITCH: return "SWITCH";
        case TOK_CASE: return "CASE";
        case TOK_DEFAULT: return "DEFAULT";
        case TOK_TRY: return "TRY";
        case TOK_EXCEPT: return "EXCEPT";
        case TOK_FINALLY: return "FINALLY";
        case TOK_AS: return "AS";
        case TOK_THROW: return "THROW";
        case TOK_AND: return "AND";
        case TOK_OR: return "OR";
        case TOK_NOT: return "NOT";
        case TOK_IMPORT: return "IMPORT";
        case TOK_FROM: return "FROM";
        case TOK_VAR: return "VAR";
        case TOK_PLUS: return "PLUS";
        case TOK_MINUS: return "MINUS";
        case TOK_STAR: return "STAR";
        case TOK_SLASH: return "SLASH";
        case TOK_PERCENT: return "PERCENT";
        case TOK_EQ: return "EQ";
        case TOK_NE: return "NE";
        case TOK_LT: return "LT";
        case TOK_GT: return "GT";
        case TOK_LE: return "LE";
        case TOK_GE: return "GE";
        case TOK_ASSIGN: return "ASSIGN";
        case TOK_PLUS_ASSIGN: return "PLUS_ASSIGN";
        case TOK_MINUS_ASSIGN: return "MINUS_ASSIGN";
        case TOK_STAR_ASSIGN: return "STAR_ASSIGN";
        case TOK_SLASH_ASSIGN: return "SLASH_ASSIGN";
        case TOK_ARROW: return "ARROW";
        case TOK_LAMBDA: return "LAMBDA";
        case TOK_COALESCE: return "COALESCE";
        case TOK_QUESTION_DOT: return "QUESTION_DOT";
        case TOK_QUESTION_LBRACKET: return "QUESTION_LBRACKET";
        case TOK_AMPERSAND: return "AMPERSAND";
        case TOK_PIPE: return "PIPE";
        case TOK_CARET: return "CARET";
        case TOK_LSHIFT: return "LSHIFT";
        case TOK_RSHIFT: return "RSHIFT";
        case TOK_TILDE: return "TILDE";
        case TOK_LPAREN: return "LPAREN";
        case TOK_RPAREN: return "RPAREN";
        case TOK_LBRACKET: return "LBRACKET";
        case TOK_RBRACKET: return "RBRACKET";
        case TOK_LBRACE: return "LBRACE";
        case TOK_RBRACE: return "RBRACE";
        case TOK_COLON: return "COLON";
        case TOK_COMMA: return "COMMA";
        case TOK_DOT: return "DOT";
        case TOK_SEMICOLON: return "SEMICOLON";
        case TOK_IDENTIFIER: return "IDENTIFIER";
        case TOK_INDENT: return "INDENT";
        case TOK_DEDENT: return "DEDENT";
        case TOK_NEWLINE: return "NEWLINE";
        case TOK_EOF: return "EOF";
        case TOK_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

void token_list_print(TokenList *list) {
    if (!list) return;
    
    Token *current = list->head;
    while (current) {
        printf("[%d:%d] %s: '%s'\n",
               current->line,
               current->column,
               token_type_name(current->type),
               current->value ? current->value : "");
        current = current->next;
    }
}
