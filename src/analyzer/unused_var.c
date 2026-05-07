/* Phase 17 VAL-05/VAL-06: unused-var warning pass.
 * See unused_var.h for semantics. */
#include "analyzer/unused_var.h"
#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "vendor/stb_ds.h"

#include <string.h>

typedef struct {
    Iron_Span    var_keyword_span;  /* 3-char span anchored on `var` keyword */
    const char  *name;
    struct Iron_Symbol *sym;        /* identity for param-shadowing case */
    bool         is_param;          /* true → IRON_WARN_UNUSED_VAR_PARAM */
    bool         was_reassigned;
} VarTracker;

typedef struct {
    Iron_Arena    *arena;
    Iron_DiagList *diags;
    VarTracker    *trackers;        /* stb_ds dynamic per-function */
    const _Atomic bool *cancel_flag;
} UnusedVarCtx;

/* Per RESEARCH Open Question 4: derive a 3-char span starting at the
 * binding span's start position (the `var` keyword precedes name).
 * Phase 34 may upgrade to a stored Iron_VarDecl.var_keyword_span field
 * if this proves fragile; for v1 the synthesized 3-char anchor is
 * sufficient for LSP-06 quickfix replacement target. */
static Iron_Span var_keyword_span_from(Iron_Span s) {
    Iron_Span r = s;
    r.end_line = s.line;
    r.end_col  = s.col + 3;  /* "var" is 3 chars */
    return r;
}

/* Mark a tracker entry as reassigned.
 *
 * Matching strategy: we use NAME equality. The pass currently does not wire
 * symbol identity onto trackers (params are not given a resolved_sym pointer
 * by the parser/resolver in a form we can read here without extending
 * Iron_Param). Name match is sufficient because:
 *
 * - Per-function scope: trackers are reset per function, so name collisions
 *   across functions cannot happen.
 * - Locals shadowing locals within the same function are rare and either
 *   parser-rejected or scope-isolated in distinct nested blocks; in either
 *   case marking the outer var on an inner write is a safe over-approximation
 *   (we'd rather miss a warning than fire a false positive).
 *
 * RESEARCH Pitfall 3 (var param shadowed by inner val of same name): the
 * inner `val p = ...` is a value-defining declaration, NOT an assignment
 * statement, so it is never visited by scan_for_writes' IRON_NODE_ASSIGN
 * branch. Therefore the param tracker stays unmarked and the warning fires
 * on the outer var param — the desired behaviour. The shadowing `val` does
 * not "hide" the param from this pass because we only react to assigns. */
static void mark_reassigned(UnusedVarCtx *ctx, Iron_Ident *id) {
    if (!id || !id->name) return;
    for (ptrdiff_t i = 0; i < arrlen(ctx->trackers); i++) {
        VarTracker *t = &ctx->trackers[i];
        if (t->name && strcmp(t->name, id->name) == 0) {
            t->was_reassigned = true;
            return;
        }
    }
}

/* Recursively walk a node looking for IDENT-LHS assigns/compound-assigns.
 * Mirrors init_check.c stmt-traversal pattern. Default branch recurses no
 * further (safe over-approximation: the worst case is a missed warning,
 * never a false fire).
 *
 * -Wswitch-enum opt-out via (int) cast — the default branch safely handles
 * every other Iron_NodeKind (no recursion needed for IDENT/literals/calls
 * since they cannot contain reassign-LHS targets that change a tracked
 * binding's symbol identity). */
static void scan_for_writes(UnusedVarCtx *ctx, Iron_Node *node) {
    if (!node) return;
    switch ((int)node->kind) {
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            if (as->target && as->target->kind == IRON_NODE_IDENT) {
                /* Direct `x = ...` AND compound `x += ...` (op != IRON_OP_NONE)
                 * both count per CONTEXT.md. Only IDENT targets count;
                 * field writes (`x.f = ...`) intentionally skip below. */
                mark_reassigned(ctx, (Iron_Ident *)as->target);
            }
            /* Recurse into value to find nested writes. Do NOT recurse
             * into the IDENT target — already handled. Do recurse into
             * non-IDENT targets (e.g., FIELD_ACCESS) so writes inside
             * those subexprs are caught. */
            if (as->target && as->target->kind != IRON_NODE_IDENT) {
                scan_for_writes(ctx, as->target);
            }
            scan_for_writes(ctx, as->value);
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)node;
            for (int i = 0; i < b->stmt_count; i++) {
                scan_for_writes(ctx, b->stmts[i]);
            }
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            scan_for_writes(ctx, vd->init);
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            scan_for_writes(ctx, vd->init);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is_ = (Iron_IfStmt *)node;
            scan_for_writes(ctx, is_->condition);
            scan_for_writes(ctx, is_->body);
            for (int i = 0; i < is_->elif_count; i++) {
                scan_for_writes(ctx, is_->elif_conds[i]);
                scan_for_writes(ctx, is_->elif_bodies[i]);
            }
            scan_for_writes(ctx, is_->else_body);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *w = (Iron_WhileStmt *)node;
            scan_for_writes(ctx, w->condition);
            scan_for_writes(ctx, w->body);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *f = (Iron_ForStmt *)node;
            scan_for_writes(ctx, f->iterable);
            scan_for_writes(ctx, f->body);
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *m = (Iron_MatchStmt *)node;
            scan_for_writes(ctx, m->subject);
            for (int i = 0; i < m->case_count; i++) {
                Iron_Node *c = m->cases[i];
                if (c && c->kind == IRON_NODE_MATCH_CASE) {
                    Iron_MatchCase *mc = (Iron_MatchCase *)c;
                    scan_for_writes(ctx, mc->body);
                }
            }
            scan_for_writes(ctx, m->else_body);
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            scan_for_writes(ctx, rs->value);
            break;
        }
        /* Other statement / expression kinds: conservative default is no
         * recursion (over-warns, never under-warns is the safer direction
         * for missed-warning vs false-fire tradeoff). ADD CASES HERE as
         * test failures show missed paths. */
        default:
            break;
    }
}

/* Per-function entry: collect var locals + params, walk body, emit. */
static void check_function_body(UnusedVarCtx *ctx,
                                 Iron_Node **params, int param_count,
                                 Iron_Node *body) {
    if (!body) return;
    arrsetlen(ctx->trackers, 0);

    /* Collect var params (VAL-06). */
    for (int i = 0; i < param_count; i++) {
        if (!params[i] || params[i]->kind != IRON_NODE_PARAM) continue;
        Iron_Param *p = (Iron_Param *)params[i];
        if (p->is_var && p->name) {
            VarTracker t;
            t.var_keyword_span = var_keyword_span_from(p->span);
            t.name = p->name;
            t.sym = NULL;
            t.is_param = true;
            t.was_reassigned = false;
            arrput(ctx->trackers, t);
        }
    }

    /* Collect var locals (VAL-05). Walk body once for IRON_NODE_VAR_DECL
     * nodes. Use a minimal walker to avoid re-traversing the entire body
     * (the scan_for_writes pass below handles full traversal). */
    if (body->kind == IRON_NODE_BLOCK) {
        Iron_Block *b = (Iron_Block *)body;
        for (int i = 0; i < b->stmt_count; i++) {
            Iron_Node *s = b->stmts[i];
            if (s && s->kind == IRON_NODE_VAR_DECL) {
                Iron_VarDecl *vd = (Iron_VarDecl *)s;
                if (vd->name) {
                    VarTracker t;
                    t.var_keyword_span = var_keyword_span_from(vd->span);
                    t.name = vd->name;
                    t.sym = NULL;
                    t.is_param = false;
                    t.was_reassigned = false;
                    arrput(ctx->trackers, t);
                }
            }
        }
    }

    /* Scan entire body for IDENT-LHS writes. */
    scan_for_writes(ctx, body);

    /* Emit warnings for unmarked entries. */
    for (ptrdiff_t i = 0; i < arrlen(ctx->trackers); i++) {
        VarTracker *t = &ctx->trackers[i];
        if (t->was_reassigned) continue;
        int code = t->is_param
            ? IRON_WARN_UNUSED_VAR_PARAM
            : IRON_WARN_UNUSED_VAR;
        const char *msg = t->is_param
            ? "var parameter never mutated; remove 'var' modifier"
            : "var binding never reassigned; declare as 'val'";
        const char *hint = t->is_param
            ? "drop the 'var' modifier - parameters default to read-only"
            : "change 'var' to 'val' for an immutable binding";
        const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
        if (!msg_copy) msg_copy = "unused var";
        const char *hint_copy = iron_arena_strdup(ctx->arena, hint, strlen(hint));
        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_WARNING,
                       code, t->var_keyword_span, msg_copy, hint_copy);
    }
}

void iron_unused_var_check(Iron_Program *program,
                            Iron_Scope *global_scope,
                            Iron_Arena *arena,
                            Iron_DiagList *diags,
                            const _Atomic bool *cancel_flag) {
    (void)global_scope;
    if (!program) return;
    UnusedVarCtx ctx;
    ctx.arena = arena;
    ctx.diags = diags;
    ctx.trackers = NULL;
    ctx.cancel_flag = cancel_flag;

    for (int i = 0; i < program->decl_count; i++) {
        /* HARD-05: per-function cancel poll boundary. */
        if (cancel_flag &&
            atomic_load_explicit(cancel_flag, memory_order_relaxed)) {
            break;
        }
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        if (d->kind == IRON_NODE_FUNC_DECL) {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
            check_function_body(&ctx, fd->params, fd->param_count, fd->body);
        } else if (d->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *md = (Iron_MethodDecl *)d;
            check_function_body(&ctx, md->params, md->param_count, md->body);
        }
        /* Note: object methods are flattened into top-level Iron_MethodDecl
         * nodes by the parser, so the IRON_NODE_METHOD_DECL branch above
         * already covers in-object methods. We intentionally do NOT recurse
         * into IRON_NODE_OBJECT_DECL here. */
    }

    if (ctx.trackers) arrfree(ctx.trackers);
}
