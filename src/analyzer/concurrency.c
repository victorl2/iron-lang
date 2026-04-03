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

    /* Spawn capture analysis state */
    bool           in_spawn;
    const char   **spawn_writes;  /* stb_ds: names written inside spawn body */
    const char   **spawn_reads;   /* stb_ds: names read inside spawn body   */
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

static void emit_warn(ConcurrencyCtx *ctx, int code, Iron_Span span,
                      const char *msg) {
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_WARNING, code, span,
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

/* ── Spawn capture analysis: collect outer refs from spawn body ───────────── */

#define MAX_SPAWN_CAPTURES 64

/* Recursively walk a spawn body, recording outer-variable writes and reads. */
static void collect_spawn_refs(ConcurrencyCtx *ctx, Iron_Node *node) {
    if (!node) return;

    /* Bound check: stop collecting if we hit the limit */
    if (arrlen(ctx->spawn_writes) + arrlen(ctx->spawn_reads) >= MAX_SPAWN_CAPTURES)
        return;

    switch (node->kind) {
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
            /* Register as local, then check init for reads */
            arrpush(ctx->local_names, vd->name);
            collect_spawn_refs(ctx, vd->init);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            /* Register as local, then check init for reads */
            arrpush(ctx->local_names, vd->name);
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
        default:
            break;
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
    ctx.arena        = arena;
    ctx.diags        = diags;
    ctx.local_names  = NULL;
    ctx.in_parallel  = false;
    ctx.in_spawn     = false;
    ctx.spawn_writes = NULL;
    ctx.spawn_reads  = NULL;

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
    arrfree(ctx.spawn_writes);
    arrfree(ctx.spawn_reads);
}
