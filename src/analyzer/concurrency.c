/* concurrency.c — Concurrency checking pass for Iron.
 *
 * Pass 4 of the semantic pipeline: validates concurrency constructs.
 *
 * Current checks:
 *   1. Parallel-for bodies may not mutate outer non-mutex variables (E0208).
 *      "Outer" means declared outside the parallel-for body block.
 *      Reading outer variables is fine.
 *   2. Spawn block capture analysis: mutable captures of outer variables
 *      flagged as potential data races (W0604).
 */

#include "analyzer/concurrency.h"
#include "analyzer/types.h"
#include "vendor/stb_ds.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>

/* ── Cancellation helper (HARD-05) ─────────────────────────────────────────── */
static inline bool iron_cancel_requested(const _Atomic bool *flag) {
    return flag != NULL && atomic_load_explicit(flag, memory_order_relaxed);
}

/* ── Context ─────────────────────────────────────────────────────────────── */

typedef struct {
    Iron_Arena    *arena;
    Iron_DiagList *diags;

    /* Names of variables local to the current parallel-for body.
     * An assignment to a name NOT in this set is an outer mutation. */
    const char   **local_names;   /* stb_ds dynamic array */

    /* Are we currently inside a parallel-for body? */
    bool           in_parallel;

    /* Spawn capture analysis state */
    bool           in_spawn;
    const char   **spawn_writes;  /* stb_ds: names written inside spawn body */
    const char   **spawn_reads;   /* stb_ds: names read inside spawn body   */

    /* HARD-05: cooperative cancellation flag (NULL means never cancel). */
    const _Atomic bool *cancel_flag;

    /* Loop variable of the innermost parallel for (its element is private
     * to one iteration: xs[i] with i the loop variable is disjoint). */
    const char   *par_loop_var;
    Iron_Program *program;
} ConcurrencyCtx;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Recursively extract the root identifier name from an expression.
 * Handles bare identifiers, field access chains, and index expressions. */
static const char *expr_ident_name(Iron_Node *node) {
    if (!node) return NULL;
    switch ((int)(node->kind)) {
        case IRON_NODE_IDENT:
            return ((Iron_Ident *)node)->name;
        case IRON_NODE_FIELD_ACCESS:
            return expr_ident_name(((Iron_FieldAccess *)node)->object);
        case IRON_NODE_INDEX:
            return expr_ident_name(((Iron_IndexExpr *)node)->object);
        /* -Wswitch-enum opt-out: identifier-root extractor only handles the
         * three kinds that can be aliased storage locations. */
        default:
            return NULL;
    }
}

static bool name_is_local(ConcurrencyCtx *ctx, const char *name) {
    int n = arrlen(ctx->local_names);
    for (int i = 0; i < n; i++) {
        if (ctx->local_names[i] && strcmp(ctx->local_names[i], name) == 0)
            return true;
    }
    return false;
}

static void emit_err(ConcurrencyCtx *ctx, int code, Iron_Span span,
                     const char *msg) {
    const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
    if (!msg_copy) { /* HARD-09 REPLACE (concurrency.c:emit_err msg) */ msg_copy = "analyzer error"; }
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR, code, span,
                   msg_copy, NULL);
}

static void emit_warn(ConcurrencyCtx *ctx, int code, Iron_Span span,
                      const char *msg) {
    const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
    if (!msg_copy) { /* HARD-09 REPLACE (concurrency.c:emit_warn msg) */ msg_copy = "analyzer error"; }
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_WARNING, code, span,
                   msg_copy, NULL);
}

/* ── Collect locally-defined names in a block ─────────────────────────────── */

/* Collect all val/var declaration names from a list of statements.
 * These are the names local to the parallel-for body. */
static void collect_local_names(ConcurrencyCtx *ctx,
                                 Iron_Node **stmts, int count) {
    for (int i = 0; i < count; i++) {
        Iron_Node *s = stmts[i];
        if (!s) continue;
        if (s->kind == IRON_NODE_VAL_DECL) {
            Iron_ValDecl *vd = (Iron_ValDecl *)s;
            /* Tuple destructure (`val (a, b) = ...`) has name == NULL and
             * carries its bindings in binding_names[] (NULL entry = `_`);
             * pushing NULL would crash the strcmp in name_is_local. */
            if (vd->name) arrpush(ctx->local_names, vd->name);
            for (int bi = 0; bi < vd->binding_count; bi++) {
                if (vd->binding_names && vd->binding_names[bi])
                    arrpush(ctx->local_names, vd->binding_names[bi]);
            }
        } else if (s->kind == IRON_NODE_VAR_DECL) {
            if (((Iron_VarDecl *)s)->name)
                arrpush(ctx->local_names, ((Iron_VarDecl *)s)->name);
        } else if (s->kind == IRON_NODE_BLOCK) {
            Iron_Block *blk = (Iron_Block *)s;
            collect_local_names(ctx, blk->stmts, blk->stmt_count);
        }
    }
}

/* ── Spawn capture analysis: collect outer refs from spawn body ───────────── */

#define MAX_SPAWN_CAPTURES 64

/* Recursively walk a spawn body, recording outer-variable writes and reads. */
static void collect_spawn_refs(ConcurrencyCtx *ctx, Iron_Node *node) {
    if (!node) return;

    /* Bound check: stop collecting if we hit the limit */
    if (arrlen(ctx->spawn_writes) + arrlen(ctx->spawn_reads) >= MAX_SPAWN_CAPTURES)
        return;

    switch ((int)(node->kind)) {
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            /* Write: extract root name from assignment target */
            const char *tgt_name = expr_ident_name(as->target);
            if (tgt_name && !name_is_local(ctx, tgt_name)) {
                arrpush(ctx->spawn_writes, tgt_name);
            }
            /* The RHS may read outer variables -- recurse */
            collect_spawn_refs(ctx, as->value);
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            /* Register as local, then check init for reads. Tuple
             * destructure (`val (a, b) = ...`) has name == NULL and
             * carries its bindings in binding_names[] (NULL entry = `_`). */
            if (vd->name) arrpush(ctx->local_names, vd->name);
            for (int bi = 0; bi < vd->binding_count; bi++) {
                if (vd->binding_names && vd->binding_names[bi])
                    arrpush(ctx->local_names, vd->binding_names[bi]);
            }
            collect_spawn_refs(ctx, vd->init);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            /* Register as local, then check init for reads */
            if (vd->name) arrpush(ctx->local_names, vd->name);
            collect_spawn_refs(ctx, vd->init);
            break;
        }
        case IRON_NODE_IDENT: {
            /* An identifier in expression context: potential outer read */
            const char *name = ((Iron_Ident *)node)->name;
            if (name && !name_is_local(ctx, name)) {
                arrpush(ctx->spawn_reads, name);
            }
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            for (int i = 0; i < blk->stmt_count; i++) {
                collect_spawn_refs(ctx, blk->stmts[i]);
            }
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            collect_spawn_refs(ctx, is->condition);
            collect_spawn_refs(ctx, is->body);
            for (int i = 0; i < is->elif_count; i++) {
                collect_spawn_refs(ctx, is->elif_bodies[i]);
            }
            if (is->else_body) collect_spawn_refs(ctx, is->else_body);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            collect_spawn_refs(ctx, ws->condition);
            collect_spawn_refs(ctx, ws->body);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            if (fs->var_name) arrpush(ctx->local_names, fs->var_name);
            collect_spawn_refs(ctx, fs->body);
            break;
        }
        /* AUDIT-02 #12 fix: CALL / METHOD_CALL / MATCH / DEFER / FREE / LEAK
         * were previously missed, so spawn-body analysis silently ignored
         * reads/writes that occurred inside function calls, match arms, and
         * defer/free/leak statements. */
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)node;
            collect_spawn_refs(ctx, ce->callee);
            for (int i = 0; i < ce->arg_count; i++) {
                collect_spawn_refs(ctx, ce->args[i]);
            }
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            collect_spawn_refs(ctx, mc->object);
            for (int i = 0; i < mc->arg_count; i++) {
                collect_spawn_refs(ctx, mc->args[i]);
            }
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
            collect_spawn_refs(ctx, ms->subject);
            for (int i = 0; i < ms->case_count; i++) {
                Iron_MatchCase *mc = (Iron_MatchCase *)ms->cases[i];
                if (mc && mc->body) collect_spawn_refs(ctx, mc->body);
            }
            if (ms->else_body) collect_spawn_refs(ctx, ms->else_body);
            break;
        }
        case IRON_NODE_DEFER: {
            Iron_DeferStmt *ds = (Iron_DeferStmt *)node;
            collect_spawn_refs(ctx, ds->expr);
            break;
        }
        case IRON_NODE_FREE: {
            Iron_FreeStmt *fs = (Iron_FreeStmt *)node;
            collect_spawn_refs(ctx, fs->expr);
            break;
        }
        case IRON_NODE_LEAK: {
            Iron_LeakStmt *ls = (Iron_LeakStmt *)node;
            collect_spawn_refs(ctx, ls->expr);
            break;
        }
        /* HARD-04: graceful no-op on parser ErrorNode. */
        case IRON_NODE_ERROR:
            break;

        /* HARD-04: sentinel — never a real node kind. */
        case IRON_NODE_COUNT:
            break;

        /* -Wswitch-enum opt-out: collect_spawn_refs is a generic walker that
         * only cares about statements that can read or write a variable; all
         * remaining Iron_NodeKind values are legitimate no-ops here. */
        default:
            break;
    }
}

/* ── Check assignments inside a parallel-for body ─────────────────────────── */

/* Forward declaration */
/* True when `n` is exactly the current parallel loop variable. */
static bool is_par_loop_var(ConcurrencyCtx *ctx, Iron_Node *n) {
    return ctx->par_loop_var && n && n->kind == IRON_NODE_IDENT &&
           ((Iron_Ident *)n)->name &&
           strcmp(((Iron_Ident *)n)->name, ctx->par_loop_var) == 0;
}

/* `outer[i]` where i is the parallel loop variable: each iteration owns a
 * distinct element, so writing it (or calling a mutating method on it) is
 * not a race. */
static bool is_own_element(ConcurrencyCtx *ctx, Iron_Node *n) {
    return n && n->kind == IRON_NODE_INDEX &&
           is_par_loop_var(ctx, ((Iron_IndexExpr *)n)->index);
}

static bool list_method_mutates(const char *m) {
    static const char *const k[] = { "push", "pop", "set", "insert", "remove",
                                     "clear", "reverse", "sort", NULL };
    for (int i = 0; k[i]; i++) if (m && strcmp(m, k[i]) == 0) return true;
    return false;
}

/* Whether a method call can mutate its receiver, from the receiver type. */
static bool method_call_mutates(ConcurrencyCtx *ctx, Iron_MethodCallExpr *mc) {
    Iron_Type *t = mc->object ? ((Iron_ExprNode *)mc->object)->resolved_type : NULL;
    if (!t || !mc->method) return false;
    if (t->kind == IRON_TYPE_ARRAY) return list_method_mutates(mc->method);
    if (t->kind != IRON_TYPE_OBJECT || !t->object.decl || !ctx->program) return false;
    const char *tn = t->object.decl->name;
    /* Mutex / Channel / RWLock operations synchronize: they are the way to
     * share state across iterations. */
    if (tn && (strcmp(tn, "Mutex") == 0 || strcmp(tn, "MutexGuard") == 0 ||
               strcmp(tn, "Channel") == 0 || strcmp(tn, "RWLock") == 0 ||
               strcmp(tn, "RWReadGuard") == 0 || strcmp(tn, "RWWriteGuard") == 0))
        return false;
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *d = ctx->program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (md->type_name && md->method_name && tn &&
            strcmp(md->type_name, tn) == 0 && strcmp(md->method_name, mc->method) == 0)
            return !md->is_readonly && !md->is_pure;
    }
    return false;
}

/* Names declared in a lambda (params + local val/var) are its own. */
static bool lambda_declares(Iron_LambdaExpr *le, Iron_Node *n, const char *name);

static bool block_declares(Iron_Node *n, const char *name) {
    if (!n || n->kind != IRON_NODE_BLOCK) return false;
    Iron_Block *b = (Iron_Block *)n;
    for (int i = 0; i < b->stmt_count; i++) {
        Iron_Node *st = b->stmts[i];
        if (!st) continue;
        if (st->kind == IRON_NODE_VAL_DECL && ((Iron_ValDecl *)st)->name &&
            strcmp(((Iron_ValDecl *)st)->name, name) == 0) return true;
        if (st->kind == IRON_NODE_VAR_DECL && ((Iron_VarDecl *)st)->name &&
            strcmp(((Iron_VarDecl *)st)->name, name) == 0) return true;
    }
    return false;
}

static bool lambda_declares(Iron_LambdaExpr *le, Iron_Node *n, const char *name) {
    (void)n;
    for (int i = 0; i < le->param_count; i++) {
        Iron_Param *p = (Iron_Param *)le->params[i];
        if (p && p->name && strcmp(p->name, name) == 0) return true;
    }
    return block_declares(le->body, name);
}

/* The first captured variable a lambda assigns, or NULL. */
static const char *lambda_writes_capture(Iron_LambdaExpr *le, Iron_Node *n) {
    if (!n) return NULL;
    switch ((int)n->kind) {
        case IRON_NODE_ASSIGN: {
            const char *root = expr_ident_name(((Iron_AssignStmt *)n)->target);
            if (root && !lambda_declares(le, NULL, root)) return root;
            return NULL;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)n;
            for (int i = 0; i < b->stmt_count; i++) {
                const char *w = lambda_writes_capture(le, b->stmts[i]);
                if (w) return w;
            }
            return NULL;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is_ = (Iron_IfStmt *)n;
            const char *w = lambda_writes_capture(le, is_->body);
            for (int i = 0; !w && i < is_->elif_count; i++)
                w = lambda_writes_capture(le, is_->elif_bodies[i]);
            if (!w) w = lambda_writes_capture(le, is_->else_body);
            return w;
        }
        case IRON_NODE_WHILE: return lambda_writes_capture(le, ((Iron_WhileStmt *)n)->body);
        case IRON_NODE_FOR:   return lambda_writes_capture(le, ((Iron_ForStmt *)n)->body);
        default: return NULL;
    }
}

/* Scan an expression in a parallel-for body for writes that race: a
 * mutating method call on an outer binding (other than this iteration's
 * own element), or a call to a local closure that assigns a captured
 * variable (the closure's write is a write to the outer variable). */
static void check_expr_for_mutation(ConcurrencyCtx *ctx, Iron_Node *n) {
    if (!n) return;
    switch ((int)n->kind) {
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)n;
            check_expr_for_mutation(ctx, mc->object);
            for (int i = 0; i < mc->arg_count; i++) check_expr_for_mutation(ctx, mc->args[i]);
            const char *root = expr_ident_name(mc->object);
            if (root && !name_is_local(ctx, root) && !is_own_element(ctx, mc->object) &&
                method_call_mutates(ctx, mc)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "cannot call mutating method '%s' on outer variable '%s' "
                         "in parallel for body; use mutex for shared state (E0208)",
                         mc->method ? mc->method : "?", root);
                emit_err(ctx, IRON_ERR_PARALLEL_MUTATION, mc->span, msg);
            }
            break;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)n;
            for (int i = 0; i < ce->arg_count; i++) check_expr_for_mutation(ctx, ce->args[i]);
            if (!ce->callee || ce->callee->kind != IRON_NODE_IDENT) break;
            Iron_Ident *id = (Iron_Ident *)ce->callee;
            Iron_Node *d = id->resolved_sym ? id->resolved_sym->decl_node : NULL;
            Iron_Node *init = NULL;
            if (d && d->kind == IRON_NODE_VAL_DECL) init = ((Iron_ValDecl *)d)->init;
            else if (d && d->kind == IRON_NODE_VAR_DECL) init = ((Iron_VarDecl *)d)->init;
            if (init && init->kind == IRON_NODE_LAMBDA) {
                Iron_LambdaExpr *le = (Iron_LambdaExpr *)init;
                const char *w = lambda_writes_capture(le, le->body);
                if (w && !name_is_local(ctx, w)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "closure '%s' assigns captured variable '%s'; calling it "
                             "in a parallel for body races (E0208)",
                             id->name ? id->name : "?", w);
                    emit_err(ctx, IRON_ERR_PARALLEL_MUTATION, ce->span, msg);
                }
            }
            break;
        }
        case IRON_NODE_BINARY:
            check_expr_for_mutation(ctx, ((Iron_BinaryExpr *)n)->left);
            check_expr_for_mutation(ctx, ((Iron_BinaryExpr *)n)->right);
            break;
        case IRON_NODE_UNARY: check_expr_for_mutation(ctx, ((Iron_UnaryExpr *)n)->operand); break;
        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *is_ = (Iron_InterpString *)n;
            for (int i = 0; i < is_->part_count; i++) check_expr_for_mutation(ctx, is_->parts[i]);
            break;
        }
        case IRON_NODE_FIELD_ACCESS: check_expr_for_mutation(ctx, ((Iron_FieldAccess *)n)->object); break;
        case IRON_NODE_INDEX:
            check_expr_for_mutation(ctx, ((Iron_IndexExpr *)n)->object);
            check_expr_for_mutation(ctx, ((Iron_IndexExpr *)n)->index);
            break;
        default:
            break;
    }
}

static void check_body_stmts(ConcurrencyCtx *ctx, Iron_Node **stmts, int count);

static void check_stmt_for_mutation(ConcurrencyCtx *ctx, Iron_Node *node) {
    if (!node) return;
    switch ((int)(node->kind)) {
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            if (!as->target) break;

            /* Extract root variable name from the assignment target.
             * Handles bare identifiers, field access chains (obj.field),
             * and index expressions (arr[i]). */
            const char *name = expr_ident_name(as->target);
            if (!name) break;  /* Non-identifier-rooted target; skip */

            check_expr_for_mutation(ctx, as->value);
            /* If the target root is NOT in our local set, it's an outer variable.
             * `outer[i] = v` with i the parallel loop variable writes this
             * iteration's own element and is allowed. */
            if (!name_is_local(ctx, name) && !is_own_element(ctx, as->target)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "cannot mutate outer variable '%s' in parallel for body; "
                         "use mutex for shared state (E0208)",
                         name);
                emit_err(ctx, IRON_ERR_PARALLEL_MUTATION, as->span, msg);
            }
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            check_body_stmts(ctx, blk->stmts, blk->stmt_count);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            if (is->body) check_stmt_for_mutation(ctx, is->body);
            for (int i = 0; i < is->elif_count; i++) {
                check_stmt_for_mutation(ctx, is->elif_bodies[i]);
            }
            if (is->else_body) check_stmt_for_mutation(ctx, is->else_body);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            if (ws->body) check_stmt_for_mutation(ctx, ws->body);
            break;
        }
        /* Sequential nested for loops within a parallel body:
         * still check their bodies for outer mutations. */
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            /* Record the nested loop variable as local */
            if (fs->var_name) arrpush(ctx->local_names, fs->var_name);
            if (fs->body) check_stmt_for_mutation(ctx, fs->body);
            break;
        }
        case IRON_NODE_VAL_DECL:
            check_expr_for_mutation(ctx, ((Iron_ValDecl *)node)->init);
            break;
        case IRON_NODE_VAR_DECL:
            check_expr_for_mutation(ctx, ((Iron_VarDecl *)node)->init);
            break;
        case IRON_NODE_RETURN:
            check_expr_for_mutation(ctx, ((Iron_ReturnStmt *)node)->value);
            break;
        /* Expression statements: calls and method calls. */
        default:
            check_expr_for_mutation(ctx, node);
            break;
    }
}

static void check_body_stmts(ConcurrencyCtx *ctx, Iron_Node **stmts, int count) {
    for (int i = 0; i < count; i++) {
        check_stmt_for_mutation(ctx, stmts[i]);
    }
}

/* ── Walk the full function body looking for parallel-for statements ────────── */

static void walk_stmts(ConcurrencyCtx *ctx, Iron_Node **stmts, int count);
static void walk_stmt(ConcurrencyCtx *ctx, Iron_Node *node);

/* After mutation checking in parallel-for bodies, also walk for nested
 * spawn/parallel-for blocks that need their own analysis. */
static void walk_nested_in_parallel_body(ConcurrencyCtx *ctx, Iron_Node *node) {
    if (!node) return;
    switch ((int)(node->kind)) {
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            for (int i = 0; i < blk->stmt_count; i++)
                walk_nested_in_parallel_body(ctx, blk->stmts[i]);
            break;
        }
        case IRON_NODE_SPAWN:
            /* Delegate to walk_stmt which handles spawn capture analysis */
            walk_stmt(ctx, node);
            break;
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            if (is->body) walk_nested_in_parallel_body(ctx, is->body);
            for (int i = 0; i < is->elif_count; i++)
                walk_nested_in_parallel_body(ctx, is->elif_bodies[i]);
            if (is->else_body) walk_nested_in_parallel_body(ctx, is->else_body);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            if (fs->body) walk_nested_in_parallel_body(ctx, fs->body);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            if (ws->body) walk_nested_in_parallel_body(ctx, ws->body);
            break;
        }
        /* -Wswitch-enum opt-out: only descends into nested control flow that
         * could legitimately contain a spawn / parallel-for. */
        default:
            break;
    }
}

static void walk_stmt(ConcurrencyCtx *ctx, Iron_Node *node) {
    if (!node) return;
    /* HARD-05: cancel poll at recursive statement walker entry. */
    if (iron_cancel_requested(ctx->cancel_flag)) return;
    switch ((int)(node->kind)) {
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            if (fs->is_parallel && fs->body) {
                /* Enter parallel analysis */
                bool prev_in_parallel = ctx->in_parallel;
                ctx->in_parallel = true;

                /* Save local names count so we can restore after this for-stmt */
                int saved_count = arrlen(ctx->local_names);

                /* The loop variable itself is local */
                if (fs->var_name) {
                    arrpush(ctx->local_names, fs->var_name);
                }
                const char *prev_loop_var = ctx->par_loop_var;
                ctx->par_loop_var = fs->var_name;

                /* Collect all val/var decls inside the body as local */
                if (fs->body->kind == IRON_NODE_BLOCK) {
                    Iron_Block *body = (Iron_Block *)fs->body;
                    collect_local_names(ctx, body->stmts, body->stmt_count);
                }

                /* Check assignments in the body */
                check_stmt_for_mutation(ctx, fs->body);

                /* Walk nested spawn/parallel-for blocks inside the body */
                walk_nested_in_parallel_body(ctx, fs->body);

                /* Restore local names to pre-parallel-for state */
                arrsetlen(ctx->local_names, saved_count);
                ctx->in_parallel = prev_in_parallel;
                ctx->par_loop_var = prev_loop_var;
            } else {
                /* Sequential for: walk body recursively for nested parallel fors */
                if (fs->body) walk_stmt(ctx, fs->body);
            }
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            walk_stmts(ctx, blk->stmts, blk->stmt_count);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            if (is->body) walk_stmt(ctx, is->body);
            for (int i = 0; i < is->elif_count; i++) {
                walk_stmt(ctx, is->elif_bodies[i]);
            }
            if (is->else_body) walk_stmt(ctx, is->else_body);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            if (ws->body) walk_stmt(ctx, ws->body);
            break;
        }
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)node;
            if (!ss->body) break;

            /* Save state */
            bool prev_in_spawn = ctx->in_spawn;
            int saved_local_count  = arrlen(ctx->local_names);
            int saved_write_count  = arrlen(ctx->spawn_writes);
            int saved_read_count   = arrlen(ctx->spawn_reads);

            ctx->in_spawn = true;

            /* Collect local names from spawn body block first */
            if (ss->body->kind == IRON_NODE_BLOCK) {
                Iron_Block *body = (Iron_Block *)ss->body;
                collect_local_names(ctx, body->stmts, body->stmt_count);
            }

            /* Walk spawn body to collect outer refs */
            collect_spawn_refs(ctx, ss->body);

            /* Emit warnings for each outer write */
            int cur_writes = arrlen(ctx->spawn_writes);
            for (int w = saved_write_count; w < cur_writes; w++) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "spawn block '%s' mutates outer variable '%s'; "
                         "potential data race (E0604)",
                         ss->name ? ss->name : "<anonymous>",
                         ctx->spawn_writes[w]);
                emit_warn(ctx, IRON_WARN_SPAWN_DATA_RACE, ss->span, msg);
            }

            /* Restore state */
            arrsetlen(ctx->local_names,  saved_local_count);
            arrsetlen(ctx->spawn_writes, saved_write_count);
            arrsetlen(ctx->spawn_reads,  saved_read_count);
            ctx->in_spawn = prev_in_spawn;
            break;
        }
        /* -Wswitch-enum opt-out: top-level walker only recurses into control
         * flow and spawn / parallel-for kinds. */
        default:
            break;
    }
}

static void walk_stmts(ConcurrencyCtx *ctx, Iron_Node **stmts, int count) {
    for (int i = 0; i < count; i++) {
        walk_stmt(ctx, stmts[i]);
    }
}

/* ── Per-function analysis ────────────────────────────────────────────────── */

/* ── Await-once analysis ────────────────────────────────────────────────────
 *
 * `await h` joins the task and frees its handle, so a handle can be awaited
 * once: a second await used a freed handle and hung forever. This walks
 * each function body tracking the handles that MAY already have been
 * awaited on the current path (branches are merged, loop bodies are
 * processed twice so a second iteration is seen, `return` ends a path, and
 * declaring a binding makes it a fresh handle) and reports an await of a
 * handle in that set. */

typedef struct {
    const char **names;   /* stb_ds: handles that may have been awaited */
    bool         dead;    /* the path has returned */
} AwaitState;

typedef struct { Iron_Node *key; bool value; } AwaitReported;

static bool aw_has(const AwaitState *s, const char *n) {
    for (ptrdiff_t i = 0; i < arrlen(s->names); i++)
        if (strcmp(s->names[i], n) == 0) return true;
    return false;
}

static void aw_add(AwaitState *s, const char *n) {
    if (!aw_has(s, n)) arrput(s->names, n);
}

static void aw_remove(AwaitState *s, const char *n) {
    for (ptrdiff_t i = 0; i < arrlen(s->names); i++)
        if (strcmp(s->names[i], n) == 0) { arrdelswap(s->names, i); return; }
}

static AwaitState aw_copy(const AwaitState *s) {
    AwaitState c = { NULL, s->dead };
    for (ptrdiff_t i = 0; i < arrlen(s->names); i++) arrput(c.names, s->names[i]);
    return c;
}

/* dst := dst joined with src (a dead path contributes nothing). */
static void aw_join(AwaitState *dst, const AwaitState *src) {
    if (src->dead) return;
    if (dst->dead) {
        arrfree(dst->names);
        *dst = aw_copy(src);
        return;
    }
    for (ptrdiff_t i = 0; i < arrlen(src->names); i++) aw_add(dst, src->names[i]);
}

static void aw_stmt(ConcurrencyCtx *ctx, Iron_Node *n, AwaitState *s,
                    AwaitReported **reported);

static void aw_expr(ConcurrencyCtx *ctx, Iron_Node *n, AwaitState *s,
                    AwaitReported **reported) {
    if (!n || s->dead) return;
    switch ((int)n->kind) {
        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *ae = (Iron_AwaitExpr *)n;
            aw_expr(ctx, ae->handle, s, reported);
            const char *h = expr_ident_name(ae->handle);
            if (!h) break;
            if (aw_has(s, h) && hmgeti(*reported, n) < 0) {
                hmput(*reported, n, true);
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "handle '%s' may already have been awaited", h);
                emit_err(ctx, IRON_ERR_AWAIT_TWICE, n->span, msg);
            }
            aw_add(s, h);
            break;
        }
        case IRON_NODE_BINARY:
            aw_expr(ctx, ((Iron_BinaryExpr *)n)->left, s, reported);
            aw_expr(ctx, ((Iron_BinaryExpr *)n)->right, s, reported);
            break;
        case IRON_NODE_UNARY: aw_expr(ctx, ((Iron_UnaryExpr *)n)->operand, s, reported); break;
        case IRON_NODE_CALL: {
            Iron_CallExpr *c = (Iron_CallExpr *)n;
            for (int i = 0; i < c->arg_count; i++) aw_expr(ctx, c->args[i], s, reported);
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)n;
            aw_expr(ctx, mc->object, s, reported);
            for (int i = 0; i < mc->arg_count; i++) aw_expr(ctx, mc->args[i], s, reported);
            break;
        }
        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *is_ = (Iron_InterpString *)n;
            for (int i = 0; i < is_->part_count; i++) aw_expr(ctx, is_->parts[i], s, reported);
            break;
        }
        case IRON_NODE_FIELD_ACCESS: aw_expr(ctx, ((Iron_FieldAccess *)n)->object, s, reported); break;
        case IRON_NODE_INDEX:
            aw_expr(ctx, ((Iron_IndexExpr *)n)->object, s, reported);
            aw_expr(ctx, ((Iron_IndexExpr *)n)->index, s, reported);
            break;
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ct = (Iron_ConstructExpr *)n;
            for (int i = 0; i < ct->arg_count; i++) aw_expr(ctx, ct->args[i], s, reported);
            break;
        }
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)n;
            for (int i = 0; i < al->element_count; i++) aw_expr(ctx, al->elements[i], s, reported);
            break;
        }
        /* -Wswitch-enum opt-out: lambdas run later (not on this path);
         * leaves contain no await. */
        default:
            break;
    }
}

static void aw_block(ConcurrencyCtx *ctx, Iron_Node *n, AwaitState *s,
                     AwaitReported **reported) {
    aw_stmt(ctx, n, s, reported);
}

static void aw_stmt(ConcurrencyCtx *ctx, Iron_Node *n, AwaitState *s,
                    AwaitReported **reported) {
    if (!n || s->dead) return;
    switch ((int)n->kind) {
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)n;
            for (int i = 0; i < b->stmt_count; i++) aw_stmt(ctx, b->stmts[i], s, reported);
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)n;
            aw_expr(ctx, vd->init, s, reported);
            if (vd->name) aw_remove(s, vd->name);   /* a fresh binding */
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)n;
            aw_expr(ctx, vd->init, s, reported);
            if (vd->name) aw_remove(s, vd->name);
            break;
        }
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *sp = (Iron_SpawnStmt *)n;
            if (sp->handle_name) aw_remove(s, sp->handle_name);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)n;
            aw_expr(ctx, as->value, s, reported);
            const char *t = expr_ident_name(as->target);
            if (t && as->target && as->target->kind == IRON_NODE_IDENT) aw_remove(s, t);
            break;
        }
        case IRON_NODE_RETURN:
            aw_expr(ctx, ((Iron_ReturnStmt *)n)->value, s, reported);
            s->dead = true;
            break;
        case IRON_NODE_IF: {
            Iron_IfStmt *is_ = (Iron_IfStmt *)n;
            aw_expr(ctx, is_->condition, s, reported);
            AwaitState out = { NULL, true };
            AwaitState br = aw_copy(s);
            aw_block(ctx, is_->body, &br, reported);
            aw_join(&out, &br);
            arrfree(br.names);
            for (int i = 0; i < is_->elif_count; i++) {
                AwaitState eb = aw_copy(s);
                aw_expr(ctx, is_->elif_conds[i], &eb, reported);
                aw_block(ctx, is_->elif_bodies[i], &eb, reported);
                aw_join(&out, &eb);
                arrfree(eb.names);
            }
            AwaitState el = aw_copy(s);
            if (is_->else_body) aw_block(ctx, is_->else_body, &el, reported);
            aw_join(&out, &el);
            arrfree(el.names);
            arrfree(s->names);
            *s = out;
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *m = (Iron_MatchStmt *)n;
            aw_expr(ctx, m->subject, s, reported);
            AwaitState out = { NULL, true };
            for (int i = 0; i < m->case_count; i++) {
                Iron_Node *cn = m->cases[i];
                if (!cn || cn->kind != IRON_NODE_MATCH_CASE) continue;
                AwaitState cb = aw_copy(s);
                aw_block(ctx, ((Iron_MatchCase *)cn)->body, &cb, reported);
                aw_join(&out, &cb);
                arrfree(cb.names);
            }
            AwaitState el = aw_copy(s);
            if (m->else_body) aw_block(ctx, m->else_body, &el, reported);
            aw_join(&out, &el);
            arrfree(el.names);
            arrfree(s->names);
            *s = out;
            break;
        }
        case IRON_NODE_WHILE:
        case IRON_NODE_FOR: {
            Iron_Node *body;
            if (n->kind == IRON_NODE_WHILE) {
                aw_expr(ctx, ((Iron_WhileStmt *)n)->condition, s, reported);
                body = ((Iron_WhileStmt *)n)->body;
            } else {
                aw_expr(ctx, ((Iron_ForStmt *)n)->iterable, s, reported);
                body = ((Iron_ForStmt *)n)->body;
            }
            /* Two passes: the second iteration sees handles the first
             * awaited. The loop may also run zero times. */
            AwaitState it = aw_copy(s);
            it.dead = false;
            aw_block(ctx, body, &it, reported);
            AwaitState it2 = aw_copy(&it);
            it2.dead = false;
            aw_join(&it2, s);
            aw_block(ctx, body, &it2, reported);
            aw_join(s, &it);
            aw_join(s, &it2);
            arrfree(it.names);
            arrfree(it2.names);
            break;
        }
        case IRON_NODE_DEFER:
            aw_stmt(ctx, ((Iron_DeferStmt *)n)->expr, s, reported);
            break;
        default:
            /* Expression statements. */
            aw_expr(ctx, n, s, reported);
            break;
    }
}

static void check_await_once(ConcurrencyCtx *ctx, Iron_Node *body) {
    AwaitState s = { NULL, false };
    AwaitReported *reported = NULL;
    aw_stmt(ctx, body, &s, &reported);
    arrfree(s.names);
    hmfree(reported);
}

static void analyze_function(ConcurrencyCtx *ctx, Iron_Node *body_node) {
    if (!body_node || body_node->kind != IRON_NODE_BLOCK) return;
    Iron_Block *body = (Iron_Block *)body_node;
    walk_stmts(ctx, body->stmts, body->stmt_count);
    check_await_once(ctx, body_node);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void iron_concurrency_check(Iron_Program *program, Iron_Scope *global_scope,
                            Iron_Arena *arena, Iron_DiagList *diags,
                            const _Atomic bool *cancel_flag) {
    if (!program) return;
    /* HARD-05: pre-entry cancel check. */
    if (iron_cancel_requested(cancel_flag)) return;
    (void)global_scope;

    ConcurrencyCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena        = arena;
    ctx.diags        = diags;
    ctx.local_names  = NULL;
    ctx.in_parallel  = false;
    ctx.in_spawn     = false;
    ctx.spawn_writes = NULL;
    ctx.spawn_reads  = NULL;
    ctx.cancel_flag  = cancel_flag;
    ctx.par_loop_var = NULL;
    ctx.program      = program;

    for (int i = 0; i < program->decl_count; i++) {
        /* HARD-05: cancel poll inside top-level decl loop. */
        if (iron_cancel_requested(cancel_flag)) break;
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;
        switch ((int)(decl->kind)) {
            case IRON_NODE_FUNC_DECL: {
                Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
                if (fd->body) analyze_function(&ctx, fd->body);
                break;
            }
            case IRON_NODE_METHOD_DECL: {
                Iron_MethodDecl *md = (Iron_MethodDecl *)decl;
                if (md->body) analyze_function(&ctx, md->body);
                break;
            }
            /* -Wswitch-enum opt-out: concurrency analysis only runs on
             * function and method bodies. */
            default:
                break;
        }
    }

    arrfree(ctx.local_names);
    arrfree(ctx.spawn_writes);
    arrfree(ctx.spawn_reads);
}
