/* Environment implementation for variable and function storage.
 * Supports nested scopes and proper memory management.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "environment.h"

static GlobalState global_state;
static int initialized = 0;

/* ============== Environment operations ============== */

Environment *new_environment(Environment *enclosing) {
    Environment *env = (Environment *)malloc(sizeof(Environment));
    if (!env) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    env->variables = NULL;
    env->enclosing = enclosing;
    env->declarations = NULL;
    env->decl_count = 0;
    env->decl_capacity = 0;
    
    return env;
}

static void free_variable(Variable *var) {
    if (!var) return;
    
    free(var->name);
    if (var->value.type == VAL_OBJ && var->value.as.obj) {
        release_obj(var->value.as.obj);
    }
    if (var->type) {
        free_type(var->type);
    }
    free(var);
}

void free_environment(Environment *env) {
    if (!env) return;
    
    Variable *var = env->variables;
    while (var) {
        Variable *next = var->next;
        free_variable(var);
        var = next;
    }
    
    if (env->declarations) {
        free(env->declarations);
    }
    
    free(env);
}

/* ============== Variable operations ============== */

void define_variable(Environment *env, const char *name, Value value, Type *type, bool is_const) {
    Variable *var = (Variable *)malloc(sizeof(Variable));
    if (!var) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    
    var->name = strdup(name);
    var->value = value;
    var->type = type;
    var->is_const = is_const;
    var->next = env->variables;
    
    env->variables = var;
    
    if (value.type == VAL_OBJ && value.as.obj) {
        retain_obj(value.as.obj);
    }
}

Variable *get_variable(Environment *env, const char *name) {
    Environment *current = env;
    
    while (current) {
        Variable *var = current->variables;
        while (var) {
            if (strcmp(var->name, name) == 0) {
                return var;
            }
            var = var->next;
        }
        current = current->enclosing;
    }
    
    return NULL;
}

Value get_variable_value(Environment *env, const char *name) {
    Variable *var = get_variable(env, name);
    if (var) {
        return var->value;
    }
    
    fprintf(stderr, "Undefined variable: %s\n", name);
    return make_null();
}

void set_variable(Environment *env, const char *name, Value value) {
    Variable *var = get_variable(env, name);
    
    if (var) {
        if (var->is_const) {
            fprintf(stderr, "Cannot assign to constant variable: %s\n", name);
            return;
        }
        
        if (var->value.type == VAL_OBJ && var->value.as.obj) {
            release_obj(var->value.as.obj);
        }
        
        var->value = value;
        
        if (value.type == VAL_OBJ && value.as.obj) {
            retain_obj(value.as.obj);
        }
    } else {
        fprintf(stderr, "Undefined variable: %s\n", name);
    }
}

bool is_variable_defined(Environment *env, const char *name) {
    return get_variable(env, name) != NULL;
}

/* ============== Declaration operations ============== */

void define_declaration(Environment *env, Decl *decl) {
    if (env->decl_count >= env->decl_capacity) {
        int new_capacity = env->decl_capacity < 8 ? 8 : env->decl_capacity * 2;
        Decl **new_decls = (Decl **)realloc(env->declarations, new_capacity * sizeof(Decl *));
        if (!new_decls) {
            fprintf(stderr, "Out of memory\n");
            return;
        }
        env->declarations = new_decls;
        env->decl_capacity = new_capacity;
    }
    
    env->declarations[env->decl_count++] = decl;
}

Decl *get_declaration(Environment *env, const char *name) {
    Environment *current = env;
    
    while (current) {
        for (int i = 0; i < current->decl_count; i++) {
            Decl *decl = current->declarations[i];
            const char *decl_name = NULL;
            
            switch (decl->kind) {
                case DECL_FUNCTION:
                    decl_name = decl->data.function.name;
                    break;
                case DECL_STRUCT:
                    decl_name = decl->data.struct_decl.name;
                    break;
                case DECL_CLASS:
                    decl_name = decl->data.class_decl.name;
                    break;
                case DECL_ENUM:
                    decl_name = decl->data.enum_decl.name;
                    break;
                case DECL_IMPORT:
                    decl_name = decl->data.import_decl.module_name;
                    break;
            }
            
            if (decl_name && strcmp(decl_name, name) == 0) {
                return decl;
            }
        }
        current = current->enclosing;
    }
    
    return NULL;
}

bool is_declaration_defined(Environment *env, const char *name) {
    return get_declaration(env, name) != NULL;
}

/* ============== Global state ============== */

void init_global_state(void) {
    if (initialized) return;
    
    global_state.globals = new_environment(NULL);
    global_state.current = global_state.globals;
    global_state.objects = NULL;
    global_state.gray_count = 0;
    global_state.gray_capacity = 0;
    global_state.gray_stack = NULL;
    
    initialized = 1;
}

void cleanup_global_state(void) {
    if (!initialized) return;
    
    /* Free all objects in the object list */
    Object *obj = global_state.objects;
    while (obj) {
        Object *next = obj->next;
        free_object(obj);
        obj = next;
    }
    
    /* Free environments */
    if (global_state.globals) {
        free_environment(global_state.globals);
    }
    
    free(global_state.gray_stack);
    
    initialized = 0;
}

GlobalState *get_global_state(void) {
    return &global_state;
}

/* ============== Scope management ============== */

void push_scope(Environment *env) {
    Environment *new_scope = new_environment(env);
    global_state.current = new_scope;
}

void pop_scope(void) {
    if (global_state.current && global_state.current != global_state.globals) {
        Environment *to_free = global_state.current;
        global_state.current = global_state.current->enclosing;
        free_environment(to_free);
    }
}

Environment *current_env(void) {
    if (!initialized) {
        init_global_state();
    }
    return global_state.current;
}

Environment *global_env(void) {
    if (!initialized) {
        init_global_state();
    }
    return global_state.globals;
}
