/* Declaration parsing for Luna interpreter.
 * Functions for parsing function, class, enum, and import declarations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "parse_decl.h"
#include "parse_stmt.h"
#include "parse_expr.h"

/* ============== Parameter list parsing ============== */

FunctionParam *parse_parameters(Parser *parser, int *count) {
    *count = 0;
    if (match(parser, TOK_RPAREN)) return NULL;

    int capacity = 4;
    FunctionParam *params = (FunctionParam *)malloc(capacity * sizeof(FunctionParam));

    while (!match(parser, TOK_RPAREN) && !match(parser, TOK_EOF)) {
        if (*count >= capacity) {
            capacity *= 2;
            params = (FunctionParam *)realloc(params, capacity * sizeof(FunctionParam));
        }

        Token *name = expect(parser, TOK_IDENTIFIER, "Expected parameter name");
        params[*count].name = strdup(name ? name->value : "");

        if (match(parser, TOK_COLON)) {
            advance(parser);
            skip_type_hint(parser);
        }

        (*count)++;
        if (match(parser, TOK_COMMA)) advance(parser);
    }
    return params;
}

/* ============== Function declaration ============== */

static Decl *parse_function_declaration(Parser *parser) {
    expect(parser, TOK_DEF, "Expected 'def'");
    Token *name = expect(parser, TOK_IDENTIFIER, "Expected function name");

    expect(parser, TOK_LPAREN, "Expected '(' after function name");
    int param_count = 0;
    FunctionParam *params = parse_parameters(parser, &param_count);
    expect(parser, TOK_RPAREN, "Expected ')' after parameters");

    if (match(parser, TOK_ARROW)) {
        advance(parser);
        skip_type_hint(parser);
    }

    expect(parser, TOK_COLON, "Expected ':' after function declaration");

    int body_count = 0;
    Stmt **body = NULL;
    if (match(parser, TOK_NEWLINE)) {
        advance(parser);
        body = parse_block(parser, &body_count);
    } else {
        body = (Stmt **)malloc(sizeof(Stmt *));
        body[0] = parse_statement(parser);
        body_count = 1;
    }

    Decl *decl = make_decl(DECL_FUNCTION);
    decl->data.function.name = strdup(name ? name->value : "");
    decl->data.function.params = params;
    decl->data.function.param_count = param_count;
    decl->data.function.body = body;
    decl->data.function.body_count = body_count;
    return decl;
}

/* ============== Class declaration ============== */

static Decl *parse_class_declaration(Parser *parser) {
    expect(parser, TOK_CLASS, "Expected 'class'");
    Token *name = expect(parser, TOK_IDENTIFIER, "Expected class name");

    char *base_class = NULL;
    if (match(parser, TOK_EXTENDS)) {
        advance(parser);
        Token *base = expect(parser, TOK_IDENTIFIER, "Expected base class name");
        base_class = strdup(base ? base->value : "");
    }

    expect(parser, TOK_COLON, "Expected ':' after class name");
    expect_newline(parser);
    expect(parser, TOK_INDENT, "Expected indentation in class");

    int field_cap = 4, method_cap = 4;
    ClassField *fields = (ClassField *)malloc(field_cap * sizeof(ClassField));
    Decl **methods = (Decl **)malloc(method_cap * sizeof(Decl *));
    int field_count = 0, method_count = 0;

    while (!match2(parser, TOK_DEDENT, TOK_EOF)) {
        if (match(parser, TOK_NEWLINE)) { advance(parser); continue; }

        if (match(parser, TOK_DEF)) {
            if (method_count >= method_cap) { method_cap *= 2; methods = realloc(methods, method_cap * sizeof(Decl *)); }
            methods[method_count++] = parse_function_declaration(parser);
            if (match(parser, TOK_NEWLINE)) advance(parser);
        } else if (match(parser, TOK_VAR) || match(parser, TOK_IDENTIFIER)) {
            if (field_count >= field_cap) { field_cap *= 2; fields = realloc(fields, field_cap * sizeof(ClassField)); }
            if (match(parser, TOK_VAR)) advance(parser);
            Token *fname = expect(parser, TOK_IDENTIFIER, "Expected field name");
            fields[field_count].name = strdup(fname ? fname->value : "");
            if (match(parser, TOK_COLON)) {
                advance(parser);
                skip_type_hint(parser);
            }
            field_count++;
            expect_newline(parser);
        } else {
            parser_error(parser, "Expected field name or method definition in class body");
            advance(parser); /* skip unknown token to avoid infinite loop */
        }
    }
    expect(parser, TOK_DEDENT, "Expected dedent after class");

    Decl *decl = make_decl(DECL_CLASS);
    decl->data.class_decl.name = strdup(name ? name->value : "");
    decl->data.class_decl.base_class = base_class;
    decl->data.class_decl.fields = fields;
    decl->data.class_decl.field_count = field_count;
    decl->data.class_decl.methods = methods;
    decl->data.class_decl.method_count = method_count;
    return decl;
}

/* ============== Enum declaration ============== */

static Decl *parse_enum_declaration(Parser *parser) {
    expect(parser, TOK_ENUM, "Expected 'enum'");
    Token *name = expect(parser, TOK_IDENTIFIER, "Expected enum name");
    expect(parser, TOK_COLON, "Expected ':'");
    expect_newline(parser);
    expect(parser, TOK_INDENT, "Expected indentation in enum");

    int capacity = 4;
    EnumVariant *variants = (EnumVariant *)malloc(capacity * sizeof(EnumVariant));
    int variant_count = 0;
    int auto_value = 0;

    while (!match2(parser, TOK_DEDENT, TOK_EOF)) {
        if (match(parser, TOK_NEWLINE)) { advance(parser); continue; }
        if (variant_count >= capacity) { capacity *= 2; variants = realloc(variants, capacity * sizeof(EnumVariant)); }

        Token *vname = expect(parser, TOK_IDENTIFIER, "Expected variant name");
        variants[variant_count].name = strdup(vname ? vname->value : "");

        if (match(parser, TOK_ASSIGN)) {
            advance(parser);
            Token *val = expect(parser, TOK_INTEGER_LITERAL, "Expected integer value");
            variants[variant_count].value = val ? atoi(val->value) : 0;
            auto_value = variants[variant_count].value + 1;
            variants[variant_count].has_value = true;
        } else {
            variants[variant_count].value = auto_value++;
            variants[variant_count].has_value = false;
        }
        variant_count++;
        expect_newline(parser);
    }
    expect(parser, TOK_DEDENT, "Expected dedent after enum");

    Decl *decl = make_decl(DECL_ENUM);
    decl->data.enum_decl.name = strdup(name ? name->value : "");
    decl->data.enum_decl.variants = variants;
    decl->data.enum_decl.variant_count = variant_count;
    return decl;
}

/* ============== Import declaration ============== */

static Decl *parse_import_declaration(Parser *parser) {
    char *module_name = NULL;
    char **items = NULL;
    int item_count = 0;
    bool import_all = false;

    if (match(parser, TOK_FROM)) {
        advance(parser);
        Token *mod = expect(parser, TOK_IDENTIFIER, "Expected module name");
        module_name = strdup(mod ? mod->value : "");
        expect(parser, TOK_IMPORT, "Expected 'import'");

        if (match(parser, TOK_STAR)) {
            advance(parser);
            import_all = true;
        } else {
            int cap = 4;
            items = (char **)malloc(cap * sizeof(char *));
            Token *item = expect(parser, TOK_IDENTIFIER, "Expected item name");
            items[item_count++] = strdup(item ? item->value : "");
            while (match(parser, TOK_COMMA)) {
                advance(parser);
                if (item_count >= cap) { cap *= 2; items = realloc(items, cap * sizeof(char *)); }
                item = expect(parser, TOK_IDENTIFIER, "Expected item name");
                items[item_count++] = strdup(item ? item->value : "");
            }
        }
    } else {
        expect(parser, TOK_IMPORT, "Expected 'import'");
        Token *mod = expect(parser, TOK_IDENTIFIER, "Expected module name");
        module_name = strdup(mod ? mod->value : "");
    }

    Decl *decl = make_decl(DECL_IMPORT);
    decl->data.import_decl.module_name = module_name;
    decl->data.import_decl.items = items;
    decl->data.import_decl.item_count = item_count;
    decl->data.import_decl.import_all = import_all;
    return decl;
}

/* ============== Declaration detection and dispatch ============== */

bool is_declaration_start(Parser *parser) {
    TokenType t = peek(parser)->type;
    return t == TOK_DEF || t == TOK_CLASS ||
           t == TOK_IMPORT || t == TOK_FROM || t == TOK_ENUM;
}

Decl *parse_declaration(Parser *parser) {
    if (match(parser, TOK_DEF)) return parse_function_declaration(parser);
    if (match(parser, TOK_CLASS)) return parse_class_declaration(parser);
    if (match(parser, TOK_ENUM)) return parse_enum_declaration(parser);
    if (match(parser, TOK_IMPORT) || match(parser, TOK_FROM))
        return parse_import_declaration(parser);
    return NULL;
}
