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
    /* Set by the type checker on the declaring node when the binding is
     * used in a way `val` rejects (mutating method call, field write,
     * `var` / `*var` argument). Such a binding must stay `var`. */
    bool         requires_mutable;
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

/* Phase 18 VAL-06 false-positive fix: param-only "used via field-write"
 * marker. A var parameter with a body that ONLY does field-writes
 * (e.g., `func f(var p: Point) { p.x = 5 }`) is using the param for
 * mutation — must NOT trigger IRON_WARN_UNUSED_VAR_PARAM=614.
 *
 * This helper deliberately ignores non-param tracker entries so VAL-05
 * (val/var local) semantics remain Phase 17's narrowest definition:
 * "Mutation = binding reassignment only. Field writes (x.f = ...) do
 * NOT count." Per Plan 18-01 Pitfall 1: PARM-02 mental model differs
 * from VAL-05 (a var param exists for mutation; a var local with only
 * field-writes can be a val). */
static void mark_param_used_via_field_write(UnusedVarCtx *ctx, Iron_Ident *id) {
    if (!id || !id->name) return;
    for (ptrdiff_t i = 0; i < arrlen(ctx->trackers); i++) {
        VarTracker *t = &ctx->trackers[i];
        if (t->is_param && t->name && strcmp(t->name, id->name) == 0) {
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
                 * both count per Phase 17 CONTEXT.md. Only IDENT targets count
                 * for VAL-05 (val locals); for var-param VAL-06 the
                 * FIELD_ACCESS arm below also counts. */
                mark_reassigned(ctx, (Iron_Ident *)as->target);
            } else if (as->target && as->target->kind == IRON_NODE_FIELD_ACCESS) {
                /* Phase 18 VAL-06 false-positive fix: walk the FIELD_ACCESS
                 * chain to its root IDENT and mark the param binding as
                 * written.
                 *
                 * Rationale: a var parameter doing only field-writes (e.g.,
                 * `func f(var p: Point) { p.x = 5 }`) is being USED for
                 * mutation — must NOT trigger IRON_WARN_UNUSED_VAR_PARAM=614.
                 *
                 * Phase 17 VAL-05 (unused val local) intentionally does NOT
                 * extend this way — field-write does not "use" a val local
                 * for VAL-05 purposes (binding never reassigned). PARM-02
                 * mental model differs from VAL-05: a var param exists for
                 * mutation; field-write counts as use. The
                 * mark_param_used_via_field_write helper FILTERS to
                 * is_param entries so the Phase 17 test
                 * test_val_05_field_write_does_not_count keeps passing
                 * (var-local field-writes still warn). */
                Iron_Node *cur = as->target;
                while (cur && cur->kind == IRON_NODE_FIELD_ACCESS) {
                    cur = ((Iron_FieldAccess *)cur)->object;
                }
                if (cur && cur->kind == IRON_NODE_IDENT) {
                    mark_param_used_via_field_write(ctx, (Iron_Ident *)cur);
                }
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

/* A `var` referenced inside a lambda is captured by reference; a `val` is
 * captured as a snapshot. Changing such a binding to `val` changes what
 * the closure sees (and breaks writes made through it), so a captured
 * `var` is never reported. */
static void mark_captured_name(UnusedVarCtx *ctx, const char *name) {
    if (!name) return;
    for (ptrdiff_t i = 0; i < arrlen(ctx->trackers); i++) {
        VarTracker *t = &ctx->trackers[i];
        if (t->name && strcmp(t->name, name) == 0) {
            t->requires_mutable = true;
            return;
        }
    }
}

static void walk_node(UnusedVarCtx *ctx, Iron_Node *node, bool in_lambda);

static void walk_nodes(UnusedVarCtx *ctx, Iron_Node **nodes, int count,
                       bool in_lambda) {
    for (int i = 0; nodes && i < count; i++) walk_node(ctx, nodes[i], in_lambda);
}

/* Visit every statement and expression under `node`, marking tracked
 * bindings referenced inside lambda bodies. */
static void walk_node(UnusedVarCtx *ctx, Iron_Node *node, bool in_lambda) {
    if (!node) return;
    switch ((int)node->kind) {
        case IRON_NODE_IDENT:
            if (in_lambda) mark_captured_name(ctx, ((Iron_Ident *)node)->name);
            break;
        case IRON_NODE_LAMBDA:
            walk_node(ctx, ((Iron_LambdaExpr *)node)->body, true);
            break;
        case IRON_NODE_SPAWN:
            /* spawn bodies capture like lambdas. */
            walk_node(ctx, ((Iron_SpawnStmt *)node)->body, true);
            break;
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)node;
            walk_nodes(ctx, b->stmts, b->stmt_count, in_lambda);
            break;
        }
        case IRON_NODE_VAR_DECL: walk_node(ctx, ((Iron_VarDecl *)node)->init, in_lambda); break;
        case IRON_NODE_VAL_DECL: walk_node(ctx, ((Iron_ValDecl *)node)->init, in_lambda); break;
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            walk_node(ctx, as->target, in_lambda);
            walk_node(ctx, as->value, in_lambda);
            break;
        }
        case IRON_NODE_RETURN: walk_node(ctx, ((Iron_ReturnStmt *)node)->value, in_lambda); break;
        case IRON_NODE_IF: {
            Iron_IfStmt *is_ = (Iron_IfStmt *)node;
            walk_node(ctx, is_->condition, in_lambda);
            walk_node(ctx, is_->body, in_lambda);
            walk_nodes(ctx, is_->elif_conds, is_->elif_count, in_lambda);
            walk_nodes(ctx, is_->elif_bodies, is_->elif_count, in_lambda);
            walk_node(ctx, is_->else_body, in_lambda);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *w = (Iron_WhileStmt *)node;
            walk_node(ctx, w->condition, in_lambda);
            walk_node(ctx, w->body, in_lambda);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *f = (Iron_ForStmt *)node;
            walk_node(ctx, f->iterable, in_lambda);
            walk_node(ctx, f->body, in_lambda);
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *m = (Iron_MatchStmt *)node;
            walk_node(ctx, m->subject, in_lambda);
            walk_nodes(ctx, m->cases, m->case_count, in_lambda);
            walk_node(ctx, m->else_body, in_lambda);
            break;
        }
        case IRON_NODE_MATCH_CASE: walk_node(ctx, ((Iron_MatchCase *)node)->body, in_lambda); break;
        case IRON_NODE_DEFER: walk_node(ctx, ((Iron_DeferStmt *)node)->expr, in_lambda); break;
        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *is_ = (Iron_InterpString *)node;
            walk_nodes(ctx, is_->parts, is_->part_count, in_lambda);
            break;
        }
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *b = (Iron_BinaryExpr *)node;
            walk_node(ctx, b->left, in_lambda);
            walk_node(ctx, b->right, in_lambda);
            break;
        }
        case IRON_NODE_UNARY: walk_node(ctx, ((Iron_UnaryExpr *)node)->operand, in_lambda); break;
        case IRON_NODE_CALL: {
            Iron_CallExpr *c = (Iron_CallExpr *)node;
            walk_node(ctx, c->callee, in_lambda);
            walk_nodes(ctx, c->args, c->arg_count, in_lambda);
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            walk_node(ctx, mc->object, in_lambda);
            walk_nodes(ctx, mc->args, mc->arg_count, in_lambda);
            break;
        }
        case IRON_NODE_FIELD_ACCESS: walk_node(ctx, ((Iron_FieldAccess *)node)->object, in_lambda); break;
        case IRON_NODE_INDEX: {
            Iron_IndexExpr *ix = (Iron_IndexExpr *)node;
            walk_node(ctx, ix->object, in_lambda);
            walk_node(ctx, ix->index, in_lambda);
            break;
        }
        case IRON_NODE_SLICE: {
            Iron_SliceExpr *sl = (Iron_SliceExpr *)node;
            walk_node(ctx, sl->object, in_lambda);
            walk_node(ctx, sl->start, in_lambda);
            walk_node(ctx, sl->end, in_lambda);
            break;
        }
        case IRON_NODE_HEAP: walk_node(ctx, ((Iron_HeapExpr *)node)->inner, in_lambda); break;
        case IRON_NODE_RC: walk_node(ctx, ((Iron_RcExpr *)node)->inner, in_lambda); break;
        case IRON_NODE_COMPTIME: walk_node(ctx, ((Iron_ComptimeExpr *)node)->inner, in_lambda); break;
        case IRON_NODE_IS: walk_node(ctx, ((Iron_IsExpr *)node)->expr, in_lambda); break;
        case IRON_NODE_AWAIT: walk_node(ctx, ((Iron_AwaitExpr *)node)->handle, in_lambda); break;
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *c = (Iron_ConstructExpr *)node;
            walk_nodes(ctx, c->args, c->arg_count, in_lambda);
            break;
        }
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)node;
            walk_nodes(ctx, al->elements, al->element_count, in_lambda);
            break;
        }
        case IRON_NODE_ENUM_CONSTRUCT: {
            Iron_EnumConstruct *ec = (Iron_EnumConstruct *)node;
            walk_nodes(ctx, ec->args, ec->arg_count, in_lambda);
            break;
        }
        /* -Wswitch-enum opt-out: literals, declarations and structural
         * helpers hold no identifiers that can be captured. */
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
            t.requires_mutable = p->requires_mutable;
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
                    t.requires_mutable = vd->requires_mutable;
                    arrput(ctx->trackers, t);
                }
            }
        }
    }

    /* Scan entire body for IDENT-LHS writes. */
    scan_for_writes(ctx, body);
    /* Bindings captured by lambdas / spawn bodies must stay `var`. */
    walk_node(ctx, body, false);

    /* Emit warnings for unmarked entries. */
    for (ptrdiff_t i = 0; i < arrlen(ctx->trackers); i++) {
        VarTracker *t = &ctx->trackers[i];
        if (t->was_reassigned || t->requires_mutable) continue;
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
