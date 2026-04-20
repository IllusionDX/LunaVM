/* Expression evaluator and statement executor.
 * Tree-walking interpreter implementation.
 */

#ifndef LUNA_EVAL_H
#define LUNA_EVAL_H

#include "environment.h"

/* Exception control flow */
typedef struct {
    bool has_exception;
    Value exception;
    bool is_break;
    bool is_continue;
    bool is_return;
    Value return_value;
} ControlFlow;

/* ============== Expression evaluation ============== */

Value evaluate_expr(Environment *env, Expr *expr);
Value evaluate_binary(Environment *env, Expr *expr);
Value evaluate_unary(Environment *env, Expr *expr);
Value evaluate_call(Environment *env, Expr *expr);
Value evaluate_field_access(Environment *env, Expr *expr);
Value evaluate_index_access(Environment *env, Expr *expr);
Value evaluate_assignment(Environment *env, Expr *expr);
Value evaluate_new(Environment *env, Expr *expr);

/* ============== Statement execution ============== */

ControlFlow execute_stmt(Environment *env, Stmt *stmt);
ControlFlow execute_block(Environment *env, Stmt **stmts, int count);
ControlFlow execute_if(Environment *env, Stmt *stmt);
ControlFlow execute_while(Environment *env, Stmt *stmt);
ControlFlow execute_for(Environment *env, Stmt *stmt);
ControlFlow execute_switch(Environment *env, Stmt *stmt);
ControlFlow execute_try(Environment *env, Stmt *stmt);

/* ============== Declaration execution ============== */

void execute_decl(Environment *env, Decl *decl);
void execute_function_decl(Environment *env, Decl *decl);
void execute_struct_decl(Environment *env, Decl *decl);
void execute_class_decl(Environment *env, Decl *decl);
void execute_enum_decl(Environment *env, Decl *decl);
void execute_import_decl(Environment *env, Decl *decl);

/* ============== Program execution ============== */

void execute_program(Program *program);

/* ============== Native functions ============== */

void register_natives(Environment *env);
void init_type_methods(void);
Value native_print(Environment *env, Value *args, int arg_count);
Value native_input(Environment *env, Value *args, int arg_count);
Value native_range(Environment *env, Value *args, int arg_count);
Value native_str(Environment *env, Value *args, int arg_count);
Value native_int(Environment *env, Value *args, int arg_count);
Value native_float(Environment *env, Value *args, int arg_count);
Value native_len(Environment *env, Value *args, int arg_count);
Value native_type(Environment *env, Value *args, int arg_count);

/* ============== Control flow helpers ============== */

ControlFlow make_control_flow(void);
ControlFlow exception(Value value);
ControlFlow break_flow(void);
ControlFlow continue_flow(void);
ControlFlow return_flow(Value value);

/* ============== Type checking helpers ============== */

bool check_type(Value value, Type *expected);
bool can_coerce(Type *from, Type *to);
Type *infer_type(Expr *expr);
Type *unify_types(Type *a, Type *b);

/* ============== Numeric conversion helpers ============== */

bool is_numeric(Value value);
double value_to_double(Value value);
int64_t value_to_int64(Value value);

#endif /* LUNA_EVAL_H */
