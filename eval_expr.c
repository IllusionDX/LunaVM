/* Expression evaluator implementation.
 * Handles all expression types in the Luna language.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "eval.h"

/* ============== Forward declarations ============== */

static Value evaluate_literal(Expr *expr);
static Value evaluate_identifier(Environment *env, Expr *expr);

/* ============== Helper functions ============== */

double value_to_double(Value value) {
    switch (value.type) {
        case VAL_INT: return (double)value.as.integer;
        case VAL_LONG: return (double)value.as.long_val;
        case VAL_FLOAT: return (double)value.as.float_val;
        case VAL_DOUBLE: return value.as.double_val;
        case VAL_BOOL: return value.as.boolean ? 1.0 : 0.0;
        case VAL_CHAR: return (double)value.as.character;
        case VAL_BYTE: return (double)value.as.byte_val;
        default: return 0.0;
    }
}

int64_t value_to_int64(Value value) {
    switch (value.type) {
        case VAL_INT: return (int64_t)value.as.integer;
        case VAL_LONG: return value.as.long_val;
        case VAL_FLOAT: return (int64_t)value.as.float_val;
        case VAL_DOUBLE: return (int64_t)value.as.double_val;
        case VAL_BOOL: return value.as.boolean ? 1 : 0;
        case VAL_CHAR: return (int64_t)value.as.character;
        case VAL_BYTE: return (int64_t)value.as.byte_val;
        default: return 0;
    }
}

bool is_numeric(Value value) {
    return value.type == VAL_INT || value.type == VAL_LONG ||
           value.type == VAL_FLOAT || value.type == VAL_DOUBLE ||
           value.type == VAL_BYTE;
}

static bool is_integer(Value value) {
    return value.type == VAL_INT || value.type == VAL_LONG || value.type == VAL_BYTE;
}

/* ============== Main expression evaluator ============== */

Value evaluate_expr(Environment *env, Expr *expr) {
    if (!expr) return make_null();
    
    switch (expr->kind) {
        case EXPR_INTEGER:
        case EXPR_FLOAT:
        case EXPR_STRING:
        case EXPR_CHAR:
        case EXPR_BOOL:
        case EXPR_NULL:
            return evaluate_literal(expr);
            
        case EXPR_IDENTIFIER:
            return evaluate_identifier(env, expr);
            
        case EXPR_BINARY:
            return evaluate_binary(env, expr);
            
        case EXPR_UNARY:
            return evaluate_unary(env, expr);
            
        case EXPR_CALL:
            return evaluate_call(env, expr);
            
        case EXPR_FIELD_ACCESS:
            return evaluate_field_access(env, expr);
            
        case EXPR_INDEX_ACCESS:
            return evaluate_index_access(env, expr);
            
        case EXPR_ASSIGNMENT:
            return evaluate_assignment(env, expr);
            
        case EXPR_COMPOUND_ASSIGN:
            return evaluate_assignment(env, expr);
            
        case EXPR_TERNARY: {
            Value condition = evaluate_expr(env, expr->data.ternary.condition);
            if (is_truthy(condition)) {
                return evaluate_expr(env, expr->data.ternary.then_expr);
            } else {
                return evaluate_expr(env, expr->data.ternary.else_expr);
            }
        }
            
        case EXPR_STRUCT_LITERAL: {
            /* Create a struct instance */
            ObjStruct *strct = new_struct(expr->data.struct_literal.struct_name,
                                          expr->data.struct_literal.field_count);
            
            for (int i = 0; i < expr->data.struct_literal.field_count; i++) {
                FieldInit *field = &expr->data.struct_literal.fields[i];
                Value field_value = evaluate_expr(env, field->value);
                
                /* Find field index by name - simplified version */
                strct->fields[i] = field_value;
                if (field_value.type == VAL_OBJ) {
                    retain_obj(field_value.as.obj);
                }
            }
            
            return make_obj((Object *)strct);
        }
            
        case EXPR_ARRAY_LITERAL: {
            /* Create a fixed array - represented as a list for simplicity */
            ObjList *list = new_list(NULL);
            for (int i = 0; i < expr->data.array_literal.element_count; i++) {
                Value elem = evaluate_expr(env, expr->data.array_literal.elements[i]);
                list_add(list, elem);
            }
            return make_obj((Object *)list);
        }
            
        case EXPR_LIST_LITERAL: {
            ObjList *list = new_list(NULL);
            for (int i = 0; i < expr->data.list_literal.element_count; i++) {
                Value elem = evaluate_expr(env, expr->data.list_literal.elements[i]);
                list_add(list, elem);
            }
            return make_obj((Object *)list);
        }
            
        case EXPR_MAP_LITERAL: {
            ObjMap *map = new_map(NULL, NULL);
            for (int i = 0; i < expr->data.map_literal.entry_count; i++) {
                MapEntry *entry = &expr->data.map_literal.entries[i];
                Value key = evaluate_expr(env, entry->key);
                Value value = evaluate_expr(env, entry->value);
                map_set(map, key, value);
            }
            return make_obj((Object *)map);
        }
            
        case EXPR_NEW:
            return evaluate_new(env, expr);
            
        default:
            fprintf(stderr, "Unknown expression type: %d\n", expr->kind);
            return make_null();
    }
}

/* ============== Literal evaluation ============== */

static Value evaluate_literal(Expr *expr) {
    switch (expr->kind) {
        case EXPR_INTEGER:
            return make_int(atoi(expr->data.integer.value));
            
        case EXPR_FLOAT:
            return make_float((float)atof(expr->data.float_lit.value));
            
        case EXPR_STRING: {
            int len = (int)strlen(expr->data.string.value);
            ObjString *str = new_string(expr->data.string.value, len);
            return make_obj((Object *)str);
        }
            
        case EXPR_CHAR:
            return make_char(expr->data.char_lit.value);
            
        case EXPR_BOOL:
            return make_bool(expr->data.boolean.value);
            
        case EXPR_NULL:
            return make_null();
            
        default:
            return make_null();
    }
}

/* ============== Identifier evaluation ============== */

static Value evaluate_identifier(Environment *env, Expr *expr) {
    const char *name = expr->data.identifier.name;
    
    /* Check for special names */
    if (strcmp(name, "self") == 0) {
        /* self is handled specially in method calls */
        Variable *var = get_variable(env, "self");
        if (var) return var->value;
    }
    
    if (strcmp(name, "true") == 0) {
        return make_bool(true);
    }
    
    if (strcmp(name, "false") == 0) {
        return make_bool(false);
    }
    
    if (strcmp(name, "null") == 0) {
        return make_null();
    }
    
    /* Look up in environment */
    return get_variable_value(env, name);
}

/* ============== Binary operations ============== */

Value evaluate_binary(Environment *env, Expr *expr) {
    const char *op = expr->data.binary.operator;
    Value left = evaluate_expr(env, expr->data.binary.left);
    Value right = evaluate_expr(env, expr->data.binary.right);
    
    /* Arithmetic operators */
    if (strcmp(op, "+") == 0) {
        if (left.type == VAL_OBJ && left.as.obj->type == OBJ_STRING) {
            /* String concatenation */
            char *left_str = value_to_string(left);
            char *right_str = value_to_string(right);
            int len = (int)strlen(left_str) + (int)strlen(right_str);
            char *result = (char *)malloc(len + 1);
            strcpy(result, left_str);
            strcat(result, right_str);
            ObjString *str = new_string(result, len);
            free(left_str);
            free(right_str);
            free(result);
            return make_obj((Object *)str);
        }
        
        if (is_numeric(left) && is_numeric(right)) {
            if (left.type == VAL_DOUBLE || right.type == VAL_DOUBLE) {
                return make_double(value_to_double(left) + value_to_double(right));
            }
            if (left.type == VAL_FLOAT || right.type == VAL_FLOAT) {
                return make_float((float)(value_to_double(left) + value_to_double(right)));
            }
            if (left.type == VAL_LONG || right.type == VAL_LONG) {
                return make_long(value_to_int64(left) + value_to_int64(right));
            }
            return make_int(left.as.integer + right.as.integer);
        }
    }
    
    if (strcmp(op, "-") == 0) {
        if (is_numeric(left) && is_numeric(right)) {
            if (left.type == VAL_DOUBLE || right.type == VAL_DOUBLE) {
                return make_double(value_to_double(left) - value_to_double(right));
            }
            if (left.type == VAL_FLOAT || right.type == VAL_FLOAT) {
                return make_float((float)(value_to_double(left) - value_to_double(right)));
            }
            if (left.type == VAL_LONG || right.type == VAL_LONG) {
                return make_long(value_to_int64(left) - value_to_int64(right));
            }
            return make_int(left.as.integer - right.as.integer);
        }
    }
    
    if (strcmp(op, "*") == 0) {
        if (is_numeric(left) && is_numeric(right)) {
            if (left.type == VAL_DOUBLE || right.type == VAL_DOUBLE) {
                return make_double(value_to_double(left) * value_to_double(right));
            }
            if (left.type == VAL_FLOAT || right.type == VAL_FLOAT) {
                return make_float((float)(value_to_double(left) * value_to_double(right)));
            }
            if (left.type == VAL_LONG || right.type == VAL_LONG) {
                return make_long(value_to_int64(left) * value_to_int64(right));
            }
            return make_int(left.as.integer * right.as.integer);
        }
    }
    
    if (strcmp(op, "/") == 0) {
        if (is_numeric(left) && is_numeric(right)) {
            double divisor = value_to_double(right);
            if (divisor == 0.0) {
                fprintf(stderr, "Division by zero\n");
                return make_null();
            }
            if (left.type == VAL_DOUBLE || right.type == VAL_DOUBLE) {
                return make_double(value_to_double(left) / divisor);
            }
            if (left.type == VAL_FLOAT || right.type == VAL_FLOAT) {
                return make_float((float)(value_to_double(left) / divisor));
            }
            if (is_integer(left) && is_integer(right)) {
                return make_int((int32_t)(value_to_int64(left) / value_to_int64(right)));
            }
        }
    }
    
    if (strcmp(op, "%") == 0) {
        if (is_integer(left) && is_integer(right)) {
            int64_t divisor = value_to_int64(right);
            if (divisor == 0) {
                fprintf(stderr, "Modulo by zero\n");
                return make_null();
            }
            return make_int((int32_t)(value_to_int64(left) % divisor));
        }
    }
    
    /* Comparison operators */
    if (strcmp(op, "==") == 0) {
        return make_bool(values_equal(left, right));
    }
    
    if (strcmp(op, "!=") == 0) {
        return make_bool(!values_equal(left, right));
    }
    
    if (strcmp(op, "<") == 0) {
        if (is_numeric(left) && is_numeric(right)) {
            return make_bool(value_to_double(left) < value_to_double(right));
        }
        if (left.type == VAL_OBJ && left.as.obj->type == OBJ_STRING &&
            right.type == VAL_OBJ && right.as.obj->type == OBJ_STRING) {
            ObjString *ls = (ObjString *)left.as.obj;
            ObjString *rs = (ObjString *)right.as.obj;
            return make_bool(strcmp(ls->chars, rs->chars) < 0);
        }
    }
    
    if (strcmp(op, ">") == 0) {
        if (is_numeric(left) && is_numeric(right)) {
            return make_bool(value_to_double(left) > value_to_double(right));
        }
        if (left.type == VAL_OBJ && left.as.obj->type == OBJ_STRING &&
            right.type == VAL_OBJ && right.as.obj->type == OBJ_STRING) {
            ObjString *ls = (ObjString *)left.as.obj;
            ObjString *rs = (ObjString *)right.as.obj;
            return make_bool(strcmp(ls->chars, rs->chars) > 0);
        }
    }
    
    if (strcmp(op, "<=") == 0) {
        if (is_numeric(left) && is_numeric(right)) {
            return make_bool(value_to_double(left) <= value_to_double(right));
        }
    }
    
    if (strcmp(op, ">=") == 0) {
        if (is_numeric(left) && is_numeric(right)) {
            return make_bool(value_to_double(left) >= value_to_double(right));
        }
    }
    
    /* Logical operators */
    if (strcmp(op, "and") == 0) {
        return make_bool(is_truthy(left) && is_truthy(right));
    }
    
    if (strcmp(op, "or") == 0) {
        return make_bool(is_truthy(left) || is_truthy(right));
    }
    
    /* Bitwise operators */
    if (strcmp(op, "&") == 0) {
        if (is_integer(left) && is_integer(right)) {
            return make_int((int32_t)(value_to_int64(left) & value_to_int64(right)));
        }
    }
    
    if (strcmp(op, "|") == 0) {
        if (is_integer(left) && is_integer(right)) {
            return make_int((int32_t)(value_to_int64(left) | value_to_int64(right)));
        }
    }
    
    if (strcmp(op, "^") == 0) {
        if (is_integer(left) && is_integer(right)) {
            return make_int((int32_t)(value_to_int64(left) ^ value_to_int64(right)));
        }
    }
    
    if (strcmp(op, "<<") == 0) {
        if (is_integer(left) && is_integer(right)) {
            return make_int((int32_t)(value_to_int64(left) << value_to_int64(right)));
        }
    }
    
    if (strcmp(op, ">>") == 0) {
        if (is_integer(left) && is_integer(right)) {
            return make_int((int32_t)(value_to_int64(left) >> value_to_int64(right)));
        }
    }
    
    fprintf(stderr, "Unknown or invalid binary operator: %s\n", op);
    return make_null();
}

/* ============== Unary operations ============== */

Value evaluate_unary(Environment *env, Expr *expr) {
    const char *op = expr->data.unary.operator;
    Value operand = evaluate_expr(env, expr->data.unary.operand);
    
    if (strcmp(op, "-") == 0) {
        if (is_numeric(operand)) {
            switch (operand.type) {
                case VAL_INT:
                    return make_int(-operand.as.integer);
                case VAL_LONG:
                    return make_long(-operand.as.long_val);
                case VAL_FLOAT:
                    return make_float(-operand.as.float_val);
                case VAL_DOUBLE:
                    return make_double(-operand.as.double_val);
                case VAL_BYTE:
                    return make_int(-(int32_t)operand.as.byte_val);
                default:
                    break;
            }
        }
    }
    
    if (strcmp(op, "not") == 0) {
        return make_bool(!is_truthy(operand));
    }
    
    if (strcmp(op, "~") == 0) {
        if (is_integer(operand)) {
            return make_int(~(int32_t)value_to_int64(operand));
        }
    }
    
    fprintf(stderr, "Unknown or invalid unary operator: %s\n", op);
    return make_null();
}
