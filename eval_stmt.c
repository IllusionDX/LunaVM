/* Statement executor implementation.
 * Handles all statement types in the Luna language.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eval.h"

/* ============== Forward declarations ============== */

static ControlFlow execute_var_decl(Environment *env, Stmt *stmt);
static ControlFlow execute_return(Environment *env, Stmt *stmt);
static ControlFlow execute_throw(Environment *env, Stmt *stmt);

/* ============== Control flow helpers ============== */

ControlFlow make_control_flow(void) {
    ControlFlow flow;
    flow.has_exception = false;
    flow.is_break = false;
    flow.is_continue = false;
    flow.is_return = false;
    flow.return_value = make_null();
    flow.exception = make_null();
    return flow;
}

ControlFlow exception(Value value) {
    ControlFlow flow = make_control_flow();
    flow.has_exception = true;
    flow.exception = value;
    return flow;
}

ControlFlow break_flow(void) {
    ControlFlow flow = make_control_flow();
    flow.is_break = true;
    return flow;
}

ControlFlow continue_flow(void) {
    ControlFlow flow = make_control_flow();
    flow.is_continue = true;
    return flow;
}

ControlFlow return_flow(Value value) {
    ControlFlow flow = make_control_flow();
    flow.is_return = true;
    flow.return_value = value;
    return flow;
}

/* ============== Statement execution ============== */

ControlFlow execute_stmt(Environment *env, Stmt *stmt) {
    if (!stmt) return make_control_flow();
    
    switch (stmt->kind) {
        case STMT_EXPRESSION:
            evaluate_expr(env, stmt->data.expression.expression);
            return make_control_flow();
            
        case STMT_VAR_DECL:
            return execute_var_decl(env, stmt);
            
        case STMT_RETURN:
            return execute_return(env, stmt);
            
        case STMT_PASS:
            return make_control_flow();
            
        case STMT_BREAK:
            return break_flow();
            
        case STMT_CONTINUE:
            return continue_flow();
            
        case STMT_IF:
            return execute_if(env, stmt);
            
        case STMT_WHILE:
            return execute_while(env, stmt);
            
        case STMT_FOR:
            return execute_for(env, stmt);
            
        case STMT_SWITCH:
            return execute_switch(env, stmt);
            
        case STMT_THROW:
            return execute_throw(env, stmt);
            
        case STMT_TRY:
            return execute_try(env, stmt);
            
        default:
            fprintf(stderr, "Unknown statement type: %d\n", stmt->kind);
            return make_control_flow();
    }
}

/* ============== Block execution ============== */

ControlFlow execute_block(Environment *env, Stmt **stmts, int count) {
    ControlFlow flow = make_control_flow();
    
    for (int i = 0; i < count; i++) {
        flow = execute_stmt(env, stmts[i]);
        
        if (flow.has_exception || flow.is_break || 
            flow.is_continue || flow.is_return) {
            break;
        }
    }
    
    return flow;
}

/* ============== Variable declaration ============== */

static ControlFlow execute_var_decl(Environment *env, Stmt *stmt) {
    bool is_const = stmt->data.var_decl.is_const;
    const char *name = stmt->data.var_decl.name;
    Type *type = stmt->data.var_decl.var_type;
    Expr *initializer = stmt->data.var_decl.initializer;
    
    Value value = make_null();
    
    if (initializer) {
        value = evaluate_expr(env, initializer);
    }
    
    /* If type is not specified, infer from value */
    if (!type) {
        type = value_to_type(value);
    }
    
    define_variable(env, name, value, type, is_const);
    
    return make_control_flow();
}

/* ============== Return statement ============== */

static ControlFlow execute_return(Environment *env, Stmt *stmt) {
    Value value = make_null();
    
    if (stmt->data.return_stmt.value) {
        value = evaluate_expr(env, stmt->data.return_stmt.value);
    }
    
    return return_flow(value);
}

/* ============== If statement ============== */

ControlFlow execute_if(Environment *env, Stmt *stmt) {
    Value condition = evaluate_expr(env, stmt->data.if_stmt.condition);
    
    if (is_truthy(condition)) {
        push_scope(env);
        ControlFlow flow = execute_block(current_env(), stmt->data.if_stmt.then_body,
                                       stmt->data.if_stmt.then_count);
        pop_scope();
        return flow;
    } else if (stmt->data.if_stmt.else_body) {
        push_scope(env);
        ControlFlow flow = execute_block(current_env(), stmt->data.if_stmt.else_body,
                                       stmt->data.if_stmt.else_count);
        pop_scope();
        return flow;
    }
    
    return make_control_flow();
}

/* ============== While statement ============== */

ControlFlow execute_while(Environment *env, Stmt *stmt) {
    while (true) {
        Value condition = evaluate_expr(env, stmt->data.while_stmt.condition);
        
        if (!is_truthy(condition)) {
            break;
        }
        
        push_scope(env);
        ControlFlow flow = execute_block(current_env(), stmt->data.while_stmt.body,
                                       stmt->data.while_stmt.body_count);
        pop_scope();
        
        if (flow.has_exception || flow.is_return) {
            return flow;
        }
        
        if (flow.is_break) {
            break;
        }
        
        if (flow.is_continue) {
            continue;
        }
    }
    
    return make_control_flow();
}

/* ============== For statement ============== */

ControlFlow execute_for(Environment *env, Stmt *stmt) {
    const char *var_name = stmt->data.for_stmt.variable;
    Value iterable = evaluate_expr(env, stmt->data.for_stmt.iterable);
    
    /* Handle range iteration */
    if (iterable.type == VAL_OBJ && iterable.as.obj->type == OBJ_LIST) {
        ObjList *list = (ObjList *)iterable.as.obj;
        
        for (int i = 0; i < list->count; i++) {
            push_scope(env);
            define_variable(current_env(), var_name, list->items[i], NULL, false);
            
            ControlFlow flow = execute_block(current_env(), stmt->data.for_stmt.body,
                                           stmt->data.for_stmt.body_count);
            
            pop_scope();
            
            if (flow.has_exception || flow.is_return) {
                release_obj(iterable.as.obj);
                return flow;
            }
            
            if (flow.is_break) {
                break;
            }
            
            if (flow.is_continue) {
                continue;
            }
        }
    }
    
    if (iterable.type == VAL_OBJ) {
        release_obj(iterable.as.obj);
    }
    
    return make_control_flow();
}

/* ============== Switch statement ============== */

ControlFlow execute_switch(Environment *env, Stmt *stmt) {
    Value switch_value = evaluate_expr(env, stmt->data.switch_stmt.expression);
    ControlFlow flow = make_control_flow();
    bool matched = false;
    
    /* Find matching case */
    for (int i = 0; i < stmt->data.switch_stmt.case_count; i++) {
        SwitchCase *case_clause = &stmt->data.switch_stmt.cases[i];
        
        if (case_clause->value == NULL) {
            /* Default case - execute if no match yet */
            if (!matched) {
                push_scope(env);
                flow = execute_block(current_env(), case_clause->body, case_clause->body_count);
                pop_scope();
            }
        } else {
            Value case_value = evaluate_expr(env, case_clause->value);
            
            if (values_equal(switch_value, case_value)) {
                matched = true;
                push_scope(env);
                flow = execute_block(current_env(), case_clause->body, case_clause->body_count);
                pop_scope();
                
                if (case_value.type == VAL_OBJ) {
                    release_obj(case_value.as.obj);
                }
                
                /* Fall through to next cases */
                if (flow.has_exception || flow.is_return || flow.is_break) {
                    break;
                }
            }
            
            if (case_value.type == VAL_OBJ) {
                release_obj(case_value.as.obj);
            }
        }
    }
    
    /* Clear break flag since we handled it */
    if (flow.is_break) {
        flow.is_break = false;
    }
    
    if (switch_value.type == VAL_OBJ) {
        release_obj(switch_value.as.obj);
    }
    
    return flow;
}

/* ============== Throw statement ============== */

static ControlFlow execute_throw(Environment *env, Stmt *stmt) {
    Value exception_value = evaluate_expr(env, stmt->data.throw_stmt.expression);
    
    /* Wrap in exception object if not already */
    if (!(exception_value.type == VAL_OBJ && exception_value.as.obj->type == OBJ_EXCEPTION)) {
        char *msg = value_to_string(exception_value);
        ObjException *exc = new_exception(msg);
        free(msg);
        exception_value = make_obj((Object *)exc);
    }
    
    return exception(exception_value);
}

/* ============== Try statement ============== */

ControlFlow execute_try(Environment *env, Stmt *stmt) {
    ControlFlow flow = make_control_flow();
    
    /* Execute try block */
    push_scope(env);
    flow = execute_block(current_env(), stmt->data.try_stmt.try_body,
                        stmt->data.try_stmt.try_count);
    pop_scope();
    
    /* Handle exception if thrown */
    if (flow.has_exception) {
        Value caught = flow.exception;
        flow.has_exception = false;
        flow.exception = make_null();
        
        /* Try to catch with catch clauses */
        bool caught_by_handler = false;
        for (int i = 0; i < stmt->data.try_stmt.catch_count; i++) {
            CatchClause *catch_clause = &stmt->data.try_stmt.catch_clauses[i];
            
            /* For now, catch all exceptions */
            push_scope(env);
            define_variable(current_env(), catch_clause->variable, caught, NULL, false);
            
            flow = execute_block(current_env(), catch_clause->body, catch_clause->body_count);
            pop_scope();
            
            caught_by_handler = true;
            
            if (flow.has_exception || flow.is_return || flow.is_break || flow.is_continue) {
                break;
            }
        }
        
        /* If not caught by any handler, rethrow */
        if (!caught_by_handler) {
            return exception(caught);
        }
        
        if (caught.type == VAL_OBJ) {
            release_obj(caught.as.obj);
        }
    }
    
    /* Execute finally block if present */
    if (stmt->data.try_stmt.finally_body) {
        ControlFlow finally_flow = execute_block(env, stmt->data.try_stmt.finally_body,
                                                stmt->data.try_stmt.finally_count);
        
        /* Finally can override control flow except exceptions */
        if (finally_flow.has_exception) {
            return finally_flow;
        }
        
        if (finally_flow.is_return) {
            return finally_flow;
        }
        
        if (finally_flow.is_break) {
            return finally_flow;
        }
        
        if (finally_flow.is_continue) {
            return finally_flow;
        }
    }
    
    return flow;
}
