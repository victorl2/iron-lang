/* concurrency.c — Concurrency checking pass for Iron.
 *
 * Pass 4 of the semantic pipeline: validates concurrency constructs.
 *
 * Current checks:
 *   1. Parallel-for bodies may not mutate outer non-mutex variables (E0208).
 *      "Outer" means declared outside the parallel-for body block.
 *      Reading outer variables is fine.
 *
 * Future:
 *   - Spawn block capture validation (Phase 3).
 */

#include "analyzer/concurrency.h"
#include "vendor/stb_ds.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>

/* ── Context ─────────────────────────────────────────────────────────────── */

typedef struct {
    Iron_Arena    *arena;
    Iron_DiagList *diags;

    /* Names of variables local to the current parallel-for body.
     * An assignment to a name NOT in this set is an outer mutation. */
    const char   **local_names;   /* stb_ds dynamic array */

    /* Are we currently inside a parallel-for body? */
    bool           in_parallel;
} ConcurrencyCtx;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Recursively extract the root identifier name from an expression.
 * Handles bare identifiers, field access chains, and index expressions. */
static const char *expr_ident_name(Iron_Node *node) {
    if (!node) return NULL;
    switch (node->kind) {
        case IRON_NODE_IDENT:
            return ((Iron_Ident *)node)->name;
        case IRON_NODE_FIELD_ACCESS:
            return expr_ident_name(((Iron_FieldAccess *)node)->object);
        case IRON_NODE_INDEX:
            return expr_ident_name(((Iron_IndexExpr *)node)->object);
        default:
            return NULL;
    }
}

static bool name_is_local(ConcurrencyCtx *ctx, const char *name) {
    int n = arrlen(ctx->local_names);
    for (int i = 0; i < n; i++) {
        if (strcmp(ctx->local_names[i], name) == 0) return true;
    }
    return false;
}

static void emit_err(ConcurrencyCtx *ctx, int code, Iron_Span span,
                     const char *msg) {
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR, code, span,
                   iron_arena_strdup(ctx->arena, msg, strlen(msg)), NULL);
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
            arrpush(ctx->local_names, ((Iron_ValDecl *)s)->name);
        } else if (s->kind == IRON_NODE_VAR_DECL) {
            arrpush(ctx->local_names, ((Iron_VarDecl *)s)->name);
        } else if (s->kind == IRON_NODE_BLOCK) {
            Iron_Block *blk = (Iron_Block *)s;
            collect_local_names(ctx, blk->stmts, blk->stmt_count);
        }
    }
}

/* ── Check assignments inside a parallel-for body ─────────────────────────── */

/* Forward declaration */
static void check_body_stmts(ConcurrencyCtx *ctx, Iron_Node **stmts, int count);

static void check_stmt_for_mutation(ConcurrencyCtx *ctx, Iron_Node *node) {
    if (!node) return;
    switch (node->kind) {
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            if (!as->target) break;

            /* Extract root variable name from the assignment target.
             * Handles bare identifiers, field access chains (obj.field),
             * and index expressions (arr[i]). */
            const char *name = expr_ident_name(as->target);
            if (!name) break;  /* Non-identifier-rooted target; skip */

            /* If the target root is NOT in our local set, it's an outer variable */
            if (!name_is_local(ctx, name)) {
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
        /* Val/var decls within the body are local — already collected. */
        default:
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

static void walk_stmt(ConcurrencyCtx *ctx, Iron_Node *node) {
    if (!node) return;
    switch (node->kind) {
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

                /* Collect all val/var decls inside the body as local */
                if (fs->body->kind == IRON_NODE_BLOCK) {
                    Iron_Block *body = (Iron_Block *)fs->body;
                    collect_local_names(ctx, body->stmts, body->stmt_count);
                }

                /* Check assignments in the body */
                check_stmt_for_mutation(ctx, fs->body);

                /* Restore local names to pre-parallel-for state */
                arrsetlen(ctx->local_names, saved_count);
                ctx->in_parallel = prev_in_parallel;
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

static void analyze_function(ConcurrencyCtx *ctx, Iron_Node *body_node) {
    if (!body_node || body_node->kind != IRON_NODE_BLOCK) return;
    Iron_Block *body = (Iron_Block *)body_node;
    walk_stmts(ctx, body->stmts, body->stmt_count);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void iron_concurrency_check(Iron_Program *program, Iron_Scope *global_scope,
                            Iron_Arena *arena, Iron_DiagList *diags) {
    if (!program) return;
    (void)global_scope;

    ConcurrencyCtx ctx;
    ctx.arena       = arena;
    ctx.diags       = diags;
    ctx.local_names = NULL;
    ctx.in_parallel = false;

    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;
        switch (decl->kind) {
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
            default:
                break;
        }
    }

    arrfree(ctx.local_names);
}
