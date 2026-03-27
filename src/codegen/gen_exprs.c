/* gen_exprs.c — Expression emission for the Iron C code generator.
 *
 * Implements:
 *   emit_expr()   — emit a single expression node
 *   emit_lambda() — lift a lambda to a named C function
 */

#include "codegen/codegen.h"
#include "lexer/lexer.h"
#include "analyzer/scope.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ── Lambda capture collection ────────────────────────────────────────────── */

/* Collect identifiers used in the body that are NOT in the param list.
 * These are potential captures from the enclosing scope.
 * Returns an stb_ds array of const char* names (caller must arrfree). */
const char **collect_captures(Iron_Node *body, Iron_Node **params,
                               int param_count) {
    const char **captures = NULL;

    /* Walk the body looking for IRON_NODE_IDENT nodes */
    /* Simple recursive walk */
    if (!body) return NULL;

    /* We use a simple DFS without the full visitor mechanism */
    Iron_Node **stack = NULL;
    arrput(stack, body);

    while (arrlen(stack) > 0) {
        Iron_Node *node = stack[arrlen(stack) - 1];
        arrsetlen(stack, arrlen(stack) - 1);

        if (!node) continue;

        if (node->kind == IRON_NODE_IDENT) {
            Iron_Ident *id = (Iron_Ident *)node;
            /* Skip "self" and "super" */
            if (strcmp(id->name, "self") == 0 ||
                strcmp(id->name, "super") == 0) {
                continue;
            }
            /* Skip if it's a param name */
            bool is_param = false;
            for (int i = 0; i < param_count; i++) {
                Iron_Param *p = (Iron_Param *)params[i];
                if (strcmp(p->name, id->name) == 0) {
                    is_param = true;
                    break;
                }
            }
            if (is_param) continue;
            /* Check if already in captures list */
            bool already = false;
            for (int i = 0; i < (int)arrlen(captures); i++) {
                if (strcmp(captures[i], id->name) == 0) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                arrput(captures, id->name);
            }
            continue;
        }

        /* Push children based on node kind */
        switch (node->kind) {
            case IRON_NODE_BLOCK: {
                Iron_Block *blk = (Iron_Block *)node;
                for (int i = 0; i < blk->stmt_count; i++) {
                    arrput(stack, blk->stmts[i]);
                }
                break;
            }
            case IRON_NODE_VAL_DECL: {
                Iron_ValDecl *vd = (Iron_ValDecl *)node;
                if (vd->init) arrput(stack, vd->init);
                break;
            }
            case IRON_NODE_VAR_DECL: {
                Iron_VarDecl *vd = (Iron_VarDecl *)node;
                if (vd->init) arrput(stack, vd->init);
                break;
            }
            case IRON_NODE_ASSIGN: {
                Iron_AssignStmt *as = (Iron_AssignStmt *)node;
                arrput(stack, as->target);
                arrput(stack, as->value);
                break;
            }
            case IRON_NODE_RETURN: {
                Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
                if (rs->value) arrput(stack, rs->value);
                break;
            }
            case IRON_NODE_BINARY: {
                Iron_BinaryExpr *bin = (Iron_BinaryExpr *)node;
                arrput(stack, bin->left);
                arrput(stack, bin->right);
                break;
            }
            case IRON_NODE_UNARY: {
                Iron_UnaryExpr *un = (Iron_UnaryExpr *)node;
                arrput(stack, un->operand);
                break;
            }
            case IRON_NODE_CALL: {
                Iron_CallExpr *ce = (Iron_CallExpr *)node;
                arrput(stack, ce->callee);
                for (int i = 0; i < ce->arg_count; i++) {
                    arrput(stack, ce->args[i]);
                }
                break;
            }
            case IRON_NODE_IF: {
                Iron_IfStmt *is = (Iron_IfStmt *)node;
                arrput(stack, is->condition);
                arrput(stack, is->body);
                if (is->else_body) arrput(stack, is->else_body);
                break;
            }
            case IRON_NODE_INDEX: {
                Iron_IndexExpr *idx = (Iron_IndexExpr *)node;
                arrput(stack, idx->object);
                arrput(stack, idx->index);
                break;
            }
            case IRON_NODE_FIELD_ACCESS: {
                Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
                arrput(stack, fa->object);
                break;
            }
            case IRON_NODE_METHOD_CALL: {
                Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
                arrput(stack, mc->object);
                for (int i = 0; i < mc->arg_count; i++) {
                    arrput(stack, mc->args[i]);
                }
                break;
            }
            case IRON_NODE_FOR: {
                Iron_ForStmt *fs2 = (Iron_ForStmt *)node;
                arrput(stack, fs2->iterable);
                if (fs2->body) arrput(stack, fs2->body);
                break;
            }
            case IRON_NODE_WHILE: {
                Iron_WhileStmt *ws2 = (Iron_WhileStmt *)node;
                arrput(stack, ws2->condition);
                if (ws2->body) arrput(stack, ws2->body);
                break;
            }
            default:
                /* Skip other nodes for capture detection */
                break;
        }
    }

    arrfree(stack);
    return captures;
}

/* ── emit_lambda ──────────────────────────────────────────────────────────── */

void emit_lambda(Iron_StrBuf *sb, Iron_Node *node, Iron_Codegen *ctx,
                 const char *enclosing_name) {
    Iron_LambdaExpr *lam = (Iron_LambdaExpr *)node;

    /* Generate unique lambda name: Iron_<enclosing>_lambda_<N> */
    int lambda_idx = ctx->lambda_counter++;
    char lambda_name[256];
    snprintf(lambda_name, sizeof(lambda_name), "Iron_%s_lambda_%d",
             enclosing_name ? enclosing_name : "anon", lambda_idx);

    /* Collect captures from body */
    const char **captures = collect_captures(lam->body, lam->params,
                                             lam->param_count);
    int capture_count = (int)arrlen(captures);

    if (capture_count > 0) {
        /* ── Closure: generate env struct and lifted function ── */
        char env_name[320];
        snprintf(env_name, sizeof(env_name), "%s_env", lambda_name);

        /* Forward declare env struct */
        iron_strbuf_appendf(&ctx->forward_decls,
                             "typedef struct %s %s;\n", env_name, env_name);

        /* Emit env struct into struct_bodies */
        iron_strbuf_appendf(&ctx->struct_bodies, "struct %s {\n", env_name);
        for (int i = 0; i < capture_count; i++) {
            /* Emit as int64_t* for captured vars (pointer for mutation) */
            iron_strbuf_appendf(&ctx->struct_bodies,
                                 "    int64_t* %s;\n", captures[i]);
        }
        iron_strbuf_appendf(&ctx->struct_bodies, "};\n");

        /* Lifted function: ret lambda_name(env_name* env, params...) */
        const char *ret_type = "void";
        if (lam->resolved_type) {
            /* Lambda's function return type */
            ret_type = "void"; /* simplified for Phase 2 */
        }
        iron_strbuf_appendf(&ctx->lifted_funcs,
                             "%s %s(%s* env", ret_type, lambda_name, env_name);
        for (int i = 0; i < lam->param_count; i++) {
            Iron_Param *p = (Iron_Param *)lam->params[i];
            const char *pt = "int64_t";
            if (p->type_ann) {
                Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)p->type_ann;
                if (strcmp(ta->name, "Int") == 0) pt = "int64_t";
                else if (strcmp(ta->name, "Float") == 0) pt = "double";
                else if (strcmp(ta->name, "Bool") == 0) pt = "bool";
                else if (strcmp(ta->name, "String") == 0) pt = "Iron_String";
                else pt = iron_mangle_name(ta->name, ctx->arena);
            }
            iron_strbuf_appendf(&ctx->lifted_funcs, ", %s %s", pt, p->name);
        }
        iron_strbuf_appendf(&ctx->lifted_funcs, ") {\n");
        if (lam->body) {
            emit_block(&ctx->lifted_funcs, (Iron_Block *)lam->body, ctx);
        }
        iron_strbuf_appendf(&ctx->lifted_funcs, "}\n\n");

        /* At use site: allocate env and populate captured vars */
        iron_strbuf_appendf(sb, "({ %s* _env = malloc(sizeof(%s));\n",
                             env_name, env_name);
        for (int i = 0; i < capture_count; i++) {
            iron_strbuf_appendf(sb, "    _env->%s = &%s;\n",
                                 captures[i], captures[i]);
        }
        iron_strbuf_appendf(sb, "    %s; })", lambda_name);
    } else {
        /* ── Pure lambda (no captures): lift as regular function ── */
        const char *ret_type = "void";
        if (lam->resolved_type) {
            ret_type = "void"; /* simplified for Phase 2 */
        }

        /* Determine return type from annotation if available */
        if (lam->return_type) {
            Iron_TypeAnnotation *rta = (Iron_TypeAnnotation *)lam->return_type;
            if (rta->kind == IRON_NODE_TYPE_ANNOTATION) {
                if (strcmp(rta->name, "Int") == 0) ret_type = "int64_t";
                else if (strcmp(rta->name, "Float") == 0) ret_type = "double";
                else if (strcmp(rta->name, "Bool") == 0) ret_type = "bool";
                else if (strcmp(rta->name, "Void") == 0) ret_type = "void";
                else if (strcmp(rta->name, "String") == 0) ret_type = "Iron_String";
                else ret_type = iron_mangle_name(rta->name, ctx->arena);
            }
        }

        iron_strbuf_appendf(&ctx->lifted_funcs,
                             "%s %s(", ret_type, lambda_name);
        for (int i = 0; i < lam->param_count; i++) {
            if (i > 0) iron_strbuf_appendf(&ctx->lifted_funcs, ", ");
            Iron_Param *p = (Iron_Param *)lam->params[i];
            const char *pt = "int64_t";
            if (p->type_ann) {
                Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)p->type_ann;
                if (strcmp(ta->name, "Int") == 0) pt = "int64_t";
                else if (strcmp(ta->name, "Float") == 0) pt = "double";
                else if (strcmp(ta->name, "Bool") == 0) pt = "bool";
                else if (strcmp(ta->name, "String") == 0) pt = "Iron_String";
                else pt = iron_mangle_name(ta->name, ctx->arena);
            }
            iron_strbuf_appendf(&ctx->lifted_funcs, "%s %s", pt, p->name);
        }
        iron_strbuf_appendf(&ctx->lifted_funcs, ") {\n");
        if (lam->body) {
            emit_block(&ctx->lifted_funcs, (Iron_Block *)lam->body, ctx);
        }
        iron_strbuf_appendf(&ctx->lifted_funcs, "}\n\n");

        /* At use site: emit the function pointer */
        iron_strbuf_appendf(sb, "%s", lambda_name);
    }

    arrfree(captures);
}

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
            /* Emit as Iron_String using iron_string_from_literal() so that
             * functions accepting Iron_String (e.g. Iron_println) get the
             * correct type.  The intern call deduplicates identical literals. */
            Iron_StringLit *lit = (Iron_StringLit *)node;
            iron_strbuf_appendf(sb,
                "iron_string_from_literal(\"%s\", %zu)",
                lit->value, strlen(lit->value));
            break;
        }

        case IRON_NODE_INTERP_STRING: {
            /* Two-pass snprintf interpolation using a GNU statement expression.
             * The format string is built at Iron compile time by scanning parts;
             * only the snprintf call itself runs at C runtime.
             *
             * Variable naming uses a unique index (_interp_idx) so that nested
             * interpolations don't collide. */
            Iron_InterpString *istr = (Iron_InterpString *)node;
            int interp_idx = ctx->lambda_counter++;

            /* ── Pass 1: build format string at codegen time ── */
            size_t fmt_cap = 256;
            char  *fmt_buf = (char *)malloc(fmt_cap);
            size_t fmt_len = 0;
            fmt_buf[0] = '\0';

#define INTERP_APPEND_CHAR(ch) do { \
    if (fmt_len + 2 >= fmt_cap) { \
        fmt_cap *= 2; \
        fmt_buf = (char *)realloc(fmt_buf, fmt_cap); \
    } \
    fmt_buf[fmt_len++] = (ch); \
    fmt_buf[fmt_len]   = '\0'; \
} while (0)

#define INTERP_APPEND_STR(str) do { \
    const char *_sp = (str); \
    while (*_sp) INTERP_APPEND_CHAR(*_sp++); \
} while (0)

            for (int pi = 0; pi < istr->part_count; pi++) {
                Iron_Node *part = istr->parts[pi];
                if (part->kind == IRON_NODE_STRING_LIT) {
                    /* Literal segment: copy chars, escaping % -> %% */
                    Iron_StringLit *slit = (Iron_StringLit *)part;
                    for (const char *cp = slit->value; *cp; cp++) {
                        if (*cp == '%') {
                            INTERP_APPEND_CHAR('%');
                            INTERP_APPEND_CHAR('%');
                        } else if (*cp == '"') {
                            INTERP_APPEND_CHAR('\\');
                            INTERP_APPEND_CHAR('"');
                        } else if (*cp == '\\') {
                            INTERP_APPEND_CHAR('\\');
                            INTERP_APPEND_CHAR('\\');
                        } else {
                            INTERP_APPEND_CHAR(*cp);
                        }
                    }
                } else {
                    /* Expression segment: choose format specifier by type.
                     * Cast through Iron_IntLit* to access resolved_type which
                     * all expression nodes share at the same struct offset. */
                    Iron_Type *t = ((Iron_IntLit *)part)->resolved_type;
                    if (!t) {
                        INTERP_APPEND_STR("%s");
                    } else {
                        switch (t->kind) {
                            case IRON_TYPE_INT:
                            case IRON_TYPE_INT8:
                            case IRON_TYPE_INT16:
                            case IRON_TYPE_INT32:
                            case IRON_TYPE_INT64:
                            case IRON_TYPE_UINT:
                            case IRON_TYPE_UINT8:
                            case IRON_TYPE_UINT16:
                            case IRON_TYPE_UINT32:
                            case IRON_TYPE_UINT64:
                                INTERP_APPEND_STR("%lld");
                                break;
                            case IRON_TYPE_FLOAT:
                            case IRON_TYPE_FLOAT32:
                            case IRON_TYPE_FLOAT64:
                                INTERP_APPEND_STR("%g");
                                break;
                            case IRON_TYPE_BOOL:
                                INTERP_APPEND_STR("%s");
                                break;
                            case IRON_TYPE_STRING:
                                INTERP_APPEND_STR("%s");
                                break;
                            default:
                                INTERP_APPEND_STR("%s");
                                break;
                        }
                    }
                }
            }

#undef INTERP_APPEND_CHAR
#undef INTERP_APPEND_STR

            /* ── Emit the GNU statement expression ── */
            iron_strbuf_appendf(sb, "({ ");

            /* Declare temp Iron_String variables for String-typed expression
             * parts so we can take their address for iron_string_cstr(). */
            int str_tmp = 0;
            for (int pi = 0; pi < istr->part_count; pi++) {
                Iron_Node *part = istr->parts[pi];
                if (part->kind == IRON_NODE_STRING_LIT) continue;
                Iron_Type *t = ((Iron_IntLit *)part)->resolved_type;
                if (t && t->kind == IRON_TYPE_STRING) {
                    iron_strbuf_appendf(sb, "Iron_String _is_%d_%d = ",
                                        interp_idx, str_tmp);
                    emit_expr(sb, part, ctx);
                    iron_strbuf_appendf(sb, "; ");
                    str_tmp++;
                }
            }

            /* Stack buffer + first snprintf (measurement + small-string path) */
            iron_strbuf_appendf(sb,
                "char _ibuf_%d[1024]; "
                "int _in_%d = snprintf(_ibuf_%d, sizeof(_ibuf_%d), \"%s\"",
                interp_idx, interp_idx, interp_idx, interp_idx, fmt_buf);

            /* Args for first snprintf */
            str_tmp = 0;
            for (int pi = 0; pi < istr->part_count; pi++) {
                Iron_Node *part = istr->parts[pi];
                if (part->kind == IRON_NODE_STRING_LIT) continue;
                Iron_Type *t = ((Iron_IntLit *)part)->resolved_type;
                iron_strbuf_appendf(sb, ", ");
                if (!t) {
                    emit_expr(sb, part, ctx);
                } else {
                    switch (t->kind) {
                        case IRON_TYPE_INT:
                        case IRON_TYPE_INT8:
                        case IRON_TYPE_INT16:
                        case IRON_TYPE_INT32:
                        case IRON_TYPE_INT64:
                        case IRON_TYPE_UINT:
                        case IRON_TYPE_UINT8:
                        case IRON_TYPE_UINT16:
                        case IRON_TYPE_UINT32:
                        case IRON_TYPE_UINT64:
                            iron_strbuf_appendf(sb, "(long long)(");
                            emit_expr(sb, part, ctx);
                            iron_strbuf_appendf(sb, ")");
                            break;
                        case IRON_TYPE_FLOAT:
                        case IRON_TYPE_FLOAT32:
                        case IRON_TYPE_FLOAT64:
                            iron_strbuf_appendf(sb, "(double)(");
                            emit_expr(sb, part, ctx);
                            iron_strbuf_appendf(sb, ")");
                            break;
                        case IRON_TYPE_BOOL:
                            iron_strbuf_appendf(sb, "((");
                            emit_expr(sb, part, ctx);
                            iron_strbuf_appendf(sb, ") ? \"true\" : \"false\")");
                            break;
                        case IRON_TYPE_STRING:
                            iron_strbuf_appendf(sb,
                                "iron_string_cstr(&_is_%d_%d)",
                                interp_idx, str_tmp);
                            str_tmp++;
                            break;
                        default:
                            emit_expr(sb, part, ctx);
                            break;
                    }
                }
            }
            iron_strbuf_appendf(sb, "); ");

            /* Result variable */
            iron_strbuf_appendf(sb, "Iron_String _ir_%d; ", interp_idx);

            /* Branch: stack buf fits vs heap needed */
            iron_strbuf_appendf(sb,
                "if (_in_%d < 1024) { "
                    "_ir_%d = iron_string_from_literal(_ibuf_%d, (size_t)_in_%d); "
                "} else { "
                    "char *_ihp_%d = (char*)malloc((size_t)_in_%d + 1); "
                    "snprintf(_ihp_%d, (size_t)_in_%d + 1, \"%s\"",
                interp_idx, interp_idx, interp_idx, interp_idx,
                interp_idx, interp_idx,
                interp_idx, interp_idx, fmt_buf);

            /* Args for second snprintf (heap path) */
            str_tmp = 0;
            for (int pi = 0; pi < istr->part_count; pi++) {
                Iron_Node *part = istr->parts[pi];
                if (part->kind == IRON_NODE_STRING_LIT) continue;
                Iron_Type *t = ((Iron_IntLit *)part)->resolved_type;
                iron_strbuf_appendf(sb, ", ");
                if (!t) {
                    emit_expr(sb, part, ctx);
                } else {
                    switch (t->kind) {
                        case IRON_TYPE_INT:
                        case IRON_TYPE_INT8:
                        case IRON_TYPE_INT16:
                        case IRON_TYPE_INT32:
                        case IRON_TYPE_INT64:
                        case IRON_TYPE_UINT:
                        case IRON_TYPE_UINT8:
                        case IRON_TYPE_UINT16:
                        case IRON_TYPE_UINT32:
                        case IRON_TYPE_UINT64:
                            iron_strbuf_appendf(sb, "(long long)(");
                            emit_expr(sb, part, ctx);
                            iron_strbuf_appendf(sb, ")");
                            break;
                        case IRON_TYPE_FLOAT:
                        case IRON_TYPE_FLOAT32:
                        case IRON_TYPE_FLOAT64:
                            iron_strbuf_appendf(sb, "(double)(");
                            emit_expr(sb, part, ctx);
                            iron_strbuf_appendf(sb, ")");
                            break;
                        case IRON_TYPE_BOOL:
                            iron_strbuf_appendf(sb, "((");
                            emit_expr(sb, part, ctx);
                            iron_strbuf_appendf(sb, ") ? \"true\" : \"false\")");
                            break;
                        case IRON_TYPE_STRING:
                            iron_strbuf_appendf(sb,
                                "iron_string_cstr(&_is_%d_%d)",
                                interp_idx, str_tmp);
                            str_tmp++;
                            break;
                        default:
                            emit_expr(sb, part, ctx);
                            break;
                    }
                }
            }

            /* Close second snprintf; wrap; free heap; end stmt expr */
            iron_strbuf_appendf(sb,
                "); "
                "_ir_%d = iron_string_from_literal(_ihp_%d, (size_t)_in_%d); "
                "free(_ihp_%d); "
                "} "
                "_ir_%d; })",
                interp_idx, interp_idx, interp_idx,
                interp_idx,
                interp_idx);

            free(fmt_buf);
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
            /* Special case: print/println/len/min/max/clamp/abs/assert -> Iron_ builtins */
            if (call->callee->kind == IRON_NODE_IDENT) {
                Iron_Ident *callee_id = (Iron_Ident *)call->callee;
                if (strcmp(callee_id->name, "println") == 0) {
                    if (call->arg_count == 0) {
                        iron_strbuf_appendf(sb,
                            "Iron_println(iron_string_from_cstr(\"\", 0))");
                    } else {
                        iron_strbuf_appendf(sb, "Iron_println(");
                        emit_expr(sb, call->args[0], ctx);
                        iron_strbuf_appendf(sb, ")");
                    }
                    break;
                }
                if (strcmp(callee_id->name, "print") == 0) {
                    if (call->arg_count == 0) {
                        iron_strbuf_appendf(sb,
                            "Iron_print(iron_string_from_cstr(\"\", 0))");
                    } else {
                        iron_strbuf_appendf(sb, "Iron_print(");
                        emit_expr(sb, call->args[0], ctx);
                        iron_strbuf_appendf(sb, ")");
                    }
                    break;
                }
                if (strcmp(callee_id->name, "len") == 0 && call->arg_count == 1) {
                    iron_strbuf_appendf(sb, "Iron_len(");
                    emit_expr(sb, call->args[0], ctx);
                    iron_strbuf_appendf(sb, ")");
                    break;
                }
                if (strcmp(callee_id->name, "min") == 0 && call->arg_count == 2) {
                    iron_strbuf_appendf(sb, "Iron_min(");
                    emit_expr(sb, call->args[0], ctx);
                    iron_strbuf_appendf(sb, ", ");
                    emit_expr(sb, call->args[1], ctx);
                    iron_strbuf_appendf(sb, ")");
                    break;
                }
                if (strcmp(callee_id->name, "max") == 0 && call->arg_count == 2) {
                    iron_strbuf_appendf(sb, "Iron_max(");
                    emit_expr(sb, call->args[0], ctx);
                    iron_strbuf_appendf(sb, ", ");
                    emit_expr(sb, call->args[1], ctx);
                    iron_strbuf_appendf(sb, ")");
                    break;
                }
                if (strcmp(callee_id->name, "clamp") == 0 && call->arg_count == 3) {
                    iron_strbuf_appendf(sb, "Iron_clamp(");
                    emit_expr(sb, call->args[0], ctx);
                    iron_strbuf_appendf(sb, ", ");
                    emit_expr(sb, call->args[1], ctx);
                    iron_strbuf_appendf(sb, ", ");
                    emit_expr(sb, call->args[2], ctx);
                    iron_strbuf_appendf(sb, ")");
                    break;
                }
                if (strcmp(callee_id->name, "abs") == 0 && call->arg_count == 1) {
                    iron_strbuf_appendf(sb, "Iron_abs(");
                    emit_expr(sb, call->args[0], ctx);
                    iron_strbuf_appendf(sb, ")");
                    break;
                }
                if (strcmp(callee_id->name, "range") == 0 && call->arg_count == 1) {
                    iron_strbuf_appendf(sb, "Iron_range(");
                    emit_expr(sb, call->args[0], ctx);
                    iron_strbuf_appendf(sb, ")");
                    break;
                }
                if (strcmp(callee_id->name, "assert") == 0 && call->arg_count >= 1) {
                    iron_strbuf_appendf(sb, "Iron_assert(");
                    emit_expr(sb, call->args[0], ctx);
                    if (call->arg_count >= 2) {
                        iron_strbuf_appendf(sb, ", ");
                        emit_expr(sb, call->args[1], ctx);
                    } else {
                        iron_strbuf_appendf(sb,
                            ", iron_string_from_cstr(\"assertion failed\", 17)");
                    }
                    iron_strbuf_appendf(sb, ")");
                    break;
                }
                /* Extern function call: use raw C name, no Iron_ prefix.
                 * String literals are passed as raw C strings. */
                if (callee_id->resolved_sym &&
                    callee_id->resolved_sym->is_extern) {
                    const char *c_name = callee_id->resolved_sym->extern_c_name;
                    if (!c_name || c_name[0] == '\0') c_name = callee_id->name;
                    iron_strbuf_appendf(sb, "%s(", c_name);
                    for (int i = 0; i < call->arg_count; i++) {
                        if (i > 0) iron_strbuf_appendf(sb, ", ");
                        Iron_Node *arg = call->args[i];
                        if (arg->kind == IRON_NODE_STRING_LIT) {
                            /* Auto-convert Iron String literal to const char* */
                            Iron_StringLit *slit = (Iron_StringLit *)arg;
                            iron_strbuf_appendf(sb, "\"%s\"", slit->value);
                        } else if (arg->kind == IRON_NODE_IDENT) {
                            Iron_Ident *aid = (Iron_Ident *)arg;
                            if (aid->resolved_type &&
                                aid->resolved_type->kind == IRON_TYPE_STRING) {
                                /* Wrap Iron_String variable with iron_string_cstr */
                                iron_strbuf_appendf(sb, "iron_string_cstr(&(");
                                emit_expr(sb, arg, ctx);
                                iron_strbuf_appendf(sb, "))");
                            } else {
                                emit_expr(sb, arg, ctx);
                            }
                        } else {
                            emit_expr(sb, arg, ctx);
                        }
                    }
                    iron_strbuf_appendf(sb, ")");
                    break;
                }

                /* Check if callee is a type name (object constructor).
                 * If so, emit as compound literal (Iron_TypeName){.field=arg}
                 * rather than a C function call Iron_TypeName(arg) which
                 * would be invalid C (no such function exists). */
                if (callee_id->resolved_sym &&
                    callee_id->resolved_sym->sym_kind == IRON_SYM_TYPE) {
                    const char *mangled = iron_mangle_name(callee_id->name,
                                                           ctx->arena);
                    iron_strbuf_appendf(sb, "(%s){", mangled);
                    /* Look up field names from the object declaration */
                    Iron_Symbol *sym = iron_scope_lookup(ctx->global_scope,
                                                         callee_id->name);
                    if (sym && sym->decl_node &&
                        sym->decl_node->kind == IRON_NODE_OBJECT_DECL) {
                        Iron_ObjectDecl *od = (Iron_ObjectDecl *)sym->decl_node;
                        int n = call->arg_count < od->field_count
                                    ? call->arg_count : od->field_count;
                        for (int i = 0; i < n; i++) {
                            if (i > 0) iron_strbuf_appendf(sb, ", ");
                            Iron_Field *f = (Iron_Field *)od->fields[i];
                            iron_strbuf_appendf(sb, ".%s = ", f->name);
                            emit_expr(sb, call->args[i], ctx);
                        }
                    } else {
                        /* Fallback: positional init */
                        for (int i = 0; i < call->arg_count; i++) {
                            if (i > 0) iron_strbuf_appendf(sb, ", ");
                            emit_expr(sb, call->args[i], ctx);
                        }
                    }
                    iron_strbuf_appendf(sb, "}");
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

            /* AUTO-STATIC CHECK: If the receiver is a type name (IRON_SYM_TYPE),
             * emit a direct C function call without self pointer.
             * Convention: Iron_<lowercase_type>_<method>(args...)
             * This matches the stdlib C API: Iron_math_sin, Iron_io_read_file, etc. */
            if (mc->object->kind == IRON_NODE_IDENT) {
                Iron_Ident *obj_id_s = (Iron_Ident *)mc->object;
                if (obj_id_s->resolved_sym &&
                    obj_id_s->resolved_sym->sym_kind == IRON_SYM_TYPE) {
                    const char *tname = obj_id_s->name;
                    size_t tlen = strlen(tname);
                    char *lower = (char *)iron_arena_alloc(ctx->arena, tlen + 1,
                                                           _Alignof(char));
                    for (size_t k = 0; k < tlen; k++) {
                        lower[k] = (char)tolower((unsigned char)tname[k]);
                    }
                    lower[tlen] = '\0';
                    iron_strbuf_appendf(sb, "Iron_%s_%s(", lower, mc->method);
                    for (int i = 0; i < mc->arg_count; i++) {
                        if (i > 0) iron_strbuf_appendf(sb, ", ");
                        emit_expr(sb, mc->args[i], ctx);
                    }
                    iron_strbuf_appendf(sb, ")");
                    break;
                }
            }

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

            /* AUTO-STATIC FIELD: If the receiver is a type name (IRON_SYM_TYPE),
             * emit the constant using convention: IRON_<UPPER(field)>
             * e.g. Math.PI -> IRON_PI, Math.TAU -> IRON_TAU, Math.E -> IRON_E */
            if (fa->object->kind == IRON_NODE_IDENT) {
                Iron_Ident *obj_id_f = (Iron_Ident *)fa->object;
                if (obj_id_f->resolved_sym &&
                    obj_id_f->resolved_sym->sym_kind == IRON_SYM_TYPE) {
                    const char *fname = fa->field;
                    size_t flen = strlen(fname);
                    char *upper = (char *)iron_arena_alloc(ctx->arena, flen + 1,
                                                           _Alignof(char));
                    for (size_t k = 0; k < flen; k++) {
                        upper[k] = (char)toupper((unsigned char)fname[k]);
                    }
                    upper[flen] = '\0';
                    iron_strbuf_appendf(sb, "IRON_%s", upper);
                    break;
                }
            }

            emit_expr(sb, fa->object, ctx);

            /* Determine whether to use -> or . based on receiver type.
             * Methods receive self as T* (pointer), so field access is ->
             * For struct values (stack objects), use .
             * Heuristic: if the object is "self" identifier, always use ->
             * because methods declare self as T* (per emit_method_impl). */
            bool use_arrow = false;
            if (fa->object->kind == IRON_NODE_IDENT) {
                Iron_Ident *id = (Iron_Ident *)fa->object;
                if (strcmp(id->name, "self") == 0) {
                    use_arrow = true;
                }
            }
            iron_strbuf_appendf(sb, "%s%s", use_arrow ? "->" : ".", fa->field);
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
            Iron_AwaitExpr *ae = (Iron_AwaitExpr *)node;
            /* Emit handle_wait call */
            iron_strbuf_appendf(sb, "Iron_handle_wait(");
            if (ae->handle) {
                emit_expr(sb, ae->handle, ctx);
            }
            iron_strbuf_appendf(sb, ")");
            break;
        }

        case IRON_NODE_LAMBDA: {
            /* Lift lambda to named C function; emit use-site expression.
             * In expression context we don't know the enclosing function name.
             * Use "anon" as fallback; statement emission passes proper name. */
            emit_lambda(sb, node, ctx, "anon");
            break;
        }

        default:
            /* Unknown node in expression context */
            iron_strbuf_appendf(sb, "0 /* unsupported expr %d */",
                                (int)node->kind);
            break;
    }
}
