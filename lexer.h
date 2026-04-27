/* Native C Lexer for Luna Programming Language.
 * Tokenizes Luna source code into a stream of tokens.
 */

#ifndef LUNA_LEXER_H
#define LUNA_LEXER_H

#include <stdbool.h>

/* Token types */
typedef enum {
    TOK_INTEGER_LITERAL,
    TOK_FLOAT_LITERAL,
    TOK_STRING_LITERAL,
    TOK_MULTILINE_STRING,
    TOK_FSTRING_LITERAL,
    TOK_CHAR_LITERAL,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NULL,
    TOK_DEF,
    TOK_CONST,
    TOK_CLASS,
    TOK_EXTENDS,
    TOK_NEW,
    TOK_SELF,
    TOK_SUPER,
    TOK_IF,
    TOK_WHILE,
    TOK_FOR,
    TOK_IN,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_RETURN,
    TOK_PASS,
    TOK_ELSE,
    TOK_ENUM,
    TOK_SWITCH,
    TOK_CASE,
    TOK_DEFAULT,
    TOK_TRY,
    TOK_CATCH,
    TOK_FINALLY,
    TOK_THROW,
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_IMPORT,
    TOK_FROM,
    TOK_VAR,
    TOK_DICT,
    TOK_INT_TYPE,
    TOK_FLOAT_TYPE,
    TOK_DOUBLE_TYPE,
    TOK_BOOL_TYPE,
    TOK_CHAR_TYPE,
    TOK_STRING_TYPE,
    TOK_LIST_TYPE,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_EQ,
    TOK_NE,
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    TOK_ASSIGN,
    TOK_LAMBDA,
    TOK_PLUS_ASSIGN,
    TOK_MINUS_ASSIGN,
    TOK_STAR_ASSIGN,
    TOK_SLASH_ASSIGN,
    TOK_ARROW,
    TOK_AMPERSAND,
    TOK_PIPE,
    TOK_CARET,
    TOK_LSHIFT,
    TOK_RSHIFT,
    TOK_TILDE,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_COLON,
    TOK_COMMA,
    TOK_DOT,
    TOK_SEMICOLON,
    TOK_IDENTIFIER,
    TOK_INDENT,
    TOK_DEDENT,
    TOK_NEWLINE,
    TOK_EOF,
    TOK_ERROR
} TokenType;

/* Token structure */
typedef struct Token {
    TokenType type;
    char *value;
    int line;
    int column;
    struct Token *next;
} Token;

/* Token list structure */
typedef struct TokenList {
    Token *head;
    Token *tail;
    int count;
} TokenList;

/* Lexer state */
typedef struct Lexer {
    const char *source;
    const char *current;
    int line;
    int column;
    int indent_level;
    int *indent_stack;
    int indent_stack_size;
    int indent_stack_capacity;
    bool at_line_start;
    Token *pending_tokens;
} Lexer;

Lexer *lexer_new(const char *source);
void lexer_free(Lexer *lexer);
TokenList *lexer_tokenize(Lexer *lexer);
void token_list_free(TokenList *list);
void token_free(Token *token);
const char *token_type_name(TokenType type);
void token_list_print(TokenList *list);

#endif
