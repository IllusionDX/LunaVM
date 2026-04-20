/* Function call, field access, index access, and new expression evaluation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "eval.h"

/* ============== Forward declarations ============== */

static Value call_function(Environment *env, ObjFunction *func, Value *args, int arg_count);

/* ============== Function call evaluation ============== */

Value evaluate_call(Environment *env, Expr *expr) {
    Expr *callee = expr->data.call.callee;
    int arg_count = expr->data.call.arg_count;
    
    /* Evaluate arguments with ARC */
    Value *args = (Value *)malloc(arg_count * sizeof(Value));
    if (!args && arg_count > 0) {
        fprintf(stderr, "Out of memory\n");
        return make_null();
    }
    
    for (int i = 0; i < arg_count; i++) {
        args[i] = evaluate_expr(env, expr->data.call.arguments[i]);
        if (args[i].type == VAL_OBJ) {
            retain_obj(args[i].as.obj);
        }
    }
    
    /* Get the function name/identifier */
    if (callee->kind == EXPR_IDENTIFIER) {
        const char *func_name = callee->data.identifier.name;
        
        /* Look up function in environment */
        Variable *var = get_variable(env, func_name);
        /* Check if looking up a native or user function */
        if (var && var->value.type == VAL_OBJ && var->value.as.obj->type == OBJ_FUNCTION) {
            ObjFunction *func = (ObjFunction *)var->value.as.obj;
            Value result = call_function(env, func, args, arg_count);
            
            for (int i = 0; i < arg_count; i++) {
                if (args[i].type == VAL_OBJ) {
                    release_obj(args[i].as.obj);
                }
            }
            free(args);
            return result;
        }
        
        fprintf(stderr, "Undefined function: %s\n", func_name);
        free(args);
        return make_null();
    }
    
    /* Method calls (field access with call) */
    if (callee->kind == EXPR_FIELD_ACCESS) {
        Value obj = evaluate_expr(env, callee->data.field_access.obj);
        const char *field_name = callee->data.field_access.field;
        
        /* Generic object method dispatch */
        if (obj.type == VAL_OBJ) {
            ObjFunction **methods = NULL;
            int method_count = 0;
            
            switch (obj.as.obj->type) {
                case OBJ_INSTANCE:
                    methods = ((ObjInstance *)obj.as.obj)->methods;
                    method_count = ((ObjInstance *)obj.as.obj)->method_count;
                    break;
                case OBJ_LIST:
                    methods = ((ObjList *)obj.as.obj)->methods;
                    method_count = ((ObjList *)obj.as.obj)->method_count;
                    break;
                case OBJ_MAP:
                    methods = ((ObjMap *)obj.as.obj)->methods;
                    method_count = ((ObjMap *)obj.as.obj)->method_count;
                    break;
                default: break;
            }
            
            if (methods) {
                for (int i = 0; i < method_count; i++) {
                    if (strcmp(methods[i]->name, field_name) == 0) {
                        ObjFunction *func = methods[i];
                        Environment *method_env = new_environment(env);
                        define_variable(method_env, "self", obj, NULL, true);
                        Value result = make_null();
                        
                        if (func->is_native) {
                            result = func->native_fn(method_env, args, arg_count);
                        } else {
                            for (int j = 0; j < func->param_count && j < arg_count; j++) {
                                define_variable(method_env, func->params[j].name, 
                                              args[j], func->params[j].param_type, false);
                            }
                            ControlFlow flow = execute_block(method_env, func->body, func->body_count);
                            if (flow.is_return) result = flow.return_value;
                        }
                        
                        free_environment(method_env);
                        for (int k = 0; k < arg_count; k++) {
                            if (args[k].type == VAL_OBJ) release_obj(args[k].as.obj);
                        }
                        free(args);
                        return result;
                    }
                }
            }
        }
        
        fprintf(stderr, "Unknown method: %s\n", field_name);
        free(args);
        return make_null();
    }
    
    fprintf(stderr, "Cannot call expression\n");
    free(args);
    return make_null();
}

/* ============== Field access evaluation ============== */

Value evaluate_field_access(Environment *env, Expr *expr) {
    Value obj = evaluate_expr(env, expr->data.field_access.obj);
    const char *field = expr->data.field_access.field;

    /* Handle list properties */
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_LIST) {
        ObjList *list = (ObjList *)obj.as.obj;
        if (strcmp(field, "length") == 0) {
            return make_int(list->count);
        }
    }

    /* Handle map properties */
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_MAP) {
        ObjMap *map = (ObjMap *)obj.as.obj;
        if (strcmp(field, "length") == 0) {
            return make_int(map->entry_count);
        }
    }

    /* Handle string properties */
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_STRING) {
        ObjString *str = (ObjString *)obj.as.obj;
        if (strcmp(field, "length") == 0) {
            return make_int(str->length);
        }
    }

    /* Handle instance fields */
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_INSTANCE) {
        ObjInstance *inst = (ObjInstance *)obj.as.obj;
        return instance_get_field(inst, field);
    }

    /* Handle struct fields */
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_STRUCT) {
        ObjStruct *strct = (ObjStruct *)obj.as.obj;

        /* Look up field by name - simplified */
        /* In a real implementation, we'd have proper field mapping */
        for (int i = 0; i < strct->field_count; i++) {
            /* We'd check field names here */
            /* For now, return the field value */
            return strct->fields[i];
        }
    }

    /* Handle enum value access (e.g., Status.PENDING) */
    if (expr->data.field_access.obj->kind == EXPR_IDENTIFIER) {
        const char *obj_name = expr->data.field_access.obj->data.identifier.name;
        Decl *decl = get_declaration(env, obj_name);
        if (decl && decl->kind == DECL_ENUM) {
            char full_name[256];
            snprintf(full_name, sizeof(full_name), "%s.%s", obj_name, field);
            Variable *var = get_variable(env, full_name);
            if (var) return var->value;
        }
    }
    
    return make_null();
}

/* ============== Index access evaluation ============== */

Value evaluate_index_access(Environment *env, Expr *expr) {
    Value obj = evaluate_expr(env, expr->data.index_access.obj);
    Value index = evaluate_expr(env, expr->data.index_access.index);
    
    if (obj.type == VAL_OBJ) {
        if (obj.as.obj->type == OBJ_LIST) {
            ObjList *list = (ObjList *)obj.as.obj;
            if (index.type == VAL_INT) {
                return list_get(list, index.as.integer);
            }
        }
        
        if (obj.as.obj->type == OBJ_MAP) {
            ObjMap *map = (ObjMap *)obj.as.obj;
            return map_get(map, index);
        }
        
        if (obj.as.obj->type == OBJ_STRING) {
            ObjString *str = (ObjString *)obj.as.obj;
            if (index.type == VAL_INT) {
                int idx = index.as.integer;
                if (idx >= 0 && idx < str->length) {
                    return make_char(str->chars[idx]);
                }
            }
        }
    }
    
    fprintf(stderr, "Cannot index object\n");
    return make_null();
}

/* ============== Assignment evaluation ============== */

Value evaluate_assignment(Environment *env, Expr *expr) {
    if (expr->kind == EXPR_ASSIGNMENT) {
        Expr *target = expr->data.assignment.target;
        Value value = evaluate_expr(env, expr->data.assignment.value);
        
        /* Simple variable assignment */
        if (target->kind == EXPR_IDENTIFIER) {
            const char *name = target->data.identifier.name;
            set_variable(env, name, value);
            return value;
        }
        
        /* Field assignment */
        if (target->kind == EXPR_FIELD_ACCESS) {
            Value obj = evaluate_expr(env, target->data.field_access.obj);
            const char *field = target->data.field_access.field;
            
            /* Handle instance field assignment */
            if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_INSTANCE) {
                ObjInstance *inst = (ObjInstance *)obj.as.obj;
                instance_set_field(inst, field, value);
                return value;
            }
        }
        
        /* Index assignment */
        if (target->kind == EXPR_INDEX_ACCESS) {
            Value obj = evaluate_expr(env, target->data.index_access.obj);
            Value index = evaluate_expr(env, target->data.index_access.index);
            
            if (obj.type == VAL_OBJ) {
                if (obj.as.obj->type == OBJ_LIST && index.type == VAL_INT) {
                    ObjList *list = (ObjList *)obj.as.obj;
                    list_set(list, index.as.integer, value);
                    return value;
                }
                
                if (obj.as.obj->type == OBJ_MAP) {
                    ObjMap *map = (ObjMap *)obj.as.obj;
                    map_set(map, index, value);
                    return value;
                }
            }
        }
        
        fprintf(stderr, "Invalid assignment target\n");
        return make_null();
    }
    
    if (expr->kind == EXPR_COMPOUND_ASSIGN) {
        Expr *target = expr->data.compound_assign.target;
        const char *op = expr->data.compound_assign.operator;
        
        /* Apply operation via binary evaluator */
        Expr bin_expr;
        bin_expr.kind = EXPR_BINARY;
        
        bin_expr.data.binary.left = target;
        bin_expr.data.binary.right = expr->data.compound_assign.value;
        
        if (strcmp(op, "+=") == 0) bin_expr.data.binary.operator = "+";
        else if (strcmp(op, "-=") == 0) bin_expr.data.binary.operator = "-";
        else if (strcmp(op, "*=") == 0) bin_expr.data.binary.operator = "*";
        else if (strcmp(op, "/=") == 0) bin_expr.data.binary.operator = "/";
        else return make_null();
        
        Value result = evaluate_binary(env, &bin_expr);
        
        /* Store result */
        if (target->kind == EXPR_IDENTIFIER) {
            set_variable(env, target->data.identifier.name, result);
        }
        
        return result;
    }
    
    return make_null();
}

/* ============== New expression evaluation ============== */

Value evaluate_new(Environment *env, Expr *expr) {
    const char *class_name = expr->data.new_expr.class_name;
    int arg_count = expr->data.new_expr.arg_count;
    
    /* Look up class declaration */
    Decl *class_decl = get_declaration(env, class_name);
    if (!class_decl || class_decl->kind != DECL_CLASS) {
        fprintf(stderr, "Unknown class: %s\n", class_name);
        return make_null();
    }
    
    /* Create instance - start with 0 fields, they grow dynamically via instance_set_field */
    ObjInstance *inst = new_instance(class_name,
                                     class_decl->data.class_decl.base_class,
                                     class_decl->data.class_decl.field_count);
    
    /* Copy methods from class declaration */
    inst->method_count = class_decl->data.class_decl.method_count;
    if (inst->method_count > 0) {
        inst->methods = (ObjFunction **)malloc(inst->method_count * sizeof(ObjFunction *));
        for (int i = 0; i < inst->method_count; i++) {
            Decl *method = class_decl->data.class_decl.methods[i];
            ObjFunction *func = new_function(method->data.function.name);
            func->param_count = method->data.function.param_count;
            func->params = method->data.function.params;
            func->body_count = method->data.function.body_count;
            func->body = method->data.function.body;
            inst->methods[i] = func;
        }
    }
    
    /* Initialize fields to default values */
    for (int i = 0; i < inst->field_count; i++) {
        inst->fields[i] = make_null();
    }
    
    /* Call constructor (_init) if it exists */
    for (int i = 0; i < inst->method_count; i++) {
        if (strcmp(inst->methods[i]->name, "_init") == 0) {
            /* Evaluate arguments */
            Value *args = (Value *)malloc(arg_count * sizeof(Value));
            for (int j = 0; j < arg_count; j++) {
                args[j] = evaluate_expr(env, expr->data.new_expr.arguments[j]);
            }
            
            /* Create environment with self */
            Environment *ctor_env = new_environment(env);
            Value self = make_obj((Object *)inst);
            define_variable(ctor_env, "self", self, NULL, true);
            
            /* Bind constructor arguments */
            for (int j = 0; j < inst->methods[i]->param_count && j < arg_count; j++) {
                define_variable(ctor_env, inst->methods[i]->params[j].name, args[j],
                             inst->methods[i]->params[j].param_type, false);
            }
            
            /* Execute constructor */
            ControlFlow flow = execute_block(ctor_env, inst->methods[i]->body, 
                                           inst->methods[i]->body_count);
            
            free(args);
            free_environment(ctor_env);
            (void)flow;
            break;
        }
    }
    
    return make_obj((Object *)inst);
}

/* ============== Helper functions ============== */

static Value call_function(Environment *env, ObjFunction *func, Value *args, int arg_count) {
    if (func->is_native) {
        return func->native_fn(env, args, arg_count);
    }
    
    /* Create function environment */
    Environment *func_env = new_environment(func->closure ? func->closure : env);
    
    /* Bind parameters */
    for (int i = 0; i < func->param_count && i < arg_count; i++) {
        define_variable(func_env, func->params[i].name, args[i], 
                       func->params[i].param_type, false);
    }
    
    /* Execute function body */
    ControlFlow flow = execute_block(func_env, func->body, func->body_count);
    
    Value result = make_null();
    if (flow.is_return) {
        result = flow.return_value;
    }
    
    free_environment(func_env);
    return result;
}

/* ============== Native function implementations ============== */

Value native_print(Environment *env, Value *args, int arg_count) {
    (void)env;
    for (int i = 0; i < arg_count; i++) {
        char *str = value_to_string(args[i]);
        printf("%s", str);
        free(str);
        if (i < arg_count - 1) {
            printf(" ");
        }
    }
    printf("\n");
    return make_null();
}

Value native_input(Environment *env, Value *args, int arg_count) {
    (void)env;
    (void)args;
    
    if (arg_count > 0) {
        char *prompt = value_to_string(args[0]);
        printf("%s", prompt);
        free(prompt);
    }
    
    char buffer[1024];
    if (fgets(buffer, sizeof(buffer), stdin) != NULL) {
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
            len--;
        }
        ObjString *str = new_string(buffer, (int)len);
        return make_obj((Object *)str);
    }
    
    return make_null();
}

Value native_range(Environment *env, Value *args, int arg_count) {
    (void)env;
    
    int start = 0;
    int end = 0;
    
    if (arg_count == 1) {
        if (args[0].type == VAL_INT) {
            end = args[0].as.integer;
        }
    } else if (arg_count >= 2) {
        if (args[0].type == VAL_INT) {
            start = args[0].as.integer;
        }
        if (args[1].type == VAL_INT) {
            end = args[1].as.integer;
        }
    }
    
    /* Create a list with the range */
    ObjList *list = new_list(NULL);
    for (int i = start; i < end; i++) {
        list_add(list, make_int(i));
    }
    
    return make_obj((Object *)list);
}

Value native_str(Environment *env, Value *args, int arg_count) {
    (void)env;
    
    if (arg_count < 1) {
        ObjString *str = new_string("", 0);
        return make_obj((Object *)str);
    }
    
    char *s = value_to_string(args[0]);
    ObjString *str = new_string(s, (int)strlen(s));
    free(s);
    return make_obj((Object *)str);
}

Value native_int(Environment *env, Value *args, int arg_count) {
    (void)env;
    
    if (arg_count < 1) {
        return make_int(0);
    }
    
    switch (args[0].type) {
        case VAL_INT:
            return args[0];
        case VAL_LONG:
            return make_int((int32_t)args[0].as.long_val);
        case VAL_FLOAT:
            return make_int((int32_t)args[0].as.float_val);
        case VAL_DOUBLE:
            return make_int((int32_t)args[0].as.double_val);
        case VAL_BOOL:
            return make_int(args[0].as.boolean ? 1 : 0);
        case VAL_CHAR:
            return make_int((int32_t)args[0].as.character);
        case VAL_BYTE:
            return make_int((int32_t)args[0].as.byte_val);
        case VAL_OBJ: {
            if (args[0].as.obj->type == OBJ_STRING) {
                int val = atoi(((ObjString *)args[0].as.obj)->chars);
                return make_int(val);
            }
            break;
        }
        default:
            break;
    }
    
    return make_int(0);
}

Value native_float(Environment *env, Value *args, int arg_count) {
    (void)env;
    
    if (arg_count < 1) {
        return make_float(0.0f);
    }
    
    switch (args[0].type) {
        case VAL_INT:
            return make_float((float)args[0].as.integer);
        case VAL_LONG:
            return make_float((float)args[0].as.long_val);
        case VAL_FLOAT:
            return args[0];
        case VAL_DOUBLE:
            return make_float((float)args[0].as.double_val);
        case VAL_BOOL:
            return make_float(args[0].as.boolean ? 1.0f : 0.0f);
        case VAL_OBJ: {
            if (args[0].as.obj->type == OBJ_STRING) {
                float val = (float)atof(((ObjString *)args[0].as.obj)->chars);
                return make_float(val);
            }
            break;
        }
        default:
            break;
    }
    
    return make_float(0.0f);
}

Value native_len(Environment *env, Value *args, int arg_count) {
    (void)env;
    
    if (arg_count < 1) {
        return make_int(0);
    }
    
    if (args[0].type == VAL_OBJ) {
        switch (args[0].as.obj->type) {
            case OBJ_STRING: {
                ObjString *str = (ObjString *)args[0].as.obj;
                return make_int(str->length);
            }
            case OBJ_LIST: {
                ObjList *list = (ObjList *)args[0].as.obj;
                return make_int(list->count);
            }
            case OBJ_MAP: {
                ObjMap *map = (ObjMap *)args[0].as.obj;
                return make_int(map->entry_count);
            }
            default:
                break;
        }
    }
    
    return make_int(0);
}

Value native_type(Environment *env, Value *args, int arg_count) {
    (void)env;
    
    if (arg_count < 1) {
        ObjString *str = new_string("null", 4);
        return make_obj((Object *)str);
    }
    
    char *type_str = NULL;
    
    switch (args[0].type) {
        case VAL_NULL:
            type_str = "null";
            break;
        case VAL_BOOL:
            type_str = "bool";
            break;
        case VAL_INT:
            type_str = "int";
            break;
        case VAL_LONG:
            type_str = "long";
            break;
        case VAL_FLOAT:
            type_str = "float";
            break;
        case VAL_DOUBLE:
            type_str = "double";
            break;
        case VAL_CHAR:
            type_str = "char";
            break;
        case VAL_BYTE:
            type_str = "byte";
            break;
        case VAL_OBJ: {
            switch (args[0].as.obj->type) {
                case OBJ_STRING:
                    type_str = "string";
                    break;
                case OBJ_LIST:
                    type_str = "list";
                    break;
                case OBJ_MAP:
                    type_str = "map";
                    break;
                case OBJ_INSTANCE:
                    type_str = ((ObjInstance *)args[0].as.obj)->class_name;
                    break;
                case OBJ_FUNCTION:
                    type_str = "function";
                    break;
                default:
                    type_str = "object";
                    break;
            }
            break;
        }
        default:
            type_str = "unknown";
            break;
    }
    
    ObjString *str = new_string(type_str, (int)strlen(type_str));
    return make_obj((Object *)str);
}

void register_natives(Environment *env) {
    /* Native functions are registered as variables in the global environment */
    /* They will be looked up by name when called */
    
    // Helper macro to define native functions
    #define DEFINE_NATIVE(name_str, func_ptr) \
        do { \
            ObjFunction *func = new_native_function(name_str, func_ptr); \
            Value val = make_obj((Object *)func); \
            define_variable(env, name_str, val, NULL, true); \
        } while (0)
        
    DEFINE_NATIVE("print", native_print);
    DEFINE_NATIVE("input", native_input);
    DEFINE_NATIVE("range", native_range);
    DEFINE_NATIVE("str", native_str);
    DEFINE_NATIVE("int", native_int);
    DEFINE_NATIVE("float", native_float);
    DEFINE_NATIVE("len", native_len);
    DEFINE_NATIVE("type", native_type);
    
    #undef DEFINE_NATIVE
}

/* ============== Native Type Methods ============== */

Value native_list_add(Environment *env, Value *args, int arg_count) {
    Value obj = get_variable_value(env, "self");
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_LIST && arg_count >= 1) {
        list_add((ObjList *)obj.as.obj, args[0]);
    }
    return make_null();
}

Value native_list_insert(Environment *env, Value *args, int arg_count) {
    Value obj = get_variable_value(env, "self");
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_LIST && arg_count >= 2 && args[0].type == VAL_INT) {
        list_insert((ObjList *)obj.as.obj, args[0].as.integer, args[1]);
    }
    return make_null();
}

Value native_list_remove(Environment *env, Value *args, int arg_count) {
    Value obj = get_variable_value(env, "self");
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_LIST && arg_count >= 1 && args[0].type == VAL_INT) {
        return list_remove((ObjList *)obj.as.obj, args[0].as.integer);
    }
    return make_null();
}

Value native_list_pop(Environment *env, Value *args, int arg_count) {
    (void)args; (void)arg_count;
    Value obj = get_variable_value(env, "self");
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_LIST) {
        return list_pop((ObjList *)obj.as.obj);
    }
    return make_null();
}

Value native_list_clear(Environment *env, Value *args, int arg_count) {
    (void)args; (void)arg_count;
    Value obj = get_variable_value(env, "self");
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_LIST) {
        list_clear((ObjList *)obj.as.obj);
    }
    return make_null();
}

Value native_list_length(Environment *env, Value *args, int arg_count) {
    (void)args; (void)arg_count;
    Value obj = get_variable_value(env, "self");
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_LIST) {
        return make_int(list_length((ObjList *)obj.as.obj));
    }
    return make_int(0);
}

Value native_map_get(Environment *env, Value *args, int arg_count) {
    Value obj = get_variable_value(env, "self");
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_MAP && arg_count >= 1) {
        return map_get((ObjMap *)obj.as.obj, args[0]);
    }
    return make_null();
}

Value native_map_set(Environment *env, Value *args, int arg_count) {
    Value obj = get_variable_value(env, "self");
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_MAP && arg_count >= 2) {
        map_set((ObjMap *)obj.as.obj, args[0], args[1]);
    }
    return make_null();
}

Value native_map_has(Environment *env, Value *args, int arg_count) {
    Value obj = get_variable_value(env, "self");
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_MAP && arg_count >= 1) {
        return make_bool(map_has((ObjMap *)obj.as.obj, args[0]));
    }
    return make_bool(false);
}

Value native_map_remove(Environment *env, Value *args, int arg_count) {
    Value obj = get_variable_value(env, "self");
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_MAP && arg_count >= 1) {
        return map_remove((ObjMap *)obj.as.obj, args[0]);
    }
    return make_null();
}

Value native_map_clear(Environment *env, Value *args, int arg_count) {
    (void)args; (void)arg_count;
    Value obj = get_variable_value(env, "self");
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_MAP) {
        map_clear((ObjMap *)obj.as.obj);
    }
    return make_null();
}

Value native_map_length(Environment *env, Value *args, int arg_count) {
    (void)args; (void)arg_count;
    Value obj = get_variable_value(env, "self");
    if (obj.type == VAL_OBJ && obj.as.obj->type == OBJ_MAP) {
        return make_int(map_length((ObjMap *)obj.as.obj));
    }
    return make_int(0);
}

ObjFunction **global_list_methods = NULL;
int global_list_method_count = 0;
ObjFunction **global_map_methods = NULL;
int global_map_method_count = 0;

void init_type_methods(void) {
    global_list_method_count = 6;
    global_list_methods = (ObjFunction **)malloc(6 * sizeof(ObjFunction *));
    global_list_methods[0] = new_native_function("add", native_list_add);
    global_list_methods[1] = new_native_function("insert", native_list_insert);
    global_list_methods[2] = new_native_function("remove", native_list_remove);
    global_list_methods[3] = new_native_function("pop", native_list_pop);
    global_list_methods[4] = new_native_function("clear", native_list_clear);
    global_list_methods[5] = new_native_function("length", native_list_length);

    global_map_method_count = 6;
    global_map_methods = (ObjFunction **)malloc(6 * sizeof(ObjFunction *));
    global_map_methods[0] = new_native_function("get", native_map_get);
    global_map_methods[1] = new_native_function("set", native_map_set);
    global_map_methods[2] = new_native_function("has", native_map_has);
    global_map_methods[3] = new_native_function("remove", native_map_remove);
    global_map_methods[4] = new_native_function("clear", native_map_clear);
    global_map_methods[5] = new_native_function("length", native_map_length);
}

