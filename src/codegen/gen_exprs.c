/* gen_exprs.c — Expression emission for the Iron C code generator.
 *
 * Implements:
 *   emit_expr() — emit a single expression node
 */

#include "codegen/codegen.h"
#include "lexer/lexer.h"
#include "analyzer/scope.h"

#include <stdio.h>
#include <string.h>

/* ── emit_expr ────────────────────────────────────────────────────────────── */

void emit_expr(Iron_StrBuf *sb, Iron_Node *node, Iron_Codegen *ctx) {
    if (!node) return;

    switch (node->kind) {

        case IRON_NODE_INT_LIT: {
            /* Iron Int -> C int64_t literal */
            Iron_IntLit *lit = (Iron_IntLit *)node;
            iron_strbuf_appendf(sb, "(int64_t)%s", lit->value);
            break;
        }

        case IRON_NODE_FLOAT_LIT: {
            Iron_FloatLit *lit = (Iron_FloatLit *)node;
            iron_strbuf_appendf(sb, "%s", lit->value);
            break;
        }

        case IRON_NODE_BOOL_LIT: {
            Iron_BoolLit *lit = (Iron_BoolLit *)node;
            iron_strbuf_appendf(sb, "%s", lit->value ? "true" : "false");
            break;
        }

        case IRON_NODE_STRING_LIT: {
            /* Phase 2 stub: emit as raw C string literal */
            Iron_StringLit *lit = (Iron_StringLit *)node;
            iron_strbuf_appendf(sb, "\"%s\"", lit->value);
            break;
        }

        case IRON_NODE_INTERP_STRING: {
            /* Stub: emit empty string for now (Phase 3 will use snprintf) */
            iron_strbuf_appendf(sb, "\"\"");
            break;
        }

        case IRON_NODE_NULL_LIT: {
            /* Emit as { .has_value = false } for nullable context */
            iron_strbuf_appendf(sb, "{ .has_value = false }");
            break;
        }

        case IRON_NODE_IDENT: {
            Iron_Ident *id = (Iron_Ident *)node;
            /* Special case: self */
            if (strcmp(id->name, "self") == 0) {
                iron_strbuf_appendf(sb, "self");
                break;
            }
            /* Special case: super — emit as _base */
            if (strcmp(id->name, "super") == 0) {
                iron_strbuf_appendf(sb, "self->_base");
                break;
            }
            /* Normal identifier: emit as-is (scope variable names are kept) */
            iron_strbuf_appendf(sb, "%s", id->name);
            break;
        }

        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *bin = (Iron_BinaryExpr *)node;
            iron_strbuf_appendf(sb, "(");
            emit_expr(sb, bin->left, ctx);
            switch (bin->op) {
                case IRON_TOK_PLUS:         iron_strbuf_appendf(sb, " + ");  break;
                case IRON_TOK_MINUS:        iron_strbuf_appendf(sb, " - ");  break;
                case IRON_TOK_STAR:         iron_strbuf_appendf(sb, " * ");  break;
                case IRON_TOK_SLASH:        iron_strbuf_appendf(sb, " / ");  break;
                case IRON_TOK_PERCENT:      iron_strbuf_appendf(sb, " %% "); break;
                case IRON_TOK_EQUALS:       iron_strbuf_appendf(sb, " == "); break;
                case IRON_TOK_NOT_EQUALS:   iron_strbuf_appendf(sb, " != "); break;
                case IRON_TOK_LESS:         iron_strbuf_appendf(sb, " < ");  break;
                case IRON_TOK_GREATER:      iron_strbuf_appendf(sb, " > ");  break;
                case IRON_TOK_LESS_EQ:      iron_strbuf_appendf(sb, " <= "); break;
                case IRON_TOK_GREATER_EQ:   iron_strbuf_appendf(sb, " >= "); break;
                case IRON_TOK_AND:          iron_strbuf_appendf(sb, " && "); break;
                case IRON_TOK_OR:           iron_strbuf_appendf(sb, " || "); break;
                default:                    iron_strbuf_appendf(sb, " + ");  break;
            }
            emit_expr(sb, bin->right, ctx);
            iron_strbuf_appendf(sb, ")");
            break;
        }

        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *un = (Iron_UnaryExpr *)node;
            iron_strbuf_appendf(sb, "(");
            switch (un->op) {
                case IRON_TOK_MINUS: iron_strbuf_appendf(sb, "-"); break;
                case IRON_TOK_NOT:   iron_strbuf_appendf(sb, "!"); break;
                default:             iron_strbuf_appendf(sb, "-"); break;
            }
            emit_expr(sb, un->operand, ctx);
            iron_strbuf_appendf(sb, ")");
            break;
        }

        case IRON_NODE_CALL: {
            Iron_CallExpr *call = (Iron_CallExpr *)node;
            /* Special case: print/println -> printf stub */
            if (call->callee->kind == IRON_NODE_IDENT) {
                Iron_Ident *callee_id = (Iron_Ident *)call->callee;
                if (strcmp(callee_id->name, "print") == 0 ||
                    strcmp(callee_id->name, "println") == 0) {
                    iron_strbuf_appendf(sb, "printf(");
                    for (int i = 0; i < call->arg_count; i++) {
                        if (i > 0) iron_strbuf_appendf(sb, ", ");
                        emit_expr(sb, call->args[i], ctx);
                    }
                    if (strcmp(callee_id->name, "println") == 0) {
                        if (call->arg_count > 0) {
                            iron_strbuf_appendf(sb, ", \"\\n\"");
                        } else {
                            iron_strbuf_appendf(sb, "\"\\n\"");
                        }
                    }
                    iron_strbuf_appendf(sb, ")");
                    break;
                }
                /* Regular function call: mangle the name */
                const char *mangled = iron_mangle_name(callee_id->name,
                                                        ctx->arena);
                iron_strbuf_appendf(sb, "%s(", mangled);
                for (int i = 0; i < call->arg_count; i++) {
                    if (i > 0) iron_strbuf_appendf(sb, ", ");
                    emit_expr(sb, call->args[i], ctx);
                }
                iron_strbuf_appendf(sb, ")");
            } else {
                /* Complex callee expression */
                emit_expr(sb, call->callee, ctx);
                iron_strbuf_appendf(sb, "(");
                for (int i = 0; i < call->arg_count; i++) {
                    if (i > 0) iron_strbuf_appendf(sb, ", ");
                    emit_expr(sb, call->args[i], ctx);
                }
                iron_strbuf_appendf(sb, ")");
            }
            break;
        }

        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            /* Determine the type of the object to get the method mangling */
            const char *type_name = NULL;
            if (mc->object->kind == IRON_NODE_IDENT) {
                Iron_Ident *obj_id = (Iron_Ident *)mc->object;
                if (obj_id->resolved_type &&
                    obj_id->resolved_type->kind == IRON_TYPE_OBJECT) {
                    type_name = obj_id->resolved_type->object.decl->name;
                }
            }
            if (type_name) {
                const char *mangled = iron_mangle_method(type_name,
                                                          mc->method,
                                                          ctx->arena);
                iron_strbuf_appendf(sb, "%s(&", mangled);
                emit_expr(sb, mc->object, ctx);
                for (int i = 0; i < mc->arg_count; i++) {
                    iron_strbuf_appendf(sb, ", ");
                    emit_expr(sb, mc->args[i], ctx);
                }
                iron_strbuf_appendf(sb, ")");
            } else {
                /* Fallback: object.method(args) style */
                emit_expr(sb, mc->object, ctx);
                iron_strbuf_appendf(sb, ".%s(", mc->method);
                for (int i = 0; i < mc->arg_count; i++) {
                    if (i > 0) iron_strbuf_appendf(sb, ", ");
                    emit_expr(sb, mc->args[i], ctx);
                }
                iron_strbuf_appendf(sb, ")");
            }
            break;
        }

        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
            emit_expr(sb, fa->object, ctx);
            iron_strbuf_appendf(sb, ".%s", fa->field);
            break;
        }

        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;
            const char *mangled = iron_mangle_name(ce->type_name, ctx->arena);
            iron_strbuf_appendf(sb, "(%s){", mangled);
            /* Get the object decl to get field names */
            Iron_Symbol *sym = iron_scope_lookup(ctx->global_scope,
                                                  ce->type_name);
            if (sym && sym->decl_node &&
                sym->decl_node->kind == IRON_NODE_OBJECT_DECL) {
                Iron_ObjectDecl *od = (Iron_ObjectDecl *)sym->decl_node;
                int n = ce->arg_count < od->field_count ? ce->arg_count
                                                         : od->field_count;
                for (int i = 0; i < n; i++) {
                    if (i > 0) iron_strbuf_appendf(sb, ", ");
                    Iron_Field *f = (Iron_Field *)od->fields[i];
                    iron_strbuf_appendf(sb, ".%s = ", f->name);
                    emit_expr(sb, ce->args[i], ctx);
                }
            } else {
                /* Fallback: positional init */
                for (int i = 0; i < ce->arg_count; i++) {
                    if (i > 0) iron_strbuf_appendf(sb, ", ");
                    emit_expr(sb, ce->args[i], ctx);
                }
            }
            iron_strbuf_appendf(sb, "}");
            break;
        }

        case IRON_NODE_INDEX: {
            Iron_IndexExpr *ie = (Iron_IndexExpr *)node;
            emit_expr(sb, ie->object, ctx);
            iron_strbuf_appendf(sb, "[");
            emit_expr(sb, ie->index, ctx);
            iron_strbuf_appendf(sb, "]");
            break;
        }

        case IRON_NODE_HEAP: {
            Iron_HeapExpr *he = (Iron_HeapExpr *)node;
            /* Emit malloc + compound literal initialization */
            if (he->inner && he->inner->kind == IRON_NODE_CONSTRUCT) {
                Iron_ConstructExpr *ce = (Iron_ConstructExpr *)he->inner;
                const char *mangled = iron_mangle_name(ce->type_name,
                                                        ctx->arena);
                iron_strbuf_appendf(sb, "(%s*)malloc(sizeof(%s))",
                                    mangled, mangled);
            } else {
                /* Generic heap allocation */
                iron_strbuf_appendf(sb, "malloc(sizeof(");
                emit_expr(sb, he->inner, ctx);
                iron_strbuf_appendf(sb, "))");
            }
            break;
        }

        case IRON_NODE_RC: {
            Iron_RcExpr *re = (Iron_RcExpr *)node;
            /* Stub: emit the inner expression (Phase 3 will wrap in Rc struct) */
            emit_expr(sb, re->inner, ctx);
            break;
        }

        case IRON_NODE_IS: {
            Iron_IsExpr *ie = (Iron_IsExpr *)node;
            /* Emit type tag comparison */
            const char *mangled = iron_mangle_name(ie->type_name, ctx->arena);
            iron_strbuf_appendf(sb, "(((Iron_Base_*)&(");
            emit_expr(sb, ie->expr, ctx);
            iron_strbuf_appendf(sb, "))->iron_type_tag == IRON_TAG_%s)",
                                mangled);
            break;
        }

        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)node;
            iron_strbuf_appendf(sb, "{");
            for (int i = 0; i < al->element_count; i++) {
                if (i > 0) iron_strbuf_appendf(sb, ", ");
                emit_expr(sb, al->elements[i], ctx);
            }
            iron_strbuf_appendf(sb, "}");
            break;
        }

        case IRON_NODE_COMPTIME: {
            Iron_ComptimeExpr *ce = (Iron_ComptimeExpr *)node;
            /* Stub: emit inner expr (Phase 4 handles compile-time eval) */
            emit_expr(sb, ce->inner, ctx);
            break;
        }

        case IRON_NODE_AWAIT: {
            /* Stub for Phase 3 */
            iron_strbuf_appendf(sb, "0 /* await stub */");
            break;
        }

        default:
            /* Unknown node in expression context */
            iron_strbuf_appendf(sb, "0 /* unsupported expr %d */",
                                (int)node->kind);
            break;
    }
}
