#include "parser/printer.h"
#include "parser/ast.h"
#include "util/strbuf.h"
#include "util/arena.h"
#include "lexer/lexer.h"   /* Iron_TokenKind for operator names */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Printer context ─────────────────────────────────────────────────────── */

typedef struct {
    Iron_StrBuf *sb;
    int          indent_level;
} PrintCtx;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void print_indent(PrintCtx *ctx) {
    for (int i = 0; i < ctx->indent_level; i++) {
        iron_strbuf_appendf(ctx->sb, "  ");
    }
}

/* Operator token kind → string */
static const char *op_str(Iron_OpKind op) {
    switch ((Iron_TokenKind)op) {
        case IRON_TOK_PLUS:          return "+";
        case IRON_TOK_MINUS:         return "-";
        case IRON_TOK_STAR:          return "*";
        case IRON_TOK_SLASH:         return "/";
        case IRON_TOK_PERCENT:       return "%";
        case IRON_TOK_EQUALS:        return "==";
        case IRON_TOK_NOT_EQUALS:    return "!=";
        case IRON_TOK_LESS:          return "<";
        case IRON_TOK_GREATER:       return ">";
        case IRON_TOK_LESS_EQ:       return "<=";
        case IRON_TOK_GREATER_EQ:    return ">=";
        case IRON_TOK_AND:           return "and";
        case IRON_TOK_OR:            return "or";
        case IRON_TOK_ASSIGN:        return "=";
        case IRON_TOK_PLUS_ASSIGN:   return "+=";
        case IRON_TOK_MINUS_ASSIGN:  return "-=";
        case IRON_TOK_STAR_ASSIGN:   return "*=";
        case IRON_TOK_SLASH_ASSIGN:  return "/=";
        default:                     return "?";
    }
}

/* Forward-declare so print_node can be called recursively */
static void print_node(PrintCtx *ctx, Iron_Node *node);

/* Print a type annotation */
static void print_type_ann(PrintCtx *ctx, Iron_Node *node) {
    if (!node) return;
    if (node->kind == IRON_NODE_TYPE_ANNOTATION) {
        Iron_TypeAnnotation *t = (Iron_TypeAnnotation *)node;
        if (t->is_array) {
            iron_strbuf_appendf(ctx->sb, "[%s", t->name);
            if (t->array_size) {
                iron_strbuf_appendf(ctx->sb, "; ");
                print_node(ctx, t->array_size);
            }
            iron_strbuf_appendf(ctx->sb, "]");
        } else {
            iron_strbuf_appendf(ctx->sb, "%s", t->name);
            if (t->is_nullable) {
                iron_strbuf_appendf(ctx->sb, "?");
            }
            if (t->generic_arg_count > 0) {
                iron_strbuf_appendf(ctx->sb, "[");
                for (int i = 0; i < t->generic_arg_count; i++) {
                    if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
                    print_type_ann(ctx, t->generic_args[i]);
                }
                iron_strbuf_appendf(ctx->sb, "]");
            }
        }
    } else {
        /* Fallback: print as identifier */
        print_node(ctx, node);
    }
}

/* Print a parameter */
static void print_param(PrintCtx *ctx, Iron_Node *node) {
    if (!node || node->kind != IRON_NODE_PARAM) return;
    Iron_Param *p = (Iron_Param *)node;
    if (p->is_var) iron_strbuf_appendf(ctx->sb, "var ");
    iron_strbuf_appendf(ctx->sb, "%s", p->name);
    if (p->type_ann) {
        iron_strbuf_appendf(ctx->sb, ": ");
        print_type_ann(ctx, p->type_ann);
    }
}

/* Print a comma-separated parameter list */
static void print_params(PrintCtx *ctx, Iron_Node **params, int count) {
    iron_strbuf_appendf(ctx->sb, "(");
    for (int i = 0; i < count; i++) {
        if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
        print_param(ctx, params[i]);
    }
    iron_strbuf_appendf(ctx->sb, ")");
}

/* Print optional generic params: [T, U] */
static void print_generic_params(PrintCtx *ctx, Iron_Node **gps, int count) {
    if (count == 0) return;
    iron_strbuf_appendf(ctx->sb, "[");
    for (int i = 0; i < count; i++) {
        if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
        if (gps[i]->kind == IRON_NODE_IDENT) {
            iron_strbuf_appendf(ctx->sb, "%s", ((Iron_Ident *)gps[i])->name);
        } else {
            print_type_ann(ctx, gps[i]);
        }
    }
    iron_strbuf_appendf(ctx->sb, "]");
}

/* Print a block with proper indentation */
static void print_block(PrintCtx *ctx, Iron_Node *node) {
    if (!node) return;
    if (node->kind != IRON_NODE_BLOCK) {
        print_node(ctx, node);
        return;
    }
    Iron_Block *blk = (Iron_Block *)node;
    iron_strbuf_appendf(ctx->sb, "{\n");
    ctx->indent_level++;
    for (int i = 0; i < blk->stmt_count; i++) {
        print_indent(ctx);
        print_node(ctx, blk->stmts[i]);
        iron_strbuf_appendf(ctx->sb, "\n");
    }
    ctx->indent_level--;
    print_indent(ctx);
    iron_strbuf_appendf(ctx->sb, "}");
}

/* Print a call argument list */
static void print_args(PrintCtx *ctx, Iron_Node **args, int count) {
    iron_strbuf_appendf(ctx->sb, "(");
    for (int i = 0; i < count; i++) {
        if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
        print_node(ctx, args[i]);
    }
    iron_strbuf_appendf(ctx->sb, ")");
}

/* ── Main node printer ───────────────────────────────────────────────────── */

static void print_node(PrintCtx *ctx, Iron_Node *node) {
    if (!node) return;

    switch (node->kind) {

        /* ── Program ── */
        case IRON_NODE_PROGRAM: {
            Iron_Program *n = (Iron_Program *)node;
            for (int i = 0; i < n->decl_count; i++) {
                if (i > 0) iron_strbuf_appendf(ctx->sb, "\n");
                print_node(ctx, n->decls[i]);
                iron_strbuf_appendf(ctx->sb, "\n");
            }
            break;
        }

        /* ── Top-level declarations ── */
        case IRON_NODE_IMPORT_DECL: {
            Iron_ImportDecl *n = (Iron_ImportDecl *)node;
            iron_strbuf_appendf(ctx->sb, "import %s", n->path);
            if (n->alias) iron_strbuf_appendf(ctx->sb, " as %s", n->alias);
            break;
        }

        case IRON_NODE_OBJECT_DECL: {
            Iron_ObjectDecl *n = (Iron_ObjectDecl *)node;
            iron_strbuf_appendf(ctx->sb, "object %s", n->name);
            print_generic_params(ctx, n->generic_params, n->generic_param_count);
            if (n->extends_name) {
                iron_strbuf_appendf(ctx->sb, " extends %s", n->extends_name);
            }
            if (n->implements_count > 0) {
                iron_strbuf_appendf(ctx->sb, " implements ");
                for (int i = 0; i < n->implements_count; i++) {
                    if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
                    iron_strbuf_appendf(ctx->sb, "%s", n->implements_names[i]);
                }
            }
            iron_strbuf_appendf(ctx->sb, " {\n");
            ctx->indent_level++;
            for (int i = 0; i < n->field_count; i++) {
                print_indent(ctx);
                Iron_Field *f = (Iron_Field *)n->fields[i];
                iron_strbuf_appendf(ctx->sb, "%s %s", f->is_var ? "var" : "val", f->name);
                if (f->type_ann) {
                    iron_strbuf_appendf(ctx->sb, ": ");
                    print_type_ann(ctx, f->type_ann);
                }
                iron_strbuf_appendf(ctx->sb, "\n");
            }
            ctx->indent_level--;
            print_indent(ctx);
            iron_strbuf_appendf(ctx->sb, "}");
            break;
        }

        case IRON_NODE_INTERFACE_DECL: {
            Iron_InterfaceDecl *n = (Iron_InterfaceDecl *)node;
            iron_strbuf_appendf(ctx->sb, "interface %s {\n", n->name);
            ctx->indent_level++;
            for (int i = 0; i < n->method_count; i++) {
                print_indent(ctx);
                Iron_FuncDecl *sig = (Iron_FuncDecl *)n->method_sigs[i];
                iron_strbuf_appendf(ctx->sb, "func %s", sig->name);
                print_params(ctx, sig->params, sig->param_count);
                if (sig->return_type) {
                    iron_strbuf_appendf(ctx->sb, " -> ");
                    print_type_ann(ctx, sig->return_type);
                }
                iron_strbuf_appendf(ctx->sb, "\n");
            }
            ctx->indent_level--;
            print_indent(ctx);
            iron_strbuf_appendf(ctx->sb, "}");
            break;
        }

        case IRON_NODE_ENUM_DECL: {
            Iron_EnumDecl *n = (Iron_EnumDecl *)node;
            iron_strbuf_appendf(ctx->sb, "enum %s {\n", n->name);
            ctx->indent_level++;
            for (int i = 0; i < n->variant_count; i++) {
                print_indent(ctx);
                Iron_EnumVariant *v = (Iron_EnumVariant *)n->variants[i];
                iron_strbuf_appendf(ctx->sb, "%s,\n", v->name);
            }
            ctx->indent_level--;
            print_indent(ctx);
            iron_strbuf_appendf(ctx->sb, "}");
            break;
        }

        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *n = (Iron_FuncDecl *)node;
            if (n->is_private) iron_strbuf_appendf(ctx->sb, "private ");
            iron_strbuf_appendf(ctx->sb, "func %s", n->name);
            print_generic_params(ctx, n->generic_params, n->generic_param_count);
            print_params(ctx, n->params, n->param_count);
            if (n->return_type) {
                iron_strbuf_appendf(ctx->sb, " -> ");
                print_type_ann(ctx, n->return_type);
            }
            if (n->body) {
                iron_strbuf_appendf(ctx->sb, " ");
                print_block(ctx, n->body);
            }
            break;
        }

        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *n = (Iron_MethodDecl *)node;
            if (n->is_private) iron_strbuf_appendf(ctx->sb, "private ");
            iron_strbuf_appendf(ctx->sb, "func %s.%s", n->type_name, n->method_name);
            print_generic_params(ctx, n->generic_params, n->generic_param_count);
            print_params(ctx, n->params, n->param_count);
            if (n->return_type) {
                iron_strbuf_appendf(ctx->sb, " -> ");
                print_type_ann(ctx, n->return_type);
            }
            if (n->body) {
                iron_strbuf_appendf(ctx->sb, " ");
                print_block(ctx, n->body);
            }
            break;
        }

        /* ── Statements ── */
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *n = (Iron_ValDecl *)node;
            iron_strbuf_appendf(ctx->sb, "val %s", n->name);
            if (n->type_ann) {
                iron_strbuf_appendf(ctx->sb, ": ");
                print_type_ann(ctx, n->type_ann);
            }
            if (n->init) {
                iron_strbuf_appendf(ctx->sb, " = ");
                print_node(ctx, n->init);
            }
            break;
        }

        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *n = (Iron_VarDecl *)node;
            iron_strbuf_appendf(ctx->sb, "var %s", n->name);
            if (n->type_ann) {
                iron_strbuf_appendf(ctx->sb, ": ");
                print_type_ann(ctx, n->type_ann);
            }
            if (n->init) {
                iron_strbuf_appendf(ctx->sb, " = ");
                print_node(ctx, n->init);
            }
            break;
        }

        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *n = (Iron_AssignStmt *)node;
            print_node(ctx, n->target);
            iron_strbuf_appendf(ctx->sb, " %s ", op_str(n->op));
            print_node(ctx, n->value);
            break;
        }

        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *n = (Iron_ReturnStmt *)node;
            if (n->value) {
                iron_strbuf_appendf(ctx->sb, "return ");
                print_node(ctx, n->value);
            } else {
                iron_strbuf_appendf(ctx->sb, "return");
            }
            break;
        }

        case IRON_NODE_IF: {
            Iron_IfStmt *n = (Iron_IfStmt *)node;
            iron_strbuf_appendf(ctx->sb, "if ");
            print_node(ctx, n->condition);
            iron_strbuf_appendf(ctx->sb, " ");
            print_block(ctx, n->body);
            for (int i = 0; i < n->elif_count; i++) {
                iron_strbuf_appendf(ctx->sb, " elif ");
                print_node(ctx, n->elif_conds[i]);
                iron_strbuf_appendf(ctx->sb, " ");
                print_block(ctx, n->elif_bodies[i]);
            }
            if (n->else_body) {
                iron_strbuf_appendf(ctx->sb, " else ");
                print_block(ctx, n->else_body);
            }
            break;
        }

        case IRON_NODE_WHILE: {
            Iron_WhileStmt *n = (Iron_WhileStmt *)node;
            iron_strbuf_appendf(ctx->sb, "while ");
            print_node(ctx, n->condition);
            iron_strbuf_appendf(ctx->sb, " ");
            print_block(ctx, n->body);
            break;
        }

        case IRON_NODE_FOR: {
            Iron_ForStmt *n = (Iron_ForStmt *)node;
            iron_strbuf_appendf(ctx->sb, "for %s in ", n->var_name);
            print_node(ctx, n->iterable);
            if (n->is_parallel) {
                iron_strbuf_appendf(ctx->sb, " parallel");
                if (n->pool_expr) {
                    iron_strbuf_appendf(ctx->sb, "(");
                    print_node(ctx, n->pool_expr);
                    iron_strbuf_appendf(ctx->sb, ")");
                }
            }
            iron_strbuf_appendf(ctx->sb, " ");
            print_block(ctx, n->body);
            break;
        }

        case IRON_NODE_MATCH: {
            Iron_MatchStmt *n = (Iron_MatchStmt *)node;
            iron_strbuf_appendf(ctx->sb, "match ");
            print_node(ctx, n->subject);
            iron_strbuf_appendf(ctx->sb, " {\n");
            ctx->indent_level++;
            for (int i = 0; i < n->case_count; i++) {
                print_indent(ctx);
                Iron_MatchCase *mc = (Iron_MatchCase *)n->cases[i];
                print_node(ctx, mc->pattern);
                iron_strbuf_appendf(ctx->sb, " ");
                print_block(ctx, mc->body);
                iron_strbuf_appendf(ctx->sb, "\n");
            }
            if (n->else_body) {
                print_indent(ctx);
                iron_strbuf_appendf(ctx->sb, "else ");
                print_block(ctx, n->else_body);
                iron_strbuf_appendf(ctx->sb, "\n");
            }
            ctx->indent_level--;
            print_indent(ctx);
            iron_strbuf_appendf(ctx->sb, "}");
            break;
        }

        case IRON_NODE_DEFER: {
            Iron_DeferStmt *n = (Iron_DeferStmt *)node;
            iron_strbuf_appendf(ctx->sb, "defer ");
            print_node(ctx, n->expr);
            break;
        }

        case IRON_NODE_FREE: {
            Iron_FreeStmt *n = (Iron_FreeStmt *)node;
            iron_strbuf_appendf(ctx->sb, "free ");
            print_node(ctx, n->expr);
            break;
        }

        case IRON_NODE_LEAK: {
            Iron_LeakStmt *n = (Iron_LeakStmt *)node;
            iron_strbuf_appendf(ctx->sb, "leak ");
            print_node(ctx, n->expr);
            break;
        }

        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *n = (Iron_SpawnStmt *)node;
            iron_strbuf_appendf(ctx->sb, "spawn(\"%s\"", n->name ? n->name : "");
            if (n->pool_expr) {
                iron_strbuf_appendf(ctx->sb, ", ");
                print_node(ctx, n->pool_expr);
            }
            iron_strbuf_appendf(ctx->sb, ") ");
            print_block(ctx, n->body);
            break;
        }

        case IRON_NODE_BLOCK: {
            print_block(ctx, node);
            break;
        }

        /* ── Expressions ── */
        case IRON_NODE_INT_LIT: {
            iron_strbuf_appendf(ctx->sb, "%s", ((Iron_IntLit *)node)->value);
            break;
        }

        case IRON_NODE_FLOAT_LIT: {
            iron_strbuf_appendf(ctx->sb, "%s", ((Iron_FloatLit *)node)->value);
            break;
        }

        case IRON_NODE_STRING_LIT: {
            iron_strbuf_appendf(ctx->sb, "\"%s\"", ((Iron_StringLit *)node)->value);
            break;
        }

        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *n = (Iron_InterpString *)node;
            iron_strbuf_appendf(ctx->sb, "\"");
            for (int i = 0; i < n->part_count; i++) {
                Iron_Node *part = n->parts[i];
                if (part->kind == IRON_NODE_STRING_LIT) {
                    /* Literal segment: print without quotes */
                    iron_strbuf_appendf(ctx->sb, "%s",
                                        ((Iron_StringLit *)part)->value);
                } else {
                    /* Expression segment: wrap in {} */
                    iron_strbuf_appendf(ctx->sb, "{");
                    print_node(ctx, part);
                    iron_strbuf_appendf(ctx->sb, "}");
                }
            }
            iron_strbuf_appendf(ctx->sb, "\"");
            break;
        }

        case IRON_NODE_BOOL_LIT: {
            iron_strbuf_appendf(ctx->sb, "%s",
                                ((Iron_BoolLit *)node)->value ? "true" : "false");
            break;
        }

        case IRON_NODE_NULL_LIT: {
            iron_strbuf_appendf(ctx->sb, "null");
            break;
        }

        case IRON_NODE_IDENT: {
            iron_strbuf_appendf(ctx->sb, "%s", ((Iron_Ident *)node)->name);
            break;
        }

        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *n = (Iron_BinaryExpr *)node;
            iron_strbuf_appendf(ctx->sb, "(");
            print_node(ctx, n->left);
            iron_strbuf_appendf(ctx->sb, " %s ", op_str(n->op));
            print_node(ctx, n->right);
            iron_strbuf_appendf(ctx->sb, ")");
            break;
        }

        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *n = (Iron_UnaryExpr *)node;
            if ((Iron_TokenKind)n->op == IRON_TOK_NOT) {
                iron_strbuf_appendf(ctx->sb, "not ");
            } else {
                iron_strbuf_appendf(ctx->sb, "%s", op_str(n->op));
            }
            print_node(ctx, n->operand);
            break;
        }

        case IRON_NODE_CALL: {
            Iron_CallExpr *n = (Iron_CallExpr *)node;
            print_node(ctx, n->callee);
            print_args(ctx, n->args, n->arg_count);
            break;
        }

        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *n = (Iron_MethodCallExpr *)node;
            print_node(ctx, n->object);
            iron_strbuf_appendf(ctx->sb, ".%s", n->method);
            print_args(ctx, n->args, n->arg_count);
            break;
        }

        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *n = (Iron_FieldAccess *)node;
            print_node(ctx, n->object);
            iron_strbuf_appendf(ctx->sb, ".%s", n->field);
            break;
        }

        case IRON_NODE_INDEX: {
            Iron_IndexExpr *n = (Iron_IndexExpr *)node;
            print_node(ctx, n->object);
            iron_strbuf_appendf(ctx->sb, "[");
            print_node(ctx, n->index);
            iron_strbuf_appendf(ctx->sb, "]");
            break;
        }

        case IRON_NODE_SLICE: {
            Iron_SliceExpr *n = (Iron_SliceExpr *)node;
            print_node(ctx, n->object);
            iron_strbuf_appendf(ctx->sb, "[");
            if (n->start) print_node(ctx, n->start);
            iron_strbuf_appendf(ctx->sb, "..");
            if (n->end) print_node(ctx, n->end);
            iron_strbuf_appendf(ctx->sb, "]");
            break;
        }

        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *n = (Iron_LambdaExpr *)node;
            iron_strbuf_appendf(ctx->sb, "func");
            print_params(ctx, n->params, n->param_count);
            if (n->return_type) {
                iron_strbuf_appendf(ctx->sb, " -> ");
                print_type_ann(ctx, n->return_type);
            }
            iron_strbuf_appendf(ctx->sb, " ");
            print_block(ctx, n->body);
            break;
        }

        case IRON_NODE_HEAP: {
            Iron_HeapExpr *n = (Iron_HeapExpr *)node;
            iron_strbuf_appendf(ctx->sb, "heap ");
            print_node(ctx, n->inner);
            break;
        }

        case IRON_NODE_RC: {
            Iron_RcExpr *n = (Iron_RcExpr *)node;
            iron_strbuf_appendf(ctx->sb, "rc ");
            print_node(ctx, n->inner);
            break;
        }

        case IRON_NODE_COMPTIME: {
            Iron_ComptimeExpr *n = (Iron_ComptimeExpr *)node;
            iron_strbuf_appendf(ctx->sb, "comptime ");
            print_node(ctx, n->inner);
            break;
        }

        case IRON_NODE_IS: {
            Iron_IsExpr *n = (Iron_IsExpr *)node;
            print_node(ctx, n->expr);
            iron_strbuf_appendf(ctx->sb, " is %s", n->type_name);
            break;
        }

        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *n = (Iron_AwaitExpr *)node;
            iron_strbuf_appendf(ctx->sb, "await ");
            print_node(ctx, n->handle);
            break;
        }

        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *n = (Iron_ConstructExpr *)node;
            iron_strbuf_appendf(ctx->sb, "%s", n->type_name);
            if (n->generic_arg_count > 0) {
                iron_strbuf_appendf(ctx->sb, "[");
                for (int i = 0; i < n->generic_arg_count; i++) {
                    if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
                    print_type_ann(ctx, n->generic_args[i]);
                }
                iron_strbuf_appendf(ctx->sb, "]");
            }
            print_args(ctx, n->args, n->arg_count);
            break;
        }

        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *n = (Iron_ArrayLit *)node;
            iron_strbuf_appendf(ctx->sb, "[");
            if (n->type_ann && n->size) {
                print_type_ann(ctx, n->type_ann);
                iron_strbuf_appendf(ctx->sb, "; ");
                print_node(ctx, n->size);
            } else {
                for (int i = 0; i < n->element_count; i++) {
                    if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
                    print_node(ctx, n->elements[i]);
                }
            }
            iron_strbuf_appendf(ctx->sb, "]");
            break;
        }

        case IRON_NODE_ERROR: {
            iron_strbuf_appendf(ctx->sb, "/* error */");
            break;
        }

        /* ── Helpers (printed inline above, but handle here as fallback) ── */
        case IRON_NODE_PARAM: {
            print_param(ctx, node);
            break;
        }

        case IRON_NODE_FIELD: {
            Iron_Field *f = (Iron_Field *)node;
            iron_strbuf_appendf(ctx->sb, "%s %s", f->is_var ? "var" : "val", f->name);
            if (f->type_ann) {
                iron_strbuf_appendf(ctx->sb, ": ");
                print_type_ann(ctx, f->type_ann);
            }
            break;
        }

        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *mc = (Iron_MatchCase *)node;
            print_node(ctx, mc->pattern);
            iron_strbuf_appendf(ctx->sb, " ");
            print_block(ctx, mc->body);
            break;
        }

        case IRON_NODE_ENUM_VARIANT: {
            iron_strbuf_appendf(ctx->sb, "%s", ((Iron_EnumVariant *)node)->name);
            break;
        }

        case IRON_NODE_TYPE_ANNOTATION: {
            print_type_ann(ctx, node);
            break;
        }

        case IRON_NODE_PATTERN: {
            Iron_Pattern *n = (Iron_Pattern *)node;
            if (n->enum_name) iron_strbuf_appendf(ctx->sb, "%s.", n->enum_name);
            iron_strbuf_appendf(ctx->sb, "%s", n->variant_name ? n->variant_name : "_");
            if (n->binding_count > 0) {
                iron_strbuf_appendf(ctx->sb, "(");
                for (int i = 0; i < n->binding_count; i++) {
                    if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
                    if (n->nested_patterns && n->nested_patterns[i]) {
                        print_node(ctx, n->nested_patterns[i]);
                    } else {
                        iron_strbuf_appendf(ctx->sb, "%s",
                            (n->binding_names && n->binding_names[i]) ? n->binding_names[i] : "_");
                    }
                }
                iron_strbuf_appendf(ctx->sb, ")");
            }
            break;
        }

        case IRON_NODE_ENUM_CONSTRUCT: {
            Iron_EnumConstruct *n = (Iron_EnumConstruct *)node;
            if (n->enum_name) iron_strbuf_appendf(ctx->sb, "%s.", n->enum_name);
            iron_strbuf_appendf(ctx->sb, "%s", n->variant_name ? n->variant_name : "");
            if (n->arg_count > 0) {
                print_args(ctx, n->args, n->arg_count);
            }
            break;
        }

        case IRON_NODE_COUNT:
            break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

char *iron_print_ast(Iron_Node *root, Iron_Arena *arena) {
    Iron_StrBuf sb  = iron_strbuf_create(512);
    PrintCtx    ctx = { &sb, 0 };
    print_node(&ctx, root);

    /* Copy result into arena */
    const char *result = iron_strbuf_get(&sb);
    size_t      len    = sb.len;
    char       *out    = (char *)iron_arena_alloc(arena, len + 1, 1);
    if (out) {
        memcpy(out, result, len + 1);
    }
    iron_strbuf_free(&sb);
    return out ? out : (char *)"";
}

void iron_print_ast_to_file(Iron_Node *root, FILE *out) {
    Iron_StrBuf sb  = iron_strbuf_create(512);
    PrintCtx    ctx = { &sb, 0 };
    print_node(&ctx, root);
    fprintf(out, "%s", iron_strbuf_get(&sb));
    iron_strbuf_free(&sb);
}
