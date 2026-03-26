/* comptime.c — Tree-walking AST interpreter for Iron compile-time evaluation.
 *
 * Implements:
 *   iron_comptime_eval_expr()  — evaluate a single expression node
 *   iron_comptime_val_to_ast() — convert comptime value back to AST literal
 *   iron_comptime_apply()      — replace all IRON_NODE_COMPTIME nodes in program
 */

#include "comptime/comptime.h"
#include "lexer/lexer.h"
#include "analyzer/types.h"
#include "util/arena.h"
#include "util/strbuf.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

static Iron_ComptimeVal *cval_alloc(Iron_ComptimeCtx *ctx,
                                     Iron_ComptimeValKind kind) {
    Iron_ComptimeVal *v = iron_arena_alloc(ctx->arena,
                                            sizeof(Iron_ComptimeVal),
                                            _Alignof(Iron_ComptimeVal));
    v->kind = kind;
    return v;
}

static Iron_ComptimeVal *cval_int(Iron_ComptimeCtx *ctx, int64_t n) {
    Iron_ComptimeVal *v = cval_alloc(ctx, IRON_CVAL_INT);
    v->as_int = n;
    return v;
}

static Iron_ComptimeVal *cval_float(Iron_ComptimeCtx *ctx, double d) {
    Iron_ComptimeVal *v = cval_alloc(ctx, IRON_CVAL_FLOAT);
    v->as_float = d;
    return v;
}

static Iron_ComptimeVal *cval_bool(Iron_ComptimeCtx *ctx, bool b) {
    Iron_ComptimeVal *v = cval_alloc(ctx, IRON_CVAL_BOOL);
    v->as_bool = b;
    return v;
}

static Iron_ComptimeVal *cval_string(Iron_ComptimeCtx *ctx,
                                      const char *data, size_t len) {
    Iron_ComptimeVal *v = cval_alloc(ctx, IRON_CVAL_STRING);
    v->as_string.data = data;
    v->as_string.len  = len;
    return v;
}

static Iron_ComptimeVal *cval_null(Iron_ComptimeCtx *ctx) {
    return cval_alloc(ctx, IRON_CVAL_NULL);
}

/* ── Local scope management ──────────────────────────────────────────────── */

static void push_frame(Iron_ComptimeCtx *ctx) {
    Iron_ComptimeBinding *frame = NULL;  /* empty stb_ds hashmap */
    sh_new_strdup(frame);
    arrput(ctx->local_frames, frame);
    ctx->frame_depth++;
}

static void pop_frame(Iron_ComptimeCtx *ctx) {
    if (ctx->frame_depth <= 0) return;
    Iron_ComptimeBinding *frame = ctx->local_frames[ctx->frame_depth - 1];
    shfree(frame);
    arrsetlen(ctx->local_frames, ctx->frame_depth - 1);
    ctx->frame_depth--;
}

static void bind_local(Iron_ComptimeCtx *ctx, const char *name,
                        Iron_ComptimeVal *val) {
    if (ctx->frame_depth <= 0) return;
    Iron_ComptimeBinding *frame = ctx->local_frames[ctx->frame_depth - 1];
    shput(frame, name, val);
    /* Update the frame pointer back (stb_ds may realloc) */
    ctx->local_frames[ctx->frame_depth - 1] = frame;
}

static Iron_ComptimeVal *lookup_local(Iron_ComptimeCtx *ctx,
                                       const char *name) {
    /* Search from innermost frame outward */
    for (int i = ctx->frame_depth - 1; i >= 0; i--) {
        Iron_ComptimeBinding *frame = ctx->local_frames[i];
        int idx = shgeti(frame, name);
        if (idx >= 0) {
            return frame[idx].value;
        }
    }
    return NULL;
}

/* ── Error helpers ───────────────────────────────────────────────────────── */

/* Build a call trace hint string from the call stack. */
static const char *build_call_trace(Iron_ComptimeCtx *ctx, Iron_Arena *arena) {
    if (ctx->call_depth == 0) return NULL;

    Iron_StrBuf sb = iron_strbuf_create(256);
    iron_strbuf_appendf(&sb, "call trace:\n");
    for (int i = 0; i < ctx->call_depth; i++) {
        iron_strbuf_appendf(&sb, "  [%d] %s\n", i, ctx->call_stack[i]);
    }
    const char *result = iron_arena_strdup(arena, sb.data, sb.len);
    iron_strbuf_free(&sb);
    return result;
}

static void emit_error(Iron_ComptimeCtx *ctx, int code, Iron_Span span,
                        const char *message) {
    if (ctx->had_error) return;  /* only emit first error per comptime expr */
    ctx->had_error = true;
    const char *hint = build_call_trace(ctx, ctx->arena);
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR, code, span,
                   message, hint);
}

/* ── Statement evaluator (forward declaration) ───────────────────────────── */

static void eval_stmts(Iron_ComptimeCtx *ctx, Iron_Node **stmts, int count);
static void eval_stmt(Iron_ComptimeCtx *ctx, Iron_Node *node);

/* ── Expression evaluator ────────────────────────────────────────────────── */

Iron_ComptimeVal *iron_comptime_eval_expr(Iron_ComptimeCtx *ctx,
                                           Iron_Node *node) {
    if (!node || ctx->had_error) return cval_null(ctx);

    /* Step counting for non-literal nodes */
    ctx->steps++;
    if (ctx->steps > ctx->step_limit) {
        Iron_Span span = node->span;
        emit_error(ctx, IRON_ERR_COMPTIME_STEP_LIMIT, span,
                   "comptime evaluation exceeded step limit (1000000 steps)");
        return cval_null(ctx);
    }

    switch (node->kind) {

        /* ── Literals ───────────────────────────────────────────────────── */

        case IRON_NODE_INT_LIT: {
            Iron_IntLit *lit = (Iron_IntLit *)node;
            int64_t v = 0;
            if (lit->value) {
                v = (int64_t)strtoll(lit->value, NULL, 10);
            }
            return cval_int(ctx, v);
        }

        case IRON_NODE_FLOAT_LIT: {
            Iron_FloatLit *lit = (Iron_FloatLit *)node;
            double v = 0.0;
            if (lit->value) {
                v = strtod(lit->value, NULL);
            }
            return cval_float(ctx, v);
        }

        case IRON_NODE_BOOL_LIT: {
            Iron_BoolLit *lit = (Iron_BoolLit *)node;
            return cval_bool(ctx, lit->value);
        }

        case IRON_NODE_STRING_LIT: {
            Iron_StringLit *lit = (Iron_StringLit *)node;
            const char *s = lit->value ? lit->value : "";
            return cval_string(ctx, s, strlen(s));
        }

        case IRON_NODE_NULL_LIT: {
            return cval_null(ctx);
        }

        /* ── Identifier (variable lookup) ───────────────────────────────── */

        case IRON_NODE_IDENT: {
            Iron_Ident *id = (Iron_Ident *)node;
            Iron_ComptimeVal *val = lookup_local(ctx, id->name);
            if (val) return val;
            /* Could be a global constant — not supported in basic comptime */
            emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                       "comptime: unresolved identifier in comptime context");
            return cval_null(ctx);
        }

        /* ── Binary expression ──────────────────────────────────────────── */

        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *bin = (Iron_BinaryExpr *)node;

            /* Short-circuit for logical operators */
            if (bin->op == IRON_TOK_AND) {
                Iron_ComptimeVal *lv = iron_comptime_eval_expr(ctx, bin->left);
                if (ctx->had_error) return cval_null(ctx);
                if (lv->kind == IRON_CVAL_BOOL && !lv->as_bool) {
                    return cval_bool(ctx, false);
                }
                Iron_ComptimeVal *rv = iron_comptime_eval_expr(ctx, bin->right);
                if (ctx->had_error) return cval_null(ctx);
                bool result = (lv->kind == IRON_CVAL_BOOL ? lv->as_bool : false) &&
                              (rv->kind == IRON_CVAL_BOOL ? rv->as_bool : false);
                return cval_bool(ctx, result);
            }
            if (bin->op == IRON_TOK_OR) {
                Iron_ComptimeVal *lv = iron_comptime_eval_expr(ctx, bin->left);
                if (ctx->had_error) return cval_null(ctx);
                if (lv->kind == IRON_CVAL_BOOL && lv->as_bool) {
                    return cval_bool(ctx, true);
                }
                Iron_ComptimeVal *rv = iron_comptime_eval_expr(ctx, bin->right);
                if (ctx->had_error) return cval_null(ctx);
                bool result = (lv->kind == IRON_CVAL_BOOL ? lv->as_bool : false) ||
                              (rv->kind == IRON_CVAL_BOOL ? rv->as_bool : false);
                return cval_bool(ctx, result);
            }

            Iron_ComptimeVal *lv = iron_comptime_eval_expr(ctx, bin->left);
            if (ctx->had_error) return cval_null(ctx);
            Iron_ComptimeVal *rv = iron_comptime_eval_expr(ctx, bin->right);
            if (ctx->had_error) return cval_null(ctx);

            /* Integer arithmetic */
            if (lv->kind == IRON_CVAL_INT && rv->kind == IRON_CVAL_INT) {
                switch (bin->op) {
                    case IRON_TOK_PLUS:       return cval_int(ctx, lv->as_int + rv->as_int);
                    case IRON_TOK_MINUS:      return cval_int(ctx, lv->as_int - rv->as_int);
                    case IRON_TOK_STAR:       return cval_int(ctx, lv->as_int * rv->as_int);
                    case IRON_TOK_SLASH:
                        if (rv->as_int == 0) {
                            emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                                       "comptime: division by zero");
                            return cval_null(ctx);
                        }
                        return cval_int(ctx, lv->as_int / rv->as_int);
                    case IRON_TOK_PERCENT:
                        if (rv->as_int == 0) {
                            emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                                       "comptime: modulo by zero");
                            return cval_null(ctx);
                        }
                        return cval_int(ctx, lv->as_int % rv->as_int);
                    case IRON_TOK_EQUALS:     return cval_bool(ctx, lv->as_int == rv->as_int);
                    case IRON_TOK_NOT_EQUALS: return cval_bool(ctx, lv->as_int != rv->as_int);
                    case IRON_TOK_LESS:       return cval_bool(ctx, lv->as_int < rv->as_int);
                    case IRON_TOK_GREATER:    return cval_bool(ctx, lv->as_int > rv->as_int);
                    case IRON_TOK_LESS_EQ:    return cval_bool(ctx, lv->as_int <= rv->as_int);
                    case IRON_TOK_GREATER_EQ: return cval_bool(ctx, lv->as_int >= rv->as_int);
                    default: break;
                }
            }

            /* Float arithmetic */
            if (lv->kind == IRON_CVAL_FLOAT || rv->kind == IRON_CVAL_FLOAT) {
                double l = (lv->kind == IRON_CVAL_FLOAT) ? lv->as_float : (double)lv->as_int;
                double r = (rv->kind == IRON_CVAL_FLOAT) ? rv->as_float : (double)rv->as_int;
                switch (bin->op) {
                    case IRON_TOK_PLUS:       return cval_float(ctx, l + r);
                    case IRON_TOK_MINUS:      return cval_float(ctx, l - r);
                    case IRON_TOK_STAR:       return cval_float(ctx, l * r);
                    case IRON_TOK_SLASH:
                        if (r == 0.0) {
                            emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                                       "comptime: division by zero");
                            return cval_null(ctx);
                        }
                        return cval_float(ctx, l / r);
                    case IRON_TOK_EQUALS:     return cval_bool(ctx, l == r);
                    case IRON_TOK_NOT_EQUALS: return cval_bool(ctx, l != r);
                    case IRON_TOK_LESS:       return cval_bool(ctx, l < r);
                    case IRON_TOK_GREATER:    return cval_bool(ctx, l > r);
                    case IRON_TOK_LESS_EQ:    return cval_bool(ctx, l <= r);
                    case IRON_TOK_GREATER_EQ: return cval_bool(ctx, l >= r);
                    default: break;
                }
            }

            /* Bool comparisons */
            if (lv->kind == IRON_CVAL_BOOL && rv->kind == IRON_CVAL_BOOL) {
                switch (bin->op) {
                    case IRON_TOK_EQUALS:     return cval_bool(ctx, lv->as_bool == rv->as_bool);
                    case IRON_TOK_NOT_EQUALS: return cval_bool(ctx, lv->as_bool != rv->as_bool);
                    default: break;
                }
            }

            emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                       "comptime: unsupported binary operation");
            return cval_null(ctx);
        }

        /* ── Unary expression ───────────────────────────────────────────── */

        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *un = (Iron_UnaryExpr *)node;
            Iron_ComptimeVal *operand = iron_comptime_eval_expr(ctx, un->operand);
            if (ctx->had_error) return cval_null(ctx);

            if (un->op == IRON_TOK_MINUS) {
                if (operand->kind == IRON_CVAL_INT)
                    return cval_int(ctx, -operand->as_int);
                if (operand->kind == IRON_CVAL_FLOAT)
                    return cval_float(ctx, -operand->as_float);
            }
            if (un->op == IRON_TOK_NOT) {
                if (operand->kind == IRON_CVAL_BOOL)
                    return cval_bool(ctx, !operand->as_bool);
            }

            emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                       "comptime: unsupported unary operation");
            return cval_null(ctx);
        }

        /* ── Function call ──────────────────────────────────────────────── */

        case IRON_NODE_CALL: {
            Iron_CallExpr *call = (Iron_CallExpr *)node;

            /* Get function name from callee ident */
            if (call->callee->kind != IRON_NODE_IDENT) {
                emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                           "comptime: only named function calls are supported");
                return cval_null(ctx);
            }
            Iron_Ident *callee_id = (Iron_Ident *)call->callee;
            const char *func_name = callee_id->name;

            /* Look up function in global scope */
            Iron_Symbol *sym = iron_scope_lookup(ctx->global_scope, func_name);
            if (!sym || sym->sym_kind != IRON_SYM_FUNCTION) {
                emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                           "comptime: function not found in global scope");
                return cval_null(ctx);
            }

            Iron_FuncDecl *fn = (Iron_FuncDecl *)sym->decl_node;
            if (!fn || fn->kind != IRON_NODE_FUNC_DECL || !fn->body) {
                emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                           "comptime: function has no body (cannot evaluate)");
                return cval_null(ctx);
            }

            /* Step + depth check */
            ctx->steps++;
            if (ctx->steps > ctx->step_limit) {
                emit_error(ctx, IRON_ERR_COMPTIME_STEP_LIMIT, node->span,
                           "comptime evaluation exceeded step limit (1000000 steps)");
                return cval_null(ctx);
            }

            /* Evaluate arguments before pushing frame */
            Iron_ComptimeVal **arg_vals = NULL;
            for (int i = 0; i < call->arg_count; i++) {
                Iron_ComptimeVal *av = iron_comptime_eval_expr(ctx, call->args[i]);
                if (ctx->had_error) return cval_null(ctx);
                arrput(arg_vals, av);
            }

            /* Push call stack entry for error tracing */
            arrput(ctx->call_stack, func_name);
            arrput(ctx->call_spans, node->span);
            ctx->call_depth++;

            /* Push local frame and bind parameters */
            push_frame(ctx);
            for (int i = 0; i < fn->param_count && i < call->arg_count; i++) {
                Iron_Param *p = (Iron_Param *)fn->params[i];
                bind_local(ctx, p->name, arg_vals[i]);
            }
            arrfree(arg_vals);

            /* Reset return state */
            bool outer_had_return = ctx->had_return;
            Iron_ComptimeVal *outer_return_val = ctx->return_val;
            ctx->had_return  = false;
            ctx->return_val  = NULL;

            /* Evaluate function body */
            eval_stmt(ctx, fn->body);

            /* Collect return value */
            Iron_ComptimeVal *result = ctx->return_val
                                       ? ctx->return_val
                                       : cval_null(ctx);

            /* Restore outer return state */
            ctx->had_return  = outer_had_return;
            ctx->return_val  = outer_return_val;

            /* Pop call frame and call stack */
            pop_frame(ctx);
            arrsetlen(ctx->call_stack, ctx->call_depth - 1);
            arrsetlen(ctx->call_spans, ctx->call_depth - 1);
            ctx->call_depth--;

            return result;
        }

        /* ── Array literal ──────────────────────────────────────────────── */

        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)node;
            Iron_ComptimeVal *v = cval_alloc(ctx, IRON_CVAL_ARRAY);
            v->as_array.count = al->element_count;
            v->as_array.elems = iron_arena_alloc(ctx->arena,
                (size_t)al->element_count * sizeof(Iron_ComptimeVal *),
                _Alignof(Iron_ComptimeVal *));
            for (int i = 0; i < al->element_count; i++) {
                v->as_array.elems[i] = iron_comptime_eval_expr(ctx,
                                                                 al->elements[i]);
                if (ctx->had_error) return cval_null(ctx);
            }
            return v;
        }

        /* ── Object construction ────────────────────────────────────────── */

        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;
            Iron_ComptimeVal *v = cval_alloc(ctx, IRON_CVAL_STRUCT);
            v->as_struct.type_name = ce->type_name;
            v->as_struct.field_count = ce->arg_count;
            v->as_struct.field_names = iron_arena_alloc(ctx->arena,
                (size_t)ce->arg_count * sizeof(const char *),
                _Alignof(const char *));
            v->as_struct.field_vals = iron_arena_alloc(ctx->arena,
                (size_t)ce->arg_count * sizeof(Iron_ComptimeVal *),
                _Alignof(Iron_ComptimeVal *));
            /* Look up field names from global scope */
            Iron_Symbol *sym = iron_scope_lookup(ctx->global_scope,
                                                  ce->type_name);
            for (int i = 0; i < ce->arg_count; i++) {
                if (sym && sym->decl_node &&
                    sym->decl_node->kind == IRON_NODE_OBJECT_DECL) {
                    Iron_ObjectDecl *od = (Iron_ObjectDecl *)sym->decl_node;
                    if (i < od->field_count) {
                        Iron_Field *f = (Iron_Field *)od->fields[i];
                        v->as_struct.field_names[i] = f->name;
                    } else {
                        v->as_struct.field_names[i] = "(unknown)";
                    }
                } else {
                    v->as_struct.field_names[i] = "(unknown)";
                }
                v->as_struct.field_vals[i] = iron_comptime_eval_expr(ctx,
                                                                        ce->args[i]);
                if (ctx->had_error) return cval_null(ctx);
            }
            return v;
        }

        /* ── Restriction: heap/rc not allowed ────────────────────────────── */

        case IRON_NODE_HEAP: {
            emit_error(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                       "comptime: heap allocation is not allowed in comptime context");
            return cval_null(ctx);
        }

        case IRON_NODE_RC: {
            emit_error(ctx, IRON_ERR_COMPTIME_RESTRICTION, node->span,
                       "comptime: rc allocation is not allowed in comptime context");
            return cval_null(ctx);
        }

        /* ── Nested comptime (valid but redundant) ───────────────────────── */

        case IRON_NODE_COMPTIME: {
            Iron_ComptimeExpr *ce = (Iron_ComptimeExpr *)node;
            return iron_comptime_eval_expr(ctx, ce->inner);
        }

        default:
            emit_error(ctx, IRON_ERR_COMPTIME_ERROR, node->span,
                       "comptime: unsupported expression kind in comptime context");
            return cval_null(ctx);
    }
}

/* ── Statement evaluator ─────────────────────────────────────────────────── */

static void eval_stmt(Iron_ComptimeCtx *ctx, Iron_Node *node) {
    if (!node || ctx->had_error || ctx->had_return) return;

    switch (node->kind) {

        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            push_frame(ctx);
            eval_stmts(ctx, blk->stmts, blk->stmt_count);
            pop_frame(ctx);
            break;
        }

        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            Iron_ComptimeVal *val = vd->init
                ? iron_comptime_eval_expr(ctx, vd->init)
                : cval_null(ctx);
            if (!ctx->had_error) {
                bind_local(ctx, vd->name, val);
            }
            break;
        }

        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            Iron_ComptimeVal *val = vd->init
                ? iron_comptime_eval_expr(ctx, vd->init)
                : cval_null(ctx);
            if (!ctx->had_error) {
                bind_local(ctx, vd->name, val);
            }
            break;
        }

        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            Iron_ComptimeVal *val = iron_comptime_eval_expr(ctx, as->value);
            if (ctx->had_error) break;
            if (as->target->kind == IRON_NODE_IDENT) {
                Iron_Ident *id = (Iron_Ident *)as->target;
                /* Update binding in the innermost frame that has this name */
                for (int i = ctx->frame_depth - 1; i >= 0; i--) {
                    Iron_ComptimeBinding *frame = ctx->local_frames[i];
                    int idx = shgeti(frame, id->name);
                    if (idx >= 0) {
                        frame[idx].value = val;
                        ctx->local_frames[i] = frame;
                        break;
                    }
                }
            }
            break;
        }

        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            if (rs->value) {
                ctx->return_val = iron_comptime_eval_expr(ctx, rs->value);
            } else {
                ctx->return_val = cval_null(ctx);
            }
            ctx->had_return = true;
            break;
        }

        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            Iron_ComptimeVal *cond = iron_comptime_eval_expr(ctx, is->condition);
            if (ctx->had_error) break;

            bool taken = (cond->kind == IRON_CVAL_BOOL) ? cond->as_bool :
                         (cond->kind == IRON_CVAL_INT)  ? (cond->as_int != 0) :
                         false;
            if (taken) {
                eval_stmt(ctx, is->body);
            } else {
                /* Check elif branches */
                bool elif_taken = false;
                for (int i = 0; i < is->elif_count && !elif_taken; i++) {
                    Iron_ComptimeVal *ec = iron_comptime_eval_expr(ctx,
                                                                    is->elif_conds[i]);
                    if (ctx->had_error) break;
                    bool etaken = (ec->kind == IRON_CVAL_BOOL) ? ec->as_bool :
                                  (ec->kind == IRON_CVAL_INT)  ? (ec->as_int != 0) :
                                  false;
                    if (etaken) {
                        eval_stmt(ctx, is->elif_bodies[i]);
                        elif_taken = true;
                    }
                }
                if (!elif_taken && is->else_body) {
                    eval_stmt(ctx, is->else_body);
                }
            }
            break;
        }

        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            while (!ctx->had_error && !ctx->had_return) {
                Iron_ComptimeVal *cond = iron_comptime_eval_expr(ctx,
                                                                   ws->condition);
                if (ctx->had_error) break;
                bool keep_going = (cond->kind == IRON_CVAL_BOOL) ? cond->as_bool :
                                  (cond->kind == IRON_CVAL_INT)  ? (cond->as_int != 0) :
                                  false;
                if (!keep_going) break;

                ctx->steps++;
                if (ctx->steps > ctx->step_limit) {
                    emit_error(ctx, IRON_ERR_COMPTIME_STEP_LIMIT, ws->span,
                               "comptime evaluation exceeded step limit (1000000 steps)");
                    break;
                }
                eval_stmt(ctx, ws->body);
            }
            break;
        }

        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            Iron_ComptimeVal *iterable = iron_comptime_eval_expr(ctx, fs->iterable);
            if (ctx->had_error) break;

            if (iterable->kind == IRON_CVAL_INT) {
                /* Range: iterate 0..n */
                int64_t n = iterable->as_int;
                for (int64_t i = 0; i < n && !ctx->had_error && !ctx->had_return; i++) {
                    ctx->steps++;
                    if (ctx->steps > ctx->step_limit) {
                        emit_error(ctx, IRON_ERR_COMPTIME_STEP_LIMIT, fs->span,
                                   "comptime evaluation exceeded step limit (1000000 steps)");
                        break;
                    }
                    push_frame(ctx);
                    bind_local(ctx, fs->var_name, cval_int(ctx, i));
                    eval_stmt(ctx, fs->body);
                    pop_frame(ctx);
                }
            } else if (iterable->kind == IRON_CVAL_ARRAY) {
                for (int i = 0; i < iterable->as_array.count && !ctx->had_error && !ctx->had_return; i++) {
                    ctx->steps++;
                    if (ctx->steps > ctx->step_limit) {
                        emit_error(ctx, IRON_ERR_COMPTIME_STEP_LIMIT, fs->span,
                                   "comptime evaluation exceeded step limit (1000000 steps)");
                        break;
                    }
                    push_frame(ctx);
                    bind_local(ctx, fs->var_name, iterable->as_array.elems[i]);
                    eval_stmt(ctx, fs->body);
                    pop_frame(ctx);
                }
            }
            break;
        }

        default:
            /* Treat as expression statement if it can be evaluated */
            iron_comptime_eval_expr(ctx, node);
            break;
    }
}

static void eval_stmts(Iron_ComptimeCtx *ctx, Iron_Node **stmts, int count) {
    for (int i = 0; i < count && !ctx->had_error && !ctx->had_return; i++) {
        eval_stmt(ctx, stmts[i]);
    }
}

/* ── Value to AST conversion ─────────────────────────────────────────────── */

Iron_Node *iron_comptime_val_to_ast(Iron_ComptimeVal *val, Iron_Arena *arena,
                                     Iron_Span span,
                                     struct Iron_Type *resolved_type) {
    if (!val) return NULL;

    switch (val->kind) {

        case IRON_CVAL_INT: {
            Iron_IntLit *lit = iron_arena_alloc(arena, sizeof(Iron_IntLit),
                                                 _Alignof(Iron_IntLit));
            lit->span          = span;
            lit->kind          = IRON_NODE_INT_LIT;
            lit->resolved_type = resolved_type;
            /* Convert int64 to string */
            char buf[32];
            snprintf(buf, sizeof(buf), "%" PRId64, val->as_int);
            lit->value = iron_arena_strdup(arena, buf, strlen(buf));
            return (Iron_Node *)lit;
        }

        case IRON_CVAL_FLOAT: {
            Iron_FloatLit *lit = iron_arena_alloc(arena, sizeof(Iron_FloatLit),
                                                   _Alignof(Iron_FloatLit));
            lit->span          = span;
            lit->kind          = IRON_NODE_FLOAT_LIT;
            lit->resolved_type = resolved_type;
            char buf[64];
            snprintf(buf, sizeof(buf), "%.17g", val->as_float);
            lit->value = iron_arena_strdup(arena, buf, strlen(buf));
            return (Iron_Node *)lit;
        }

        case IRON_CVAL_BOOL: {
            Iron_BoolLit *lit = iron_arena_alloc(arena, sizeof(Iron_BoolLit),
                                                  _Alignof(Iron_BoolLit));
            lit->span          = span;
            lit->kind          = IRON_NODE_BOOL_LIT;
            lit->resolved_type = resolved_type;
            lit->value         = val->as_bool;
            return (Iron_Node *)lit;
        }

        case IRON_CVAL_STRING: {
            Iron_StringLit *lit = iron_arena_alloc(arena, sizeof(Iron_StringLit),
                                                    _Alignof(Iron_StringLit));
            lit->span          = span;
            lit->kind          = IRON_NODE_STRING_LIT;
            lit->resolved_type = resolved_type;
            lit->value = iron_arena_strdup(arena, val->as_string.data,
                                            val->as_string.len);
            return (Iron_Node *)lit;
        }

        case IRON_CVAL_ARRAY: {
            Iron_ArrayLit *al = iron_arena_alloc(arena, sizeof(Iron_ArrayLit),
                                                  _Alignof(Iron_ArrayLit));
            al->span          = span;
            al->kind          = IRON_NODE_ARRAY_LIT;
            al->resolved_type = resolved_type;
            al->type_ann      = NULL;
            al->size          = NULL;
            al->element_count = val->as_array.count;
            al->elements = iron_arena_alloc(arena,
                (size_t)val->as_array.count * sizeof(Iron_Node *),
                _Alignof(Iron_Node *));
            for (int i = 0; i < val->as_array.count; i++) {
                al->elements[i] = iron_comptime_val_to_ast(
                    val->as_array.elems[i], arena, span, NULL);
            }
            return (Iron_Node *)al;
        }

        case IRON_CVAL_STRUCT:
        case IRON_CVAL_NULL:
        default:
            return NULL;
    }
}

/* ── AST walker for replacing IRON_NODE_COMPTIME nodes ───────────────────── */

/* Context for the replacement walk */
typedef struct {
    Iron_ComptimeCtx *eval_ctx;
    Iron_Arena        *arena;
} ReplaceCtx;

/* Replace COMPTIME nodes in an array of node pointers. */
static void replace_in_node_array(Iron_Node **nodes, int count,
                                   ReplaceCtx *rctx);
static void replace_in_node(Iron_Node **slot, ReplaceCtx *rctx);

static void replace_in_node(Iron_Node **slot, ReplaceCtx *rctx) {
    if (!slot || !*slot) return;
    Iron_Node *node = *slot;

    if (node->kind == IRON_NODE_COMPTIME) {
        Iron_ComptimeExpr *ce = (Iron_ComptimeExpr *)node;
        struct Iron_Type *orig_type = ce->resolved_type;
        Iron_Span orig_span = ce->span;

        /* Reset error state for each comptime expression */
        rctx->eval_ctx->had_error  = false;
        rctx->eval_ctx->had_return = false;
        rctx->eval_ctx->return_val = NULL;
        rctx->eval_ctx->steps      = 0;

        /* Reset call stack */
        arrsetlen(rctx->eval_ctx->call_stack, 0);
        arrsetlen(rctx->eval_ctx->call_spans, 0);
        rctx->eval_ctx->call_depth = 0;

        /* Reset local frames */
        while (rctx->eval_ctx->frame_depth > 0) {
            pop_frame(rctx->eval_ctx);
        }

        Iron_ComptimeVal *val = iron_comptime_eval_expr(rctx->eval_ctx,
                                                         ce->inner);
        if (!rctx->eval_ctx->had_error && val) {
            Iron_Node *replacement = iron_comptime_val_to_ast(val, rctx->arena,
                                                               orig_span,
                                                               orig_type);
            if (replacement) {
                *slot = replacement;
                return;
            }
        }
        /* On error: leave node as-is (error already emitted) */
        return;
    }

    /* Recurse into children */
    switch (node->kind) {

        case IRON_NODE_PROGRAM: {
            Iron_Program *p = (Iron_Program *)node;
            replace_in_node_array(p->decls, p->decl_count, rctx);
            break;
        }
        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *f = (Iron_FuncDecl *)node;
            replace_in_node(&f->body, rctx);
            break;
        }
        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *m = (Iron_MethodDecl *)node;
            replace_in_node(&m->body, rctx);
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)node;
            replace_in_node_array(b->stmts, b->stmt_count, rctx);
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            replace_in_node(&vd->init, rctx);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            replace_in_node(&vd->init, rctx);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            replace_in_node(&as->value, rctx);
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            replace_in_node(&rs->value, rctx);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            replace_in_node(&is->condition, rctx);
            replace_in_node(&is->body, rctx);
            replace_in_node_array(is->elif_conds, is->elif_count, rctx);
            replace_in_node_array(is->elif_bodies, is->elif_count, rctx);
            replace_in_node(&is->else_body, rctx);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            replace_in_node(&ws->condition, rctx);
            replace_in_node(&ws->body, rctx);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            replace_in_node(&fs->iterable, rctx);
            replace_in_node(&fs->body, rctx);
            break;
        }
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *bin = (Iron_BinaryExpr *)node;
            replace_in_node(&bin->left, rctx);
            replace_in_node(&bin->right, rctx);
            break;
        }
        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *un = (Iron_UnaryExpr *)node;
            replace_in_node(&un->operand, rctx);
            break;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *c = (Iron_CallExpr *)node;
            replace_in_node(&c->callee, rctx);
            replace_in_node_array(c->args, c->arg_count, rctx);
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            replace_in_node(&mc->object, rctx);
            replace_in_node_array(mc->args, mc->arg_count, rctx);
            break;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
            replace_in_node(&fa->object, rctx);
            break;
        }
        case IRON_NODE_INDEX: {
            Iron_IndexExpr *ie = (Iron_IndexExpr *)node;
            replace_in_node(&ie->object, rctx);
            replace_in_node(&ie->index, rctx);
            break;
        }
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)node;
            replace_in_node_array(al->elements, al->element_count, rctx);
            break;
        }
        case IRON_NODE_HEAP: {
            Iron_HeapExpr *he = (Iron_HeapExpr *)node;
            replace_in_node(&he->inner, rctx);
            break;
        }
        case IRON_NODE_RC: {
            Iron_RcExpr *re = (Iron_RcExpr *)node;
            replace_in_node(&re->inner, rctx);
            break;
        }
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;
            replace_in_node_array(ce->args, ce->arg_count, rctx);
            break;
        }
        case IRON_NODE_DEFER: {
            Iron_DeferStmt *ds = (Iron_DeferStmt *)node;
            replace_in_node(&ds->expr, rctx);
            break;
        }
        case IRON_NODE_FREE: {
            Iron_FreeStmt *fs = (Iron_FreeStmt *)node;
            replace_in_node(&fs->expr, rctx);
            break;
        }
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)node;
            replace_in_node(&ss->body, rctx);
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
            replace_in_node(&ms->subject, rctx);
            replace_in_node_array(ms->cases, ms->case_count, rctx);
            replace_in_node(&ms->else_body, rctx);
            break;
        }
        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *mc = (Iron_MatchCase *)node;
            replace_in_node(&mc->body, rctx);
            break;
        }
        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *ae = (Iron_AwaitExpr *)node;
            replace_in_node(&ae->handle, rctx);
            break;
        }
        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *le = (Iron_LambdaExpr *)node;
            replace_in_node(&le->body, rctx);
            break;
        }
        default:
            /* Leaf nodes: INT_LIT, FLOAT_LIT, BOOL_LIT, STRING_LIT, IDENT, etc. */
            break;
    }
}

static void replace_in_node_array(Iron_Node **nodes, int count,
                                   ReplaceCtx *rctx) {
    for (int i = 0; i < count; i++) {
        replace_in_node(&nodes[i], rctx);
    }
}

/* ── Main entry point ────────────────────────────────────────────────────── */

void iron_comptime_apply(Iron_Program *program, Iron_Scope *global_scope,
                          Iron_Arena *arena, Iron_DiagList *diags,
                          const char *source_file_dir, bool force_comptime) {
    (void)force_comptime;  /* reserved for future use */

    Iron_ComptimeCtx eval_ctx;
    eval_ctx.arena           = arena;
    eval_ctx.diags           = diags;
    eval_ctx.global_scope    = global_scope;
    eval_ctx.steps           = 0;
    eval_ctx.step_limit      = 1000000;
    eval_ctx.call_stack      = NULL;
    eval_ctx.call_spans      = NULL;
    eval_ctx.call_depth      = 0;
    eval_ctx.source_file_dir = source_file_dir;
    eval_ctx.had_error       = false;
    eval_ctx.local_frames    = NULL;
    eval_ctx.frame_depth     = 0;
    eval_ctx.had_return      = false;
    eval_ctx.return_val      = NULL;

    ReplaceCtx rctx;
    rctx.eval_ctx = &eval_ctx;
    rctx.arena    = arena;

    replace_in_node((Iron_Node **)&program, &rctx);

    /* Clean up stb_ds arrays */
    arrfree(eval_ctx.call_stack);
    arrfree(eval_ctx.call_spans);
    arrfree(eval_ctx.local_frames);
}
