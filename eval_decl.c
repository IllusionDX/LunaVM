/* Declaration execution implementation.
 * Handles all declaration types in the Luna language.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eval.h"

/* ============== Declaration execution ============== */

void execute_decl(Environment *env, Decl *decl) {
    if (!decl) return;
    
    switch (decl->kind) {
        case DECL_FUNCTION:
            execute_function_decl(env, decl);
            break;
            
        case DECL_STRUCT:
            execute_struct_decl(env, decl);
            break;
            
        case DECL_CLASS:
            execute_class_decl(env, decl);
            break;
            
        case DECL_ENUM:
            execute_enum_decl(env, decl);
            break;
            
        case DECL_IMPORT:
            execute_import_decl(env, decl);
            break;
            
        default:
            fprintf(stderr, "Unknown declaration type: %d\n", decl->kind);
            break;
    }
}

/* ============== Function declaration ============== */

void execute_function_decl(Environment *env, Decl *decl) {
    /* Create a function object */
    ObjFunction *func = new_function(decl->data.function.name);
    
    /* Copy parameters */
    func->param_count = decl->data.function.param_count;
    if (func->param_count > 0) {
        func->params = (FunctionParam *)malloc(func->param_count * sizeof(FunctionParam));
        if (func->params) {
            for (int i = 0; i < func->param_count; i++) {
                func->params[i] = decl->data.function.params[i];
                /* Duplicate strings to avoid reference issues */
                if (func->params[i].name) {
                    func->params[i].name = strdup(func->params[i].name);
                }
            }
        }
    }
    
    /* Copy return type */
    func->return_type = decl->data.function.return_type;
    
    /* Copy body (reference, not copy) */
    func->body_count = decl->data.function.body_count;
    func->body = decl->data.function.body;
    
    /* Set closure to current environment */
    func->closure = env;
    
    /* Define the function as a variable */
    Value func_value = make_obj((Object *)func);
    define_variable(env, decl->data.function.name, func_value, 
                   func->return_type ? func->return_type : NULL, true);
}

/* ============== Struct declaration ============== */

void execute_struct_decl(Environment *env, Decl *decl) {
    /* Store struct declaration in environment */
    /* For simplicity, we store the declaration itself */
    define_declaration(env, decl);
}

/* ============== Class declaration ============== */

void execute_class_decl(Environment *env, Decl *decl) {
    /* Store class declaration in environment */
    define_declaration(env, decl);
}

/* ============== Enum declaration ============== */

void execute_enum_decl(Environment *env, Decl *decl) {
    /* Create an enum type by storing each variant as a constant */
    for (int i = 0; i < decl->data.enum_decl.variant_count; i++) {
        EnumVariant *variant = &decl->data.enum_decl.variants[i];
        
        /* Create full name: EnumName.VARIANT */
        char full_name[256];
        snprintf(full_name, sizeof(full_name), "%s.%s", 
                decl->data.enum_decl.name, variant->name);
        
        /* Define as a constant integer */
        Value value = make_int(variant->value);
        define_variable(env, full_name, value, NULL, true);
    }
    
    /* Also store the declaration for type checking */
    define_declaration(env, decl);
}

/* ============== Import declaration ============== */

void execute_import_decl(Environment *env, Decl *decl) {
    /* For now, imports are no-ops in the interpreter */
    /* In a full implementation, this would load and execute the imported module */
    (void)env;
    (void)decl;
}

/* ============== Program execution ============== */

void execute_program(Program *program) {
    if (!program) return;
    
    /* Initialize global state */
    init_global_state();
    
    Environment *env = global_env();
    
    /* Register native functions */
    register_natives(env);
    init_type_methods();
    
    /* Execute all declarations first (for top-level function/class definitions) */
    for (int i = 0; i < program->decl_count; i++) {
        execute_decl(env, program->declarations[i]);
    }
    
    /* Then execute top-level statements */
    for (int i = 0; i < program->stmt_count; i++) {
        ControlFlow flow = execute_stmt(env, program->statements[i]);
        
        if (flow.has_exception) {
            char *msg = value_to_string(flow.exception);
            fprintf(stderr, "Uncaught exception: %s\n", msg);
            free(msg);
            if (flow.exception.type == VAL_OBJ) {
                release_obj(flow.exception.as.obj);
            }
            break;
        }
        
        if (flow.is_return) {
            /* Top-level return - just continue */
            if (flow.return_value.type == VAL_OBJ) {
                release_obj(flow.return_value.as.obj);
            }
        }
    }
    
    /* Cleanup */
    cleanup_global_state();
}
