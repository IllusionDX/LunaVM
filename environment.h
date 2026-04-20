/* Environment for variable and function storage.
 * Supports nested scopes and proper cleanup.
 */

#ifndef LUNA_ENVIRONMENT_H
#define LUNA_ENVIRONMENT_H

#include "value.h"

/* Variable entry */
typedef struct Variable {
    char *name;
    Value value;
    Type *type;
    bool is_const;
    struct Variable *next;
} Variable;

/* Environment (scope) */
typedef struct Environment {
    Variable *variables;
    struct Environment *enclosing;
    Decl **declarations;
    int decl_count;
    int decl_capacity;
} Environment;

/* Global environment state */
typedef struct GlobalState {
    Environment *globals;
    Environment *current;
    Object *objects;
    int gray_count;
    int gray_capacity;
    Object **gray_stack;
} GlobalState;

/* ============== Environment operations ============== */

Environment *new_environment(Environment *enclosing);
void free_environment(Environment *env);

/* ============== Variable operations ============== */

void define_variable(Environment *env, const char *name, Value value, Type *type, bool is_const);
Variable *get_variable(Environment *env, const char *name);
Value get_variable_value(Environment *env, const char *name);
void set_variable(Environment *env, const char *name, Value value);
bool is_variable_defined(Environment *env, const char *name);

/* ============== Declaration operations ============== */

void define_declaration(Environment *env, Decl *decl);
Decl *get_declaration(Environment *env, const char *name);
bool is_declaration_defined(Environment *env, const char *name);

/* ============== Global state ============== */

void init_global_state(void);
void cleanup_global_state(void);
GlobalState *get_global_state(void);

/* ============== Scope management ============== */

void push_scope(Environment *env);
void pop_scope(void);
Environment *current_env(void);
Environment *global_env(void);

#endif /* LUNA_ENVIRONMENT_H */
