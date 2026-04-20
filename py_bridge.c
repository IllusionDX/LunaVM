/* Python to C AST bridge.
 * Converts Python AST nodes to C AST nodes for interpretation.
 * This is used by the Python driver to run Luna programs.
 * 
 * To use this:
 * 1. Build as shared library: gcc -shared -fPIC -o luna_interpreter.so ...
 * 2. Python loads the library with ctypes
 * 3. Python serializes AST to JSON or calls C functions directly
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "eval.h"

/* ============== Helper functions ============== */

static char *py_str_to_c(PyObject *py_str) {
    if (!py_str) return NULL;
    const char *utf8 = PyUnicode_AsUTF8(py_str);
    if (!utf8) return NULL;
    return strdup(utf8);
}

static int py_bool_to_c(PyObject *py_bool) {
    return PyObject_IsTrue(py_bool);
}

static int py_int_to_c(PyObject *py_int) {
    return (int)PyLong_AsLong(py_int);
}

/* ============== Type conversion ============== */

static Type *convert_type(PyObject *py_type) {
    if (!py_type || py_type == Py_None) return NULL;
    
    PyObject *type_class = PyObject_GetAttrString(py_type, "__class__");
    PyObject *type_name = PyObject_GetAttrString(type_class, "__name__");
    const char *class_name = PyUnicode_AsUTF8(type_name);
    
    Type *type = (Type *)malloc(sizeof(Type));
    if (!type) return NULL;
    memset(type, 0, sizeof(Type));
    
    if (strcmp(class_name, "BaseType") == 0) {
        type->kind = TYPE_BASE;
        PyObject *name = PyObject_GetAttrString(py_type, "name");
        type->data.base.name = py_str_to_c(name);
        Py_XDECREF(name);
    } else if (strcmp(class_name, "ArrayType") == 0) {
        type->kind = TYPE_ARRAY;
        PyObject *elem_type = PyObject_GetAttrString(py_type, "element_type");
        PyObject *size = PyObject_GetAttrString(py_type, "size");
        
        type->data.array.element_type = convert_type(elem_type);
        type->data.array.size = py_int_to_c(size);
        type->data.array.has_size = (size != Py_None);
        
        Py_XDECREF(elem_type);
        Py_XDECREF(size);
    } else if (strcmp(class_name, "ListType") == 0) {
        type->kind = TYPE_LIST;
        PyObject *elem_type = PyObject_GetAttrString(py_type, "element_type");
        type->data.list.element_type = convert_type(elem_type);
        Py_XDECREF(elem_type);
    } else if (strcmp(class_name, "MapType") == 0) {
        type->kind = TYPE_MAP;
        PyObject *key_type = PyObject_GetAttrString(py_type, "key_type");
        PyObject *value_type = PyObject_GetAttrString(py_type, "value_type");
        
        type->data.map.key_type = convert_type(key_type);
        type->data.map.value_type = convert_type(value_type);
        
        Py_XDECREF(key_type);
        Py_XDECREF(value_type);
    } else if (strcmp(class_name, "GenericType") == 0) {
        type->kind = TYPE_GENERIC;
        PyObject *base = PyObject_GetAttrString(py_type, "base");
        PyObject *type_args = PyObject_GetAttrString(py_type, "type_args");
        
        type->data.generic.base = py_str_to_c(base);
        
        if (type_args && PyList_Check(type_args)) {
            int count = (int)PyList_Size(type_args);
            type->data.generic.type_arg_count = count;
            type->data.generic.type_args = (Type **)malloc(count * sizeof(Type *));
            
            for (int i = 0; i < count; i++) {
                PyObject *arg = PyList_GetItem(type_args, i);
                type->data.generic.type_args[i] = convert_type(arg);
            }
        }
        
        Py_XDECREF(base);
        Py_XDECREF(type_args);
    } else {
        /* Default to base type */
        type->kind = TYPE_BASE;
        type->data.base.name = strdup("unknown");
    }
    
    Py_XDECREF(type_class);
    Py_XDECREF(type_name);
    
    return type;
}

/* ============== Expression conversion ============== */

static FieldInit *convert_fields(PyObject *fields, int *count);
static MapEntry *convert_map_entries(PyObject *entries, int *count);

static Expr *convert_expr(PyObject *py_expr) {
    if (!py_expr || py_expr == Py_None) return NULL;
    
    PyObject *expr_class = PyObject_GetAttrString(py_expr, "__class__");
    PyObject *class_name = PyObject_GetAttrString(expr_class, "__name__");
    const char *name = PyUnicode_AsUTF8(class_name);
    
    Expr *expr = (Expr *)malloc(sizeof(Expr));
    if (!expr) return NULL;
    memset(expr, 0, sizeof(Expr));
    
    if (strcmp(name, "IntegerLiteral") == 0) {
        expr->kind = EXPR_INTEGER;
        PyObject *value = PyObject_GetAttrString(py_expr, "value");
        expr->data.integer.value = py_str_to_c(value);
        Py_XDECREF(value);
    } else if (strcmp(name, "FloatLiteral") == 0) {
        expr->kind = EXPR_FLOAT;
        PyObject *value = PyObject_GetAttrString(py_expr, "value");
        expr->data.float_lit.value = py_str_to_c(value);
        Py_XDECREF(value);
    } else if (strcmp(name, "StringLiteral") == 0) {
        expr->kind = EXPR_STRING;
        PyObject *value = PyObject_GetAttrString(py_expr, "value");
        expr->data.string.value = py_str_to_c(value);
        Py_XDECREF(value);
    } else if (strcmp(name, "CharLiteral") == 0) {
        expr->kind = EXPR_CHAR;
        PyObject *value = PyObject_GetAttrString(py_expr, "value");
        if (value && PyUnicode_Check(value) && PyUnicode_GetLength(value) > 0) {
            expr->data.char_lit.value = PyUnicode_AsUTF8(value)[0];
        }
        Py_XDECREF(value);
    } else if (strcmp(name, "BooleanLiteral") == 0) {
        expr->kind = EXPR_BOOL;
        PyObject *value = PyObject_GetAttrString(py_expr, "value");
        expr->data.boolean.value = py_bool_to_c(value);
        Py_XDECREF(value);
    } else if (strcmp(name, "NullLiteral") == 0) {
        expr->kind = EXPR_NULL;
    } else if (strcmp(name, "Identifier") == 0) {
        expr->kind = EXPR_IDENTIFIER;
        PyObject *name_attr = PyObject_GetAttrString(py_expr, "name");
        expr->data.identifier.name = py_str_to_c(name_attr);
        Py_XDECREF(name_attr);
    } else if (strcmp(name, "BinaryOp") == 0) {
        expr->kind = EXPR_BINARY;
        PyObject *left = PyObject_GetAttrString(py_expr, "left");
        PyObject *op = PyObject_GetAttrString(py_expr, "operator");
        PyObject *right = PyObject_GetAttrString(py_expr, "right");
        
        expr->data.binary.left = convert_expr(left);
        expr->data.binary.operator = py_str_to_c(op);
        expr->data.binary.right = convert_expr(right);
        
        Py_XDECREF(left);
        Py_XDECREF(op);
        Py_XDECREF(right);
    } else if (strcmp(name, "UnaryOp") == 0) {
        expr->kind = EXPR_UNARY;
        PyObject *op = PyObject_GetAttrString(py_expr, "operator");
        PyObject *operand = PyObject_GetAttrString(py_expr, "operand");
        
        expr->data.unary.operator = py_str_to_c(op);
        expr->data.unary.operand = convert_expr(operand);
        
        Py_XDECREF(op);
        Py_XDECREF(operand);
    } else if (strcmp(name, "Call") == 0) {
        expr->kind = EXPR_CALL;
        PyObject *callee = PyObject_GetAttrString(py_expr, "callee");
        PyObject *arguments = PyObject_GetAttrString(py_expr, "arguments");
        
        expr->data.call.callee = convert_expr(callee);
        
        if (arguments && PyList_Check(arguments)) {
            int count = (int)PyList_Size(arguments);
            expr->data.call.arg_count = count;
            expr->data.call.arguments = (Expr **)malloc(count * sizeof(Expr *));
            
            for (int i = 0; i < count; i++) {
                PyObject *arg = PyList_GetItem(arguments, i);
                expr->data.call.arguments[i] = convert_expr(arg);
            }
        }
        
        Py_XDECREF(callee);
        Py_XDECREF(arguments);
    } else if (strcmp(name, "FieldAccess") == 0) {
        expr->kind = EXPR_FIELD_ACCESS;
        PyObject *obj = PyObject_GetAttrString(py_expr, "obj");
        PyObject *field = PyObject_GetAttrString(py_expr, "field");
        
        expr->data.field_access.obj = convert_expr(obj);
        expr->data.field_access.field = py_str_to_c(field);
        
        Py_XDECREF(obj);
        Py_XDECREF(field);
    } else if (strcmp(name, "IndexAccess") == 0) {
        expr->kind = EXPR_INDEX_ACCESS;
        PyObject *obj = PyObject_GetAttrString(py_expr, "obj");
        PyObject *index = PyObject_GetAttrString(py_expr, "index");
        
        expr->data.index_access.obj = convert_expr(obj);
        expr->data.index_access.index = convert_expr(index);
        
        Py_XDECREF(obj);
        Py_XDECREF(index);
    } else if (strcmp(name, "Assignment") == 0) {
        expr->kind = EXPR_ASSIGNMENT;
        PyObject *target = PyObject_GetAttrString(py_expr, "target");
        PyObject *value = PyObject_GetAttrString(py_expr, "value");
        
        expr->data.assignment.target = convert_expr(target);
        expr->data.assignment.value = convert_expr(value);
        
        Py_XDECREF(target);
        Py_XDECREF(value);
    } else if (strcmp(name, "CompoundAssignment") == 0) {
        expr->kind = EXPR_COMPOUND_ASSIGN;
        PyObject *target = PyObject_GetAttrString(py_expr, "target");
        PyObject *op = PyObject_GetAttrString(py_expr, "operator");
        PyObject *value = PyObject_GetAttrString(py_expr, "value");
        
        expr->data.compound_assign.target = convert_expr(target);
        expr->data.compound_assign.operator = py_str_to_c(op);
        expr->data.compound_assign.value = convert_expr(value);
        
        Py_XDECREF(target);
        Py_XDECREF(op);
        Py_XDECREF(value);
    } else if (strcmp(name, "TernaryOp") == 0) {
        expr->kind = EXPR_TERNARY;
        PyObject *condition = PyObject_GetAttrString(py_expr, "condition");
        PyObject *then_expr = PyObject_GetAttrString(py_expr, "then_expr");
        PyObject *else_expr = PyObject_GetAttrString(py_expr, "else_expr");
        
        expr->data.ternary.condition = convert_expr(condition);
        expr->data.ternary.then_expr = convert_expr(then_expr);
        expr->data.ternary.else_expr = convert_expr(else_expr);
        
        Py_XDECREF(condition);
        Py_XDECREF(then_expr);
        Py_XDECREF(else_expr);
    } else if (strcmp(name, "StructLiteral") == 0) {
        expr->kind = EXPR_STRUCT_LITERAL;
        PyObject *struct_name = PyObject_GetAttrString(py_expr, "struct_name");
        PyObject *fields = PyObject_GetAttrString(py_expr, "fields");
        
        expr->data.struct_literal.struct_name = py_str_to_c(struct_name);
        expr->data.struct_literal.fields = convert_fields(fields, 
            &expr->data.struct_literal.field_count);
        
        Py_XDECREF(struct_name);
        Py_XDECREF(fields);
    } else if (strcmp(name, "ArrayLiteral") == 0) {
        expr->kind = EXPR_ARRAY_LITERAL;
        PyObject *elements = PyObject_GetAttrString(py_expr, "elements");
        
        if (elements && PyList_Check(elements)) {
            int count = (int)PyList_Size(elements);
            expr->data.array_literal.element_count = count;
            expr->data.array_literal.elements = (Expr **)malloc(count * sizeof(Expr *));
            
            for (int i = 0; i < count; i++) {
                PyObject *elem = PyList_GetItem(elements, i);
                expr->data.array_literal.elements[i] = convert_expr(elem);
            }
        }
        
        Py_XDECREF(elements);
    } else if (strcmp(name, "ListLiteral") == 0) {
        expr->kind = EXPR_LIST_LITERAL;
        PyObject *elements = PyObject_GetAttrString(py_expr, "elements");
        
        if (elements && PyList_Check(elements)) {
            int count = (int)PyList_Size(elements);
            expr->data.list_literal.element_count = count;
            expr->data.list_literal.elements = (Expr **)malloc(count * sizeof(Expr *));
            
            for (int i = 0; i < count; i++) {
                PyObject *elem = PyList_GetItem(elements, i);
                expr->data.list_literal.elements[i] = convert_expr(elem);
            }
        }
        
        Py_XDECREF(elements);
    } else if (strcmp(name, "MapLiteral") == 0) {
        expr->kind = EXPR_MAP_LITERAL;
        PyObject *entries = PyObject_GetAttrString(py_expr, "entries");
        
        expr->data.map_literal.entries = convert_map_entries(entries, 
            &expr->data.map_literal.entry_count);
        
        Py_XDECREF(entries);
    } else if (strcmp(name, "NewExpression") == 0) {
        expr->kind = EXPR_NEW;
        PyObject *class_name = PyObject_GetAttrString(py_expr, "class_name");
        PyObject *arguments = PyObject_GetAttrString(py_expr, "arguments");
        
        expr->data.new_expr.class_name = py_str_to_c(class_name);
        
        if (arguments && PyList_Check(arguments)) {
            int count = (int)PyList_Size(arguments);
            expr->data.new_expr.arg_count = count;
            expr->data.new_expr.arguments = (Expr **)malloc(count * sizeof(Expr *));
            
            for (int i = 0; i < count; i++) {
                PyObject *arg = PyList_GetItem(arguments, i);
                expr->data.new_expr.arguments[i] = convert_expr(arg);
            }
        }
        
        Py_XDECREF(class_name);
        Py_XDECREF(arguments);
    } else {
        /* Unknown expression type - default to null */
        expr->kind = EXPR_NULL;
    }
    
    Py_XDECREF(expr_class);
    Py_XDECREF(class_name);
    
    return expr;
}

static FieldInit *convert_fields(PyObject *fields, int *count) {
    *count = 0;
    if (!fields || !PyList_Check(fields)) return NULL;
    
    int n = (int)PyList_Size(fields);
    FieldInit *result = (FieldInit *)malloc(n * sizeof(FieldInit));
    if (!result) return NULL;
    
    for (int i = 0; i < n; i++) {
        PyObject *item = PyList_GetItem(fields, i);
        if (item && PyTuple_Check(item) && PyTuple_Size(item) == 2) {
            PyObject *name = PyTuple_GetItem(item, 0);
            PyObject *value = PyTuple_GetItem(item, 1);
            
            result[i].name = py_str_to_c(name);
            result[i].value = convert_expr(value);
        }
    }
    
    *count = n;
    return result;
}

static MapEntry *convert_map_entries(PyObject *entries, int *count) {
    *count = 0;
    if (!entries || !PyList_Check(entries)) return NULL;
    
    int n = (int)PyList_Size(entries);
    MapEntry *result = (MapEntry *)malloc(n * sizeof(MapEntry));
    if (!result) return NULL;
    
    for (int i = 0; i < n; i++) {
        PyObject *item = PyList_GetItem(entries, i);
        if (item && PyTuple_Check(item) && PyTuple_Size(item) == 2) {
            PyObject *key = PyTuple_GetItem(item, 0);
            PyObject *value = PyTuple_GetItem(item, 1);
            
            result[i].key = convert_expr(key);
            result[i].value = convert_expr(value);
        }
    }
    
    *count = n;
    return result;
}

/* ============== Statement conversion ============== */

static SwitchCase *convert_switch_cases(PyObject *cases, int *count);
static CatchClause *convert_catch_clauses(PyObject *clauses, int *count);

static Stmt *convert_stmt(PyObject *py_stmt) {
    if (!py_stmt || py_stmt == Py_None) return NULL;
    
    PyObject *stmt_class = PyObject_GetAttrString(py_stmt, "__class__");
    PyObject *class_name = PyObject_GetAttrString(stmt_class, "__name__");
    const char *name = PyUnicode_AsUTF8(class_name);
    
    Stmt *stmt = (Stmt *)malloc(sizeof(Stmt));
    if (!stmt) return NULL;
    memset(stmt, 0, sizeof(Stmt));
    
    if (strcmp(name, "ExpressionStatement") == 0) {
        stmt->kind = STMT_EXPRESSION;
        PyObject *expr = PyObject_GetAttrString(py_stmt, "expression");
        stmt->data.expression.expression = convert_expr(expr);
        Py_XDECREF(expr);
    } else if (strcmp(name, "VariableDeclaration") == 0) {
        stmt->kind = STMT_VAR_DECL;
        PyObject *is_const = PyObject_GetAttrString(py_stmt, "is_const");
        PyObject *var_type = PyObject_GetAttrString(py_stmt, "var_type");
        PyObject *var_name = PyObject_GetAttrString(py_stmt, "name");
        PyObject *initializer = PyObject_GetAttrString(py_stmt, "initializer");
        
        stmt->data.var_decl.is_const = py_bool_to_c(is_const);
        stmt->data.var_decl.var_type = convert_type(var_type);
        stmt->data.var_decl.name = py_str_to_c(var_name);
        stmt->data.var_decl.initializer = convert_expr(initializer);
        
        Py_XDECREF(is_const);
        Py_XDECREF(var_type);
        Py_XDECREF(var_name);
        Py_XDECREF(initializer);
    } else if (strcmp(name, "ReturnStatement") == 0) {
        stmt->kind = STMT_RETURN;
        PyObject *value = PyObject_GetAttrString(py_stmt, "value");
        stmt->data.return_stmt.value = convert_expr(value);
        Py_XDECREF(value);
    } else if (strcmp(name, "PassStatement") == 0) {
        stmt->kind = STMT_PASS;
    } else if (strcmp(name, "BreakStatement") == 0) {
        stmt->kind = STMT_BREAK;
    } else if (strcmp(name, "ContinueStatement") == 0) {
        stmt->kind = STMT_CONTINUE;
    } else if (strcmp(name, "IfStatement") == 0) {
        stmt->kind = STMT_IF;
        PyObject *condition = PyObject_GetAttrString(py_stmt, "condition");
        PyObject *then_body = PyObject_GetAttrString(py_stmt, "then_body");
        PyObject *else_body = PyObject_GetAttrString(py_stmt, "else_body");
        
        stmt->data.if_stmt.condition = convert_expr(condition);
        
        if (then_body && PyList_Check(then_body)) {
            int count = (int)PyList_Size(then_body);
            stmt->data.if_stmt.then_count = count;
            stmt->data.if_stmt.then_body = (Stmt **)malloc(count * sizeof(Stmt *));
            for (int i = 0; i < count; i++) {
                PyObject *s = PyList_GetItem(then_body, i);
                stmt->data.if_stmt.then_body[i] = convert_stmt(s);
            }
        }
        
        if (else_body && else_body != Py_None && PyList_Check(else_body)) {
            int count = (int)PyList_Size(else_body);
            stmt->data.if_stmt.else_count = count;
            stmt->data.if_stmt.else_body = (Stmt **)malloc(count * sizeof(Stmt *));
            for (int i = 0; i < count; i++) {
                PyObject *s = PyList_GetItem(else_body, i);
                stmt->data.if_stmt.else_body[i] = convert_stmt(s);
            }
        }
        
        Py_XDECREF(condition);
        Py_XDECREF(then_body);
        Py_XDECREF(else_body);
    } else if (strcmp(name, "WhileStatement") == 0) {
        stmt->kind = STMT_WHILE;
        PyObject *condition = PyObject_GetAttrString(py_stmt, "condition");
        PyObject *body = PyObject_GetAttrString(py_stmt, "body");
        
        stmt->data.while_stmt.condition = convert_expr(condition);
        
        if (body && PyList_Check(body)) {
            int count = (int)PyList_Size(body);
            stmt->data.while_stmt.body_count = count;
            stmt->data.while_stmt.body = (Stmt **)malloc(count * sizeof(Stmt *));
            for (int i = 0; i < count; i++) {
                PyObject *s = PyList_GetItem(body, i);
                stmt->data.while_stmt.body[i] = convert_stmt(s);
            }
        }
        
        Py_XDECREF(condition);
        Py_XDECREF(body);
    } else if (strcmp(name, "ForStatement") == 0) {
        stmt->kind = STMT_FOR;
        PyObject *variable = PyObject_GetAttrString(py_stmt, "variable");
        PyObject *iterable = PyObject_GetAttrString(py_stmt, "iterable");
        PyObject *body = PyObject_GetAttrString(py_stmt, "body");
        
        stmt->data.for_stmt.variable = py_str_to_c(variable);
        stmt->data.for_stmt.iterable = convert_expr(iterable);
        
        if (body && PyList_Check(body)) {
            int count = (int)PyList_Size(body);
            stmt->data.for_stmt.body_count = count;
            stmt->data.for_stmt.body = (Stmt **)malloc(count * sizeof(Stmt *));
            for (int i = 0; i < count; i++) {
                PyObject *s = PyList_GetItem(body, i);
                stmt->data.for_stmt.body[i] = convert_stmt(s);
            }
        }
        
        Py_XDECREF(variable);
        Py_XDECREF(iterable);
        Py_XDECREF(body);
    } else if (strcmp(name, "SwitchStatement") == 0) {
        stmt->kind = STMT_SWITCH;
        PyObject *expression = PyObject_GetAttrString(py_stmt, "expression");
        PyObject *cases = PyObject_GetAttrString(py_stmt, "cases");
        
        stmt->data.switch_stmt.expression = convert_expr(expression);
        stmt->data.switch_stmt.cases = convert_switch_cases(cases, 
            &stmt->data.switch_stmt.case_count);
        
        Py_XDECREF(expression);
        Py_XDECREF(cases);
    } else if (strcmp(name, "ThrowStatement") == 0) {
        stmt->kind = STMT_THROW;
        PyObject *expression = PyObject_GetAttrString(py_stmt, "expression");
        stmt->data.throw_stmt.expression = convert_expr(expression);
        Py_XDECREF(expression);
    } else if (strcmp(name, "TryStatement") == 0) {
        stmt->kind = STMT_TRY;
        PyObject *try_body = PyObject_GetAttrString(py_stmt, "try_body");
        PyObject *catch_clauses = PyObject_GetAttrString(py_stmt, "catch_clauses");
        PyObject *finally_body = PyObject_GetAttrString(py_stmt, "finally_body");
        
        if (try_body && PyList_Check(try_body)) {
            int count = (int)PyList_Size(try_body);
            stmt->data.try_stmt.try_count = count;
            stmt->data.try_stmt.try_body = (Stmt **)malloc(count * sizeof(Stmt *));
            for (int i = 0; i < count; i++) {
                PyObject *s = PyList_GetItem(try_body, i);
                stmt->data.try_stmt.try_body[i] = convert_stmt(s);
            }
        }
        
        stmt->data.try_stmt.catch_clauses = convert_catch_clauses(catch_clauses,
            &stmt->data.try_stmt.catch_count);
        
        if (finally_body && finally_body != Py_None && PyList_Check(finally_body)) {
            int count = (int)PyList_Size(finally_body);
            stmt->data.try_stmt.finally_count = count;
            stmt->data.try_stmt.finally_body = (Stmt **)malloc(count * sizeof(Stmt *));
            for (int i = 0; i < count; i++) {
                PyObject *s = PyList_GetItem(finally_body, i);
                stmt->data.try_stmt.finally_body[i] = convert_stmt(s);
            }
        }
        
        Py_XDECREF(try_body);
        Py_XDECREF(catch_clauses);
        Py_XDECREF(finally_body);
    } else {
        /* Unknown statement - treat as pass */
        stmt->kind = STMT_PASS;
    }
    
    Py_XDECREF(stmt_class);
    Py_XDECREF(class_name);
    
    return stmt;
}

static SwitchCase *convert_switch_cases(PyObject *cases, int *count) {
    *count = 0;
    if (!cases || !PyList_Check(cases)) return NULL;
    
    int n = (int)PyList_Size(cases);
    SwitchCase *result = (SwitchCase *)malloc(n * sizeof(SwitchCase));
    if (!result) return NULL;
    memset(result, 0, n * sizeof(SwitchCase));
    
    for (int i = 0; i < n; i++) {
        PyObject *case_obj = PyList_GetItem(cases, i);
        if (!case_obj) continue;
        
        PyObject *value = PyObject_GetAttrString(case_obj, "value");
        PyObject *body = PyObject_GetAttrString(case_obj, "body");
        
        if (value && value != Py_None) {
            result[i].value = convert_expr(value);
        } else {
            result[i].value = NULL; /* Default case */
        }
        
        if (body && PyList_Check(body)) {
            int body_count = (int)PyList_Size(body);
            result[i].body_count = body_count;
            result[i].body = (Stmt **)malloc(body_count * sizeof(Stmt *));
            for (int j = 0; j < body_count; j++) {
                PyObject *s = PyList_GetItem(body, j);
                result[i].body[j] = convert_stmt(s);
            }
        }
        
        Py_XDECREF(value);
        Py_XDECREF(body);
    }
    
    *count = n;
    return result;
}

static CatchClause *convert_catch_clauses(PyObject *clauses, int *count) {
    *count = 0;
    if (!clauses || !PyList_Check(clauses)) return NULL;
    
    int n = (int)PyList_Size(clauses);
    CatchClause *result = (CatchClause *)malloc(n * sizeof(CatchClause));
    if (!result) return NULL;
    memset(result, 0, n * sizeof(CatchClause));
    
    for (int i = 0; i < n; i++) {
        PyObject *clause = PyList_GetItem(clauses, i);
        if (!clause) continue;
        
        PyObject *variable = PyObject_GetAttrString(clause, "variable");
        PyObject *body = PyObject_GetAttrString(clause, "body");
        
        result[i].variable = py_str_to_c(variable);
        
        if (body && PyList_Check(body)) {
            int body_count = (int)PyList_Size(body);
            result[i].body_count = body_count;
            result[i].body = (Stmt **)malloc(body_count * sizeof(Stmt *));
            for (int j = 0; j < body_count; j++) {
                PyObject *s = PyList_GetItem(body, j);
                result[i].body[j] = convert_stmt(s);
            }
        }
        
        Py_XDECREF(variable);
        Py_XDECREF(body);
    }
    
    *count = n;
    return result;
}

/* ============== Declaration conversion ============== */

static FunctionParam *convert_params(PyObject *params, int *count);
static StructField *convert_fields_decl(PyObject *fields, int *count);
static EnumVariant *convert_variants(PyObject *variants, int *count);

static Decl *convert_decl(PyObject *py_decl) {
    if (!py_decl || py_decl == Py_None) return NULL;
    
    PyObject *decl_class = PyObject_GetAttrString(py_decl, "__class__");
    PyObject *class_name = PyObject_GetAttrString(decl_class, "__name__");
    const char *name = PyUnicode_AsUTF8(class_name);
    
    Decl *decl = (Decl *)malloc(sizeof(Decl));
    if (!decl) return NULL;
    memset(decl, 0, sizeof(Decl));
    
    if (strcmp(name, "FunctionDeclaration") == 0) {
        decl->kind = DECL_FUNCTION;
        PyObject *func_name = PyObject_GetAttrString(py_decl, "name");
        PyObject *params = PyObject_GetAttrString(py_decl, "parameters");
        PyObject *return_type = PyObject_GetAttrString(py_decl, "return_type");
        PyObject *body = PyObject_GetAttrString(py_decl, "body");
        
        decl->data.function.name = py_str_to_c(func_name);
        decl->data.function.params = convert_params(params, &decl->data.function.param_count);
        decl->data.function.return_type = convert_type(return_type);
        
        if (body && PyList_Check(body)) {
            int count = (int)PyList_Size(body);
            decl->data.function.body_count = count;
            decl->data.function.body = (Stmt **)malloc(count * sizeof(Stmt *));
            for (int i = 0; i < count; i++) {
                PyObject *s = PyList_GetItem(body, i);
                decl->data.function.body[i] = convert_stmt(s);
            }
        }
        
        Py_XDECREF(func_name);
        Py_XDECREF(params);
        Py_XDECREF(return_type);
        Py_XDECREF(body);
    } else if (strcmp(name, "StructDeclaration") == 0) {
        decl->kind = DECL_STRUCT;
        PyObject *struct_name = PyObject_GetAttrString(py_decl, "name");
        PyObject *fields = PyObject_GetAttrString(py_decl, "fields");
        
        decl->data.struct_decl.name = py_str_to_c(struct_name);
        decl->data.struct_decl.fields = convert_fields_decl(fields, 
            &decl->data.struct_decl.field_count);
        
        Py_XDECREF(struct_name);
        Py_XDECREF(fields);
    } else if (strcmp(name, "ClassDeclaration") == 0) {
        decl->kind = DECL_CLASS;
        PyObject *class_name = PyObject_GetAttrString(py_decl, "name");
        PyObject *base_class = PyObject_GetAttrString(py_decl, "base_class");
        PyObject *fields = PyObject_GetAttrString(py_decl, "fields");
        PyObject *methods = PyObject_GetAttrString(py_decl, "methods");
        
        decl->data.class_decl.name = py_str_to_c(class_name);
        decl->data.class_decl.base_class = py_str_to_c(base_class);
        decl->data.class_decl.fields = convert_fields_decl(fields, 
            &decl->data.class_decl.field_count);
        
        if (methods && PyList_Check(methods)) {
            int count = (int)PyList_Size(methods);
            decl->data.class_decl.method_count = count;
            decl->data.class_decl.methods = (Decl **)malloc(count * sizeof(Decl *));
            for (int i = 0; i < count; i++) {
                PyObject *m = PyList_GetItem(methods, i);
                decl->data.class_decl.methods[i] = convert_decl(m);
            }
        }
        
        Py_XDECREF(class_name);
        Py_XDECREF(base_class);
        Py_XDECREF(fields);
        Py_XDECREF(methods);
    } else if (strcmp(name, "EnumDeclaration") == 0) {
        decl->kind = DECL_ENUM;
        PyObject *enum_name = PyObject_GetAttrString(py_decl, "name");
        PyObject *variants = PyObject_GetAttrString(py_decl, "variants");
        
        decl->data.enum_decl.name = py_str_to_c(enum_name);
        decl->data.enum_decl.variants = convert_variants(variants,
            &decl->data.enum_decl.variant_count);
        
        Py_XDECREF(enum_name);
        Py_XDECREF(variants);
    } else if (strcmp(name, "ImportDeclaration") == 0) {
        decl->kind = DECL_IMPORT;
        PyObject *module_name = PyObject_GetAttrString(py_decl, "module_name");
        PyObject *items = PyObject_GetAttrString(py_decl, "items");
        
        decl->data.import_decl.module_name = py_str_to_c(module_name);
        
        if (items && items != Py_None && PyList_Check(items)) {
            int count = (int)PyList_Size(items);
            decl->data.import_decl.item_count = count;
            decl->data.import_decl.import_all = false;
            decl->data.import_decl.items = (char **)malloc(count * sizeof(char *));
            for (int i = 0; i < count; i++) {
                PyObject *item = PyList_GetItem(items, i);
                decl->data.import_decl.items[i] = py_str_to_c(item);
            }
        } else if (items == Py_None) {
            decl->data.import_decl.import_all = true;
        }
        
        Py_XDECREF(module_name);
        Py_XDECREF(items);
    } else {
        /* Unknown declaration type */
        free(decl);
        Py_XDECREF(decl_class);
        Py_XDECREF(class_name);
        return NULL;
    }
    
    Py_XDECREF(decl_class);
    Py_XDECREF(class_name);
    
    return decl;
}

static FunctionParam *convert_params(PyObject *params, int *count) {
    *count = 0;
    if (!params || !PyList_Check(params)) return NULL;
    
    int n = (int)PyList_Size(params);
    FunctionParam *result = (FunctionParam *)malloc(n * sizeof(FunctionParam));
    if (!result) return NULL;
    
    for (int i = 0; i < n; i++) {
        PyObject *param = PyList_GetItem(params, i);
        if (!param) continue;
        
        PyObject *param_type = PyObject_GetAttrString(param, "param_type");
        PyObject *param_name = PyObject_GetAttrString(param, "name");
        
        result[i].param_type = convert_type(param_type);
        result[i].name = py_str_to_c(param_name);
        
        Py_XDECREF(param_type);
        Py_XDECREF(param_name);
    }
    
    *count = n;
    return result;
}

static StructField *convert_fields_decl(PyObject *fields, int *count) {
    *count = 0;
    if (!fields || !PyList_Check(fields)) return NULL;
    
    int n = (int)PyList_Size(fields);
    StructField *result = (StructField *)malloc(n * sizeof(StructField));
    if (!result) return NULL;
    
    for (int i = 0; i < n; i++) {
        PyObject *field = PyList_GetItem(fields, i);
        if (!field) continue;
        
        PyObject *field_type = PyObject_GetAttrString(field, "field_type");
        PyObject *field_name = PyObject_GetAttrString(field, "name");
        
        result[i].field_type = convert_type(field_type);
        result[i].name = py_str_to_c(field_name);
        
        Py_XDECREF(field_type);
        Py_XDECREF(field_name);
    }
    
    *count = n;
    return result;
}

static EnumVariant *convert_variants(PyObject *variants, int *count) {
    *count = 0;
    if (!variants || !PyList_Check(variants)) return NULL;
    
    int n = (int)PyList_Size(variants);
    EnumVariant *result = (EnumVariant *)malloc(n * sizeof(EnumVariant));
    if (!result) return NULL;
    memset(result, 0, n * sizeof(EnumVariant));
    
    for (int i = 0; i < n; i++) {
        PyObject *variant = PyList_GetItem(variants, i);
        if (!variant) continue;
        
        PyObject *name = PyObject_GetAttrString(variant, "name");
        PyObject *value = PyObject_GetAttrString(variant, "value");
        
        result[i].name = py_str_to_c(name);
        if (value && value != Py_None) {
            result[i].value = py_int_to_c(value);
            result[i].has_value = true;
        } else {
            result[i].value = i; /* Auto-increment */
            result[i].has_value = false;
        }
        
        Py_XDECREF(name);
        Py_XDECREF(value);
    }
    
    *count = n;
    return result;
}

/* ============== Program conversion ============== */

static Program *convert_program(PyObject *py_program) {
    if (!py_program || py_program == Py_None) return NULL;
    
    Program *program = (Program *)malloc(sizeof(Program));
    if (!program) return NULL;
    memset(program, 0, sizeof(Program));
    
    PyObject *declarations = PyObject_GetAttrString(py_program, "declarations");
    PyObject *statements = PyObject_GetAttrString(py_program, "statements");
    
    if (declarations && PyList_Check(declarations)) {
        int count = (int)PyList_Size(declarations);
        program->decl_count = count;
        program->declarations = (Decl **)malloc(count * sizeof(Decl *));
        for (int i = 0; i < count; i++) {
            PyObject *d = PyList_GetItem(declarations, i);
            program->declarations[i] = convert_decl(d);
        }
    }
    
    if (statements && PyList_Check(statements)) {
        int count = (int)PyList_Size(statements);
        program->stmt_count = count;
        program->statements = (Stmt **)malloc(count * sizeof(Stmt *));
        for (int i = 0; i < count; i++) {
            PyObject *s = PyList_GetItem(statements, i);
            program->statements[i] = convert_stmt(s);
        }
    }
    
    Py_XDECREF(declarations);
    Py_XDECREF(statements);
    
    return program;
}

/* ============== Python module methods ============== */

static PyObject *py_execute_program(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *py_program;
    
    if (!PyArg_ParseTuple(args, "O", &py_program)) {
        return NULL;
    }
    
    /* Convert Python AST to C AST */
    Program *program = convert_program(py_program);
    if (!program) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to convert program");
        return NULL;
    }
    
    /* Execute the program */
    execute_program(program);
    
    /* Clean up */
    free_program(program);
    
    Py_RETURN_NONE;
}

/* Method definitions */
static PyMethodDef LunaMethods[] = {
    {"execute_program", py_execute_program, METH_VARARGS,
     "Execute a Luna program from a Python AST"},
    {NULL, NULL, 0, NULL}
};

/* Module definition */
static struct PyModuleDef lunamodule = {
    PyModuleDef_HEAD_INIT,
    "luna_interpreter",
    "Luna programming language C interpreter",
    -1,
    LunaMethods
};

/* Module initialization */
PyMODINIT_FUNC PyInit_luna_interpreter(void) {
    return PyModule_Create(&lunamodule);
}
