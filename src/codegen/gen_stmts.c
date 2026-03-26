/* gen_stmts.c — Statement emission for the Iron C code generator.
 *
 * Implements:
 *   emit_stmt()    — emit a single statement node
 *   emit_block()   — emit a block with defer drain at exit
 *   emit_defers()  — emit deferred expressions in reverse order
 */

#include "codegen/codegen.h"
#include "lexer/lexer.h"

#include <stdio.h>
#include <string.h>

/* Forward declarations for mutual recursion */
static void emit_func_params(Iron_StrBuf *sb, Iron_Node **params,
                              int param_count, Iron_Codegen *ctx);

/* ── emit_defers ──────────────────────────────────────────────────────────── */

void emit_defers(Iron_StrBuf *sb, Iron_Codegen *ctx, int target_depth) {
    for (int d = ctx->defer_depth - 1; d >= target_depth; d--) {
        int n = (int)arrlen(ctx->defer_stacks[d]);
        for (int i = n - 1; i >= 0; i--) {
            codegen_indent(sb, ctx->indent);
            emit_expr(sb, ctx->defer_stacks[d][i], ctx);
            iron_strbuf_appendf(sb, ";\n");
        }
    }
}

/* ── emit_block ───────────────────────────────────────────────────────────── */

void emit_block(Iron_StrBuf *sb, Iron_Block *block, Iron_Codegen *ctx) {
    /* Push a new defer level */
    int old_depth = ctx->defer_depth;
    ctx->defer_depth++;

    /* Grow defer_stacks array if needed */
    while (arrlen(ctx->defer_stacks) < ctx->defer_depth) {
        Iron_Node **empty = NULL;
        arrput(ctx->defer_stacks, empty);
    }
    /* Reset this level's defer list */
    arrsetlen(ctx->defer_stacks[ctx->defer_depth - 1], 0);

    ctx->indent++;
    for (int i = 0; i < block->stmt_count; i++) {
        emit_stmt(sb, block->stmts[i], ctx);
    }

    /* Drain defers at block exit */
    emit_defers(sb, ctx, ctx->defer_depth - 1);

    ctx->indent--;
    ctx->defer_depth = old_depth;
}

/* ── emit_stmt ────────────────────────────────────────────────────────────── */

void emit_stmt(Iron_StrBuf *sb, Iron_Node *node, Iron_Codegen *ctx) {
    if (!node) return;

    switch (node->kind) {

        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            codegen_indent(sb, ctx->indent);
            const char *c_type = vd->declared_type
                                     ? iron_type_to_c(vd->declared_type, ctx)
                                     : "int64_t";  /* fallback */
            /* Nullable val decls */
            if (vd->declared_type &&
                vd->declared_type->kind == IRON_TYPE_NULLABLE) {
                iron_strbuf_appendf(sb, "const %s %s", c_type,
                                    vd->name);
                if (vd->init) {
                    iron_strbuf_appendf(sb, " = ");
                    emit_expr(sb, vd->init, ctx);
                } else {
                    iron_strbuf_appendf(sb, " = { .has_value = false }");
                }
                iron_strbuf_appendf(sb, ";\n");
            } else {
                iron_strbuf_appendf(sb, "const %s %s", c_type, vd->name);
                if (vd->init) {
                    iron_strbuf_appendf(sb, " = ");
                    emit_expr(sb, vd->init, ctx);
                }
                iron_strbuf_appendf(sb, ";\n");
            }
            break;
        }

        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            codegen_indent(sb, ctx->indent);
            const char *c_type = vd->declared_type
                                     ? iron_type_to_c(vd->declared_type, ctx)
                                     : "int64_t";
            /* Nullable var decls */
            if (vd->declared_type &&
                vd->declared_type->kind == IRON_TYPE_NULLABLE) {
                iron_strbuf_appendf(sb, "%s %s", c_type, vd->name);
                if (vd->init) {
                    iron_strbuf_appendf(sb, " = ");
                    emit_expr(sb, vd->init, ctx);
                } else {
                    iron_strbuf_appendf(sb, " = { .has_value = false }");
                }
                iron_strbuf_appendf(sb, ";\n");
            } else {
                iron_strbuf_appendf(sb, "%s %s", c_type, vd->name);
                if (vd->init) {
                    iron_strbuf_appendf(sb, " = ");
                    emit_expr(sb, vd->init, ctx);
                }
                iron_strbuf_appendf(sb, ";\n");
            }
            break;
        }

        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            codegen_indent(sb, ctx->indent);
            emit_expr(sb, as->target, ctx);
            switch (as->op) {
                case IRON_TOK_ASSIGN:       iron_strbuf_appendf(sb, " = ");   break;
                case IRON_TOK_PLUS_ASSIGN:  iron_strbuf_appendf(sb, " += ");  break;
                case IRON_TOK_MINUS_ASSIGN: iron_strbuf_appendf(sb, " -= ");  break;
                case IRON_TOK_STAR_ASSIGN:  iron_strbuf_appendf(sb, " *= ");  break;
                case IRON_TOK_SLASH_ASSIGN: iron_strbuf_appendf(sb, " /= ");  break;
                default:                    iron_strbuf_appendf(sb, " = ");   break;
            }
            emit_expr(sb, as->value, ctx);
            iron_strbuf_appendf(sb, ";\n");
            break;
        }

        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            /* Emit defers before return */
            emit_defers(sb, ctx, ctx->function_scope_depth);
            codegen_indent(sb, ctx->indent);
            if (rs->value) {
                iron_strbuf_appendf(sb, "return ");
                emit_expr(sb, rs->value, ctx);
                iron_strbuf_appendf(sb, ";\n");
            } else {
                iron_strbuf_appendf(sb, "return;\n");
            }
            break;
        }

        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            codegen_indent(sb, ctx->indent);
            iron_strbuf_appendf(sb, "if (");
            emit_expr(sb, is->condition, ctx);
            iron_strbuf_appendf(sb, ") {\n");
            emit_block(sb, (Iron_Block *)is->body, ctx);
            codegen_indent(sb, ctx->indent);
            iron_strbuf_appendf(sb, "}");
            for (int i = 0; i < is->elif_count; i++) {
                iron_strbuf_appendf(sb, " else if (");
                emit_expr(sb, is->elif_conds[i], ctx);
                iron_strbuf_appendf(sb, ") {\n");
                emit_block(sb, (Iron_Block *)is->elif_bodies[i], ctx);
                codegen_indent(sb, ctx->indent);
                iron_strbuf_appendf(sb, "}");
            }
            if (is->else_body) {
                iron_strbuf_appendf(sb, " else {\n");
                emit_block(sb, (Iron_Block *)is->else_body, ctx);
                codegen_indent(sb, ctx->indent);
                iron_strbuf_appendf(sb, "}");
            }
            iron_strbuf_appendf(sb, "\n");
            break;
        }

        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            codegen_indent(sb, ctx->indent);
            iron_strbuf_appendf(sb, "while (");
            emit_expr(sb, ws->condition, ctx);
            iron_strbuf_appendf(sb, ") {\n");
            emit_block(sb, (Iron_Block *)ws->body, ctx);
            codegen_indent(sb, ctx->indent);
            iron_strbuf_appendf(sb, "}\n");
            break;
        }

        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            /* Range-based for — iterable is a range expression */
            /* For now, emit as: for (int64_t var = 0; var < iterable; var++) */
            codegen_indent(sb, ctx->indent);
            if (!fs->is_parallel) {
                iron_strbuf_appendf(sb, "for (int64_t %s = 0; %s < ",
                                    fs->var_name, fs->var_name);
                emit_expr(sb, fs->iterable, ctx);
                iron_strbuf_appendf(sb, "; %s++) {\n", fs->var_name);
                emit_block(sb, (Iron_Block *)fs->body, ctx);
                codegen_indent(sb, ctx->indent);
                iron_strbuf_appendf(sb, "}\n");
            } else {
                /* Parallel for: stub — emit as sequential for Phase 2 */
                iron_strbuf_appendf(sb, "/* parallel for (stub) */\n");
                codegen_indent(sb, ctx->indent);
                iron_strbuf_appendf(sb, "for (int64_t %s = 0; %s < ",
                                    fs->var_name, fs->var_name);
                emit_expr(sb, fs->iterable, ctx);
                iron_strbuf_appendf(sb, "; %s++) {\n", fs->var_name);
                emit_block(sb, (Iron_Block *)fs->body, ctx);
                codegen_indent(sb, ctx->indent);
                iron_strbuf_appendf(sb, "}\n");
            }
            break;
        }

        case IRON_NODE_MATCH: {
            Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
            /* Emit as if/else if chain */
            for (int i = 0; i < ms->case_count; i++) {
                Iron_MatchCase *mc = (Iron_MatchCase *)ms->cases[i];
                codegen_indent(sb, ctx->indent);
                if (i == 0) {
                    iron_strbuf_appendf(sb, "if (");
                } else {
                    iron_strbuf_appendf(sb, "else if (");
                }
                emit_expr(sb, ms->subject, ctx);
                iron_strbuf_appendf(sb, " == ");
                emit_expr(sb, mc->pattern, ctx);
                iron_strbuf_appendf(sb, ") {\n");
                emit_block(sb, (Iron_Block *)mc->body, ctx);
                codegen_indent(sb, ctx->indent);
                iron_strbuf_appendf(sb, "}");
                if (i < ms->case_count - 1 || ms->else_body) {
                    iron_strbuf_appendf(sb, " ");
                } else {
                    iron_strbuf_appendf(sb, "\n");
                }
            }
            if (ms->else_body) {
                iron_strbuf_appendf(sb, "else {\n");
                emit_block(sb, (Iron_Block *)ms->else_body, ctx);
                codegen_indent(sb, ctx->indent);
                iron_strbuf_appendf(sb, "}\n");
            }
            break;
        }

        case IRON_NODE_DEFER: {
            Iron_DeferStmt *ds = (Iron_DeferStmt *)node;
            /* Push defer expr onto current level's stack */
            if (ctx->defer_depth > 0) {
                /* Grow defer_stacks if needed */
                while (arrlen(ctx->defer_stacks) < ctx->defer_depth) {
                    Iron_Node **empty = NULL;
                    arrput(ctx->defer_stacks, empty);
                }
                arrput(ctx->defer_stacks[ctx->defer_depth - 1], ds->expr);
            }
            /* Emit nothing — defers are drained later */
            break;
        }

        case IRON_NODE_FREE: {
            Iron_FreeStmt *fs = (Iron_FreeStmt *)node;
            codegen_indent(sb, ctx->indent);
            iron_strbuf_appendf(sb, "free(");
            emit_expr(sb, fs->expr, ctx);
            iron_strbuf_appendf(sb, ");\n");
            break;
        }

        case IRON_NODE_LEAK: {
            /* Semantic annotation — emit nothing */
            break;
        }

        case IRON_NODE_SPAWN: {
            /* Stub for Phase 3 concurrency */
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)node;
            codegen_indent(sb, ctx->indent);
            iron_strbuf_appendf(sb, "/* spawn %s (stub) */\n",
                                ss->name ? ss->name : "");
            break;
        }

        case IRON_NODE_BLOCK: {
            /* Nested block */
            Iron_Block *blk = (Iron_Block *)node;
            codegen_indent(sb, ctx->indent);
            iron_strbuf_appendf(sb, "{\n");
            emit_block(sb, blk, ctx);
            codegen_indent(sb, ctx->indent);
            iron_strbuf_appendf(sb, "}\n");
            break;
        }

        default:
            /* Expression statement — try to emit as expression */
            codegen_indent(sb, ctx->indent);
            emit_expr(sb, node, ctx);
            iron_strbuf_appendf(sb, ";\n");
            break;
    }
}

/* ── Function/method parameter list emission ─────────────────────────────── */

static void emit_func_params(Iron_StrBuf *sb, Iron_Node **params,
                              int param_count, Iron_Codegen *ctx) {
    for (int i = 0; i < param_count; i++) {
        if (i > 0) iron_strbuf_appendf(sb, ", ");
        Iron_Param *p = (Iron_Param *)params[i];
        /* Look up the param's resolved type via the symbol table */
        Iron_Symbol *sym = iron_scope_lookup(ctx->global_scope, p->name);
        const char *c_type = "int64_t";  /* default fallback */
        if (sym && sym->type) {
            c_type = iron_type_to_c(sym->type, ctx);
        } else if (p->type_ann) {
            /* Fallback: use type annotation name directly */
            Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)p->type_ann;
            (void)ta;
            /* We can't resolve it without a scope, use fallback */
        }
        iron_strbuf_appendf(sb, "%s %s", c_type, p->name);
    }
}

/* ── Function prototype/implementation emission ───────────────────────────── */

void emit_func_prototype(Iron_Codegen *ctx, Iron_FuncDecl *fd) {
    Iron_StrBuf *sb = &ctx->prototypes;
    const char *ret_type = "void";
    if (fd->resolved_return_type) {
        ret_type = iron_type_to_c(fd->resolved_return_type, ctx);
    }
    const char *mangled = iron_mangle_name(fd->name, ctx->arena);
    iron_strbuf_appendf(sb, "%s %s(", ret_type, mangled);
    emit_func_params(sb, fd->params, fd->param_count, ctx);
    iron_strbuf_appendf(sb, ");\n");
}

void emit_func_impl(Iron_Codegen *ctx, Iron_FuncDecl *fd) {
    Iron_StrBuf *sb = &ctx->implementations;
    const char *ret_type = "void";
    if (fd->resolved_return_type) {
        ret_type = iron_type_to_c(fd->resolved_return_type, ctx);
    }
    const char *mangled = iron_mangle_name(fd->name, ctx->arena);
    iron_strbuf_appendf(sb, "%s %s(", ret_type, mangled);
    emit_func_params(sb, fd->params, fd->param_count, ctx);
    iron_strbuf_appendf(sb, ") {\n");

    /* Set up defer tracking for this function */
    int saved_fn_depth = ctx->function_scope_depth;
    ctx->function_scope_depth = ctx->defer_depth;

    if (fd->body) {
        emit_block(sb, (Iron_Block *)fd->body, ctx);
    }

    ctx->function_scope_depth = saved_fn_depth;
    iron_strbuf_appendf(sb, "}\n\n");
}

void emit_method_prototype(Iron_Codegen *ctx, Iron_MethodDecl *md) {
    Iron_StrBuf *sb = &ctx->prototypes;
    const char *ret_type = "void";
    if (md->resolved_return_type) {
        ret_type = iron_type_to_c(md->resolved_return_type, ctx);
    }
    const char *mangled = iron_mangle_method(md->type_name, md->method_name,
                                              ctx->arena);
    const char *self_type = iron_mangle_name(md->type_name, ctx->arena);
    iron_strbuf_appendf(sb, "%s %s(%s* self", ret_type, mangled, self_type);
    if (md->param_count > 0) {
        iron_strbuf_appendf(sb, ", ");
        emit_func_params(sb, md->params, md->param_count, ctx);
    }
    iron_strbuf_appendf(sb, ");\n");
}

void emit_method_impl(Iron_Codegen *ctx, Iron_MethodDecl *md) {
    Iron_StrBuf *sb = &ctx->implementations;
    const char *ret_type = "void";
    if (md->resolved_return_type) {
        ret_type = iron_type_to_c(md->resolved_return_type, ctx);
    }
    const char *mangled = iron_mangle_method(md->type_name, md->method_name,
                                              ctx->arena);
    const char *self_type = iron_mangle_name(md->type_name, ctx->arena);
    iron_strbuf_appendf(sb, "%s %s(%s* self", ret_type, mangled, self_type);
    if (md->param_count > 0) {
        iron_strbuf_appendf(sb, ", ");
        emit_func_params(sb, md->params, md->param_count, ctx);
    }
    iron_strbuf_appendf(sb, ") {\n");

    int saved_fn_depth = ctx->function_scope_depth;
    ctx->function_scope_depth = ctx->defer_depth;

    if (md->body) {
        emit_block(sb, (Iron_Block *)md->body, ctx);
    }

    ctx->function_scope_depth = saved_fn_depth;
    iron_strbuf_appendf(sb, "}\n\n");
}
