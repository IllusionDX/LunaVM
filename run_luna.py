#!/usr/bin/env python3
"""Driver script to run Luna programs using the C interpreter.

This script:
1. Uses the existing Python lexer/parser to generate AST
2. Serializes AST to JSON
3. Calls the C interpreter via ctypes or subprocess
"""

import sys
import os
import json
import subprocess
import tempfile
from pathlib import Path

# Add parent directory to import Luna modules
sys.path.insert(0, str(Path(__file__).parent.parent))

from lexer import Lexer, LexerError
from parser import Parser, ParseError, format_ast
from ast_nodes import *


def ast_to_dict(node):
    """Convert Python AST to a JSON-serializable dictionary."""
    if node is None:
        return None
    
    if isinstance(node, list):
        return [ast_to_dict(item) for item in node]
    
    if isinstance(node, tuple):
        return {"__tuple__": [ast_to_dict(item) for item in node]}
    
    if not isinstance(node, ASTNode):
        return node
    
    result = {"kind": node.__class__.__name__}
    
    # Handle each AST node type
    if isinstance(node, Program):
        result["declarations"] = ast_to_dict(node.declarations)
        result["statements"] = ast_to_dict(node.statements)
    
    elif isinstance(node, BaseType):
        result["kind"] = "base"
        result["name"] = node.name
    
    elif isinstance(node, ArrayType):
        result["kind"] = "array"
        result["element_type"] = ast_to_dict(node.element_type)
        result["size"] = node.size
    
    elif isinstance(node, ListType):
        result["kind"] = "list"
        result["element_type"] = ast_to_dict(node.element_type)
    
    elif isinstance(node, MapType):
        result["kind"] = "map"
        result["key_type"] = ast_to_dict(node.key_type)
        result["value_type"] = ast_to_dict(node.value_type)
    
    elif isinstance(node, GenericType):
        result["kind"] = "generic"
        result["base"] = node.base
        result["type_args"] = ast_to_dict(node.type_args)
    
    elif isinstance(node, IntegerLiteral):
        result["kind"] = "integer"
        result["value"] = node.value
    
    elif isinstance(node, FloatLiteral):
        result["kind"] = "float"
        result["value"] = node.value
    
    elif isinstance(node, StringLiteral):
        result["kind"] = "string"
        result["value"] = node.value
    
    elif isinstance(node, CharLiteral):
        result["kind"] = "char"
        result["value"] = node.value
    
    elif isinstance(node, BooleanLiteral):
        result["kind"] = "bool"
        result["value"] = node.value
    
    elif isinstance(node, NullLiteral):
        result["kind"] = "null"
    
    elif isinstance(node, Identifier):
        result["kind"] = "identifier"
        result["name"] = node.name
    
    elif isinstance(node, BinaryOp):
        result["kind"] = "binary"
        result["left"] = ast_to_dict(node.left)
        result["operator"] = node.operator
        result["right"] = ast_to_dict(node.right)
    
    elif isinstance(node, UnaryOp):
        result["kind"] = "unary"
        result["operator"] = node.operator
        result["operand"] = ast_to_dict(node.operand)
    
    elif isinstance(node, Call):
        result["kind"] = "call"
        result["callee"] = ast_to_dict(node.callee)
        result["arguments"] = ast_to_dict(node.arguments)
    
    elif isinstance(node, FieldAccess):
        result["kind"] = "field_access"
        result["obj"] = ast_to_dict(node.obj)
        result["field"] = node.field
    
    elif isinstance(node, IndexAccess):
        result["kind"] = "index_access"
        result["obj"] = ast_to_dict(node.obj)
        result["index"] = ast_to_dict(node.index)
    
    elif isinstance(node, Assignment):
        result["kind"] = "assignment"
        result["target"] = ast_to_dict(node.target)
        result["value"] = ast_to_dict(node.value)
    
    elif isinstance(node, CompoundAssignment):
        result["kind"] = "compound_assignment"
        result["target"] = ast_to_dict(node.target)
        result["operator"] = node.operator
        result["value"] = ast_to_dict(node.value)
    
    elif isinstance(node, TernaryOp):
        result["kind"] = "ternary"
        result["condition"] = ast_to_dict(node.condition)
        result["then_expr"] = ast_to_dict(node.then_expr)
        result["else_expr"] = ast_to_dict(node.else_expr)
    
    elif isinstance(node, StructLiteral):
        result["kind"] = "struct_literal"
        result["struct_name"] = node.struct_name
        result["fields"] = [
            {"name": name, "value": ast_to_dict(value)}
            for name, value in node.fields
        ]
    
    elif isinstance(node, ArrayLiteral):
        result["kind"] = "array_literal"
        result["elements"] = ast_to_dict(node.elements)
    
    elif isinstance(node, ListLiteral):
        result["kind"] = "list_literal"
        result["elements"] = ast_to_dict(node.elements)
    
    elif isinstance(node, MapLiteral):
        result["kind"] = "map_literal"
        result["entries"] = [
            {"key": ast_to_dict(key), "value": ast_to_dict(value)}
            for key, value in node.entries
        ]
    
    elif isinstance(node, NewExpression):
        result["kind"] = "new"
        result["class_name"] = node.class_name
        result["arguments"] = ast_to_dict(node.arguments)
    
    elif isinstance(node, ExpressionStatement):
        result["kind"] = "expression"
        result["expression"] = ast_to_dict(node.expression)
    
    elif isinstance(node, VariableDeclaration):
        result["kind"] = "var_decl"
        result["is_const"] = node.is_const
        result["var_type"] = ast_to_dict(node.var_type)
        result["name"] = node.name
        result["initializer"] = ast_to_dict(node.initializer)
    
    elif isinstance(node, ReturnStatement):
        result["kind"] = "return"
        result["value"] = ast_to_dict(node.value)
    
    elif isinstance(node, PassStatement):
        result["kind"] = "pass"
    
    elif isinstance(node, BreakStatement):
        result["kind"] = "break"
    
    elif isinstance(node, ContinueStatement):
        result["kind"] = "continue"
    
    elif isinstance(node, IfStatement):
        result["kind"] = "if"
        result["condition"] = ast_to_dict(node.condition)
        result["then_body"] = ast_to_dict(node.then_body)
        result["else_body"] = ast_to_dict(node.else_body)
    
    elif isinstance(node, WhileStatement):
        result["kind"] = "while"
        result["condition"] = ast_to_dict(node.condition)
        result["body"] = ast_to_dict(node.body)
    
    elif isinstance(node, ForStatement):
        result["kind"] = "for"
        result["variable"] = node.variable
        result["iterable"] = ast_to_dict(node.iterable)
        result["body"] = ast_to_dict(node.body)
    
    elif isinstance(node, SwitchCase):
        result["kind"] = "case"
        result["value"] = ast_to_dict(node.value)
        result["body"] = ast_to_dict(node.body)
    
    elif isinstance(node, SwitchStatement):
        result["kind"] = "switch"
        result["expression"] = ast_to_dict(node.expression)
        result["cases"] = ast_to_dict(node.cases)
    
    elif isinstance(node, ThrowStatement):
        result["kind"] = "throw"
        result["expression"] = ast_to_dict(node.expression)
    
    elif isinstance(node, CatchClause):
        result["kind"] = "catch"
        result["variable"] = node.variable
        result["body"] = ast_to_dict(node.body)
    
    elif isinstance(node, TryStatement):
        result["kind"] = "try"
        result["try_body"] = ast_to_dict(node.try_body)
        result["catch_clauses"] = ast_to_dict(node.catch_clauses)
        result["finally_body"] = ast_to_dict(node.finally_body)
    
    elif isinstance(node, FunctionParameter):
        result["kind"] = "param"
        result["param_type"] = ast_to_dict(node.param_type)
        result["name"] = node.name
    
    elif isinstance(node, FunctionDeclaration):
        result["kind"] = "function"
        result["name"] = node.name
        result["parameters"] = ast_to_dict(node.parameters)
        result["return_type"] = ast_to_dict(node.return_type)
        result["body"] = ast_to_dict(node.body)
    
    elif isinstance(node, StructField):
        result["kind"] = "field"
        result["field_type"] = ast_to_dict(node.field_type)
        result["name"] = node.name
    
    elif isinstance(node, StructDeclaration):
        result["kind"] = "struct"
        result["name"] = node.name
        result["fields"] = ast_to_dict(node.fields)
    
    elif isinstance(node, ClassDeclaration):
        result["kind"] = "class"
        result["name"] = node.name
        result["base_class"] = node.base_class
        result["fields"] = ast_to_dict(node.fields)
        result["methods"] = ast_to_dict(node.methods)
    
    elif isinstance(node, EnumVariant):
        result["kind"] = "variant"
        result["name"] = node.name
        result["value"] = node.value
    
    elif isinstance(node, EnumDeclaration):
        result["kind"] = "enum"
        result["name"] = node.name
        result["variants"] = ast_to_dict(node.variants)
    
    elif isinstance(node, ImportDeclaration):
        result["kind"] = "import"
        result["module_name"] = node.module_name
        result["items"] = node.items
    
    return result


def compile_and_run_c_interpreter(json_ast: str, interpreter_path: str = None):
    """Compile and run the C interpreter with the JSON AST."""
    
    # Find the interpreter executable
    if interpreter_path is None:
        interpreter_dir = Path(__file__).parent
        # Use .exe extension on Windows
        exe_suffix = ".exe" if os.name == "nt" else ""
        interpreter_path = interpreter_dir / f"luna{exe_suffix}"
        
        # Compile if not exists
        if not interpreter_path.exists():
            print("C interpreter not found. Building...")
            makefile_path = interpreter_dir / "Makefile"
            
            if makefile_path.exists():
                result = subprocess.run(
                    ["make", "-C", str(interpreter_dir)],
                    capture_output=True,
                    text=True
                )
                if result.returncode != 0:
                    print(f"Build failed:\n{result.stderr}")
                    return 1
            else:
                print("Error: Makefile not found. Cannot build interpreter.")
                return 1
    
    # Write JSON to temp file
    with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
        f.write(json_ast)
        json_file = f.name
    
    try:
        # Run interpreter
        result = subprocess.run(
            [str(interpreter_path), "--json", json_file],
            capture_output=True,
            text=True
        )
        
        if result.stdout:
            print(result.stdout, end='')
        if result.stderr:
            print(result.stderr, end='', file=sys.stderr)
        
        return result.returncode
    finally:
        os.unlink(json_file)


def run_luna_file(source_file: str):
    """Run a Luna source file."""
    
    try:
        # Read source
        with open(source_file, 'r') as f:
            source = f.read()
        
        # Tokenize
        print(f"Lexing {source_file}...")
        lexer = Lexer(source, filename=source_file)
        tokens = lexer.tokenize()
        
        # Parse
        print(f"Parsing {source_file}...")
        parser = Parser(tokens, filename=source_file)
        ast = parser.parse()
        
        # Convert to JSON
        print("Converting AST to JSON...")
        ast_dict = ast_to_dict(ast)
        json_ast = json.dumps(ast_dict, indent=2)
        
        # Save JSON for debugging
        json_file = source_file + ".json"
        with open(json_file, 'w') as f:
            f.write(json_ast)
        print(f"AST saved to {json_file}")
        
        # Run interpreter
        print("\nExecuting program...")
        print("=" * 50)
        return compile_and_run_c_interpreter(json_ast)
        
    except LexerError as e:
        print(f"Lexer error: {e}", file=sys.stderr)
        return 1
    except ParseError as e:
        print(f"Parse error: {e}", file=sys.stderr)
        return 1
    except FileNotFoundError as e:
        print(f"File not found: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1


def main():
    if len(sys.argv) < 2:
        print("Usage: python run_luna.py <source_file.luna>")
        print("       python run_luna.py --debug <source_file.luna>")
        sys.exit(1)
    
    debug_mode = False
    source_file = None
    
    for arg in sys.argv[1:]:
        if arg == "--debug":
            debug_mode = True
        elif not arg.startswith("--"):
            source_file = arg
    
    if source_file is None:
        print("Error: No source file specified")
        sys.exit(1)
    
    if debug_mode:
        print(f"Running in debug mode")
    
    sys.exit(run_luna_file(source_file))


if __name__ == "__main__":
    main()
