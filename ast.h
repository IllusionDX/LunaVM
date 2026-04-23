/* Abstract Syntax Tree definitions for Luna interpreter.
 * Dynamic VM variant — no static type system in the AST.
 */

#ifndef LUNA_AST_H
#define LUNA_AST_H

#include <stdbool.h>

/* Forward declarations */
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Decl Decl;

/* ============== Expressions ============== */

typedef enum {
    EXPR_INTEGER,
    EXPR_FLOAT,
    EXPR_STRING,
    EXPR_CHAR,
    EXPR_BOOL,
    EXPR_NULL,
    EXPR_IDENTIFIER,
    EXPR_BINARY,
    EXPR_UNARY,
    EXPR_CALL,
    EXPR_FIELD_ACCESS,
    EXPR_INDEX_ACCESS,
    EXPR_ASSIGNMENT,
    EXPR_COMPOUND_ASSIGN,
    EXPR_TERNARY,
    EXPR_LIST_LITERAL,
    EXPR_DICT_LITERAL,
    EXPR_NEW
} ExprKind;

typedef struct DictEntry {
    Expr *key;
    Expr *value;
} DictEntry;

typedef struct Expr {
    ExprKind kind;
    union {
        struct {
            char *value;
        } integer;
        struct {
            char *value;
        } float_lit;
        struct {
            char *value;
        } string;
        struct {
            char value;
        } char_lit;
        struct {
            bool value;
        } boolean;
        struct {
            char *name;
        } identifier;
        struct {
            Expr *left;
            char *operator;
            Expr *right;
        } binary;
        struct {
            char *operator;
            Expr *operand;
        } unary;
        struct {
            Expr *callee;
            Expr **arguments;
            int arg_count;
        } call;
        struct {
            Expr *obj;
            char *field;
        } field_access;
        struct {
            Expr *obj;
            Expr *index;
        } index_access;
        struct {
            Expr *target;
            Expr *value;
        } assignment;
        struct {
            Expr *target;
            char *operator;
            Expr *value;
        } compound_assign;
        struct {
            Expr *condition;
            Expr *then_expr;
            Expr *else_expr;
        } ternary;
        struct {
            Expr **elements;
            int element_count;
        } list_literal;
        struct {
            DictEntry *entries;
            int entry_count;
        } dict_literal;
        struct {
            char *class_name;
            Expr **arguments;
            int arg_count;
        } new_expr;
    } data;
} Expr;

/* ============== Statements ============== */

typedef enum {
    STMT_EXPRESSION,
    STMT_VAR_DECL,
    STMT_RETURN,
    STMT_PASS,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_IF,
    STMT_WHILE,
    STMT_FOR,
    STMT_SWITCH,
    STMT_THROW,
    STMT_TRY
} StmtKind;

typedef struct SwitchCase {
    Expr *value;
    Stmt **body;
    int body_count;
} SwitchCase;

typedef struct CatchClause {
    char *variable;
    Stmt **body;
    int body_count;
} CatchClause;

typedef struct Stmt {
    StmtKind kind;
    union {
        struct {
            Expr *expression;
        } expression;
        struct {
            bool is_const;
            char *name;
            Expr *initializer;
        } var_decl;
        struct {
            Expr *value;
        } return_stmt;
        struct {
            Expr *condition;
            Stmt **then_body;
            int then_count;
            Stmt **else_body;
            int else_count;
        } if_stmt;
        struct {
            Expr *condition;
            Stmt **body;
            int body_count;
        } while_stmt;
        struct {
            char *variable;
            Expr *iterable;
            Stmt **body;
            int body_count;
        } for_stmt;
        struct {
            Expr *expression;
            SwitchCase *cases;
            int case_count;
        } switch_stmt;
        struct {
            Expr *expression;
        } throw_stmt;
        struct {
            Stmt **try_body;
            int try_count;
            CatchClause *catch_clauses;
            int catch_count;
            Stmt **finally_body;
            int finally_count;
        } try_stmt;
    } data;
} Stmt;

/* ============== Declarations ============== */

typedef enum {
    DECL_FUNCTION,
    DECL_CLASS,
    DECL_ENUM,
    DECL_IMPORT
} DeclKind;

typedef struct FunctionParam {
    char *name;
} FunctionParam;

typedef struct ClassField {
    char *name;
} ClassField;

typedef struct EnumVariant {
    char *name;
    int value;
    bool has_value;
} EnumVariant;

typedef struct Decl {
    DeclKind kind;
    union {
        struct {
            char *name;
            FunctionParam *params;
            int param_count;
            Stmt **body;
            int body_count;
        } function;
        struct {
            char *name;
            char *base_class;
            ClassField *fields;
            int field_count;
            Decl **methods;
            int method_count;
        } class_decl;
        struct {
            char *name;
            EnumVariant *variants;
            int variant_count;
        } enum_decl;
        struct {
            char *module_name;
            char **items;
            int item_count;
            bool import_all;
        } import_decl;
    } data;
} Decl;

/* ============== Program ============== */

typedef struct Program {
    Decl **declarations;
    int decl_count;
    Stmt **statements;
    int stmt_count;
} Program;

/* ============== Memory management ============== */

void free_expr(Expr *expr);
void free_stmt(Stmt *stmt);
void free_decl(Decl *decl);
void free_program(Program *program);

#endif /* LUNA_AST_H */
