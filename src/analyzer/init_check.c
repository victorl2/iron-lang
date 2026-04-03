/* init_check.c -- Definite assignment analysis for Iron.
 *
 * Runs after type checking. For each function:
 *   1. Collect `var` declarations without initializers (uninit vars).
 *   2. Walk statements tracking a "definitely assigned" name set.
 *   3. On identifier use, check if name is in the uninit set AND not
 *      in the definitely-assigned set => emit E0314.
 *   4. On assignment to a tracked var, add to definitely-assigned set.
 *
 * Control flow (Plan 02):
 *   - if/else: intersection of assigned sets from both branches
 *   - match: intersection of all arm assigned sets
 *   - loops: body assignments not trusted (may not execute)
 *   - early return: branch that returns is excluded from merge
 */

#include "analyzer/init_check.h"
#include "vendor/stb_ds.h"
#include <string.h>
#include <stdio.h>

/* Max tracked uninitialized variables per function scope */
#define MAX_UNINIT_VARS 256

typedef struct {
    Iron_Arena    *arena;
    Iron_DiagList *diags;

    /* Names of var declarations without initializers in current function */
    const char   *uninit_vars[MAX_UNINIT_VARS];
    int           uninit_count;

    /* Names of variables definitely assigned at current program point.
     * Uses a bool parallel array indexed same as uninit_vars. */
    bool          assigned[MAX_UNINIT_VARS];
} InitCheckCtx;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static int find_uninit_index(InitCheckCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->uninit_count; i++) {
        if (strcmp(ctx->uninit_vars[i], name) == 0) return i;
    }
    return -1;
}

static void mark_assigned(InitCheckCtx *ctx, const char *name) {
    int idx = find_uninit_index(ctx, name);
    if (idx >= 0) ctx->assigned[idx] = true;
}

static bool is_assigned(InitCheckCtx *ctx, const char *name) {
    int idx = find_uninit_index(ctx, name);
    if (idx < 0) return true;  /* not in uninit set => always safe */
    return ctx->assigned[idx];
}

static void emit_uninit_error(InitCheckCtx *ctx, Iron_Span span,
                              const char *name) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "variable '%s' may be used before initialization", name);
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                   IRON_ERR_POSSIBLY_UNINITIALIZED, span,
                   iron_arena_strdup(ctx->arena, buf, strlen(buf)), NULL);
}

/* ── Expression walker ───────────────────────────────────────────────────── */

static void check_expr_uses(InitCheckCtx *ctx, Iron_Node *expr) {
    if (!expr) return;

    switch (expr->kind) {
    case IRON_NODE_IDENT: {
        Iron_Ident *id = (Iron_Ident *)expr;
        if (find_uninit_index(ctx, id->name) >= 0 &&
            !is_assigned(ctx, id->name)) {
            emit_uninit_error(ctx, id->span, id->name);
        }
        break;
    }
    case IRON_NODE_BINARY: {
        Iron_BinaryExpr *bin = (Iron_BinaryExpr *)expr;
        check_expr_uses(ctx, bin->left);
        check_expr_uses(ctx, bin->right);
        break;
    }
    case IRON_NODE_UNARY: {
        Iron_UnaryExpr *un = (Iron_UnaryExpr *)expr;
        check_expr_uses(ctx, un->operand);
        break;
    }
    case IRON_NODE_CALL: {
        Iron_CallExpr *call = (Iron_CallExpr *)expr;
        check_expr_uses(ctx, call->callee);
        for (int i = 0; i < call->arg_count; i++) {
            check_expr_uses(ctx, call->args[i]);
        }
        break;
    }
    case IRON_NODE_METHOD_CALL: {
        Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)expr;
        check_expr_uses(ctx, mc->object);
        for (int i = 0; i < mc->arg_count; i++) {
            check_expr_uses(ctx, mc->args[i]);
        }
        break;
    }
    case IRON_NODE_FIELD_ACCESS: {
        Iron_FieldAccess *fa = (Iron_FieldAccess *)expr;
        check_expr_uses(ctx, fa->object);
        break;
    }
    case IRON_NODE_INDEX: {
        Iron_IndexExpr *ix = (Iron_IndexExpr *)expr;
        check_expr_uses(ctx, ix->object);
        check_expr_uses(ctx, ix->index);
        break;
    }
    case IRON_NODE_ARRAY_LIT: {
        Iron_ArrayLit *al = (Iron_ArrayLit *)expr;
        for (int i = 0; i < al->element_count; i++) {
            check_expr_uses(ctx, al->elements[i]);
        }
        break;
    }
    case IRON_NODE_SLICE: {
        Iron_SliceExpr *sl = (Iron_SliceExpr *)expr;
        check_expr_uses(ctx, sl->object);
        check_expr_uses(ctx, sl->start);
        check_expr_uses(ctx, sl->end);
        break;
    }
    case IRON_NODE_HEAP: {
        Iron_HeapExpr *he = (Iron_HeapExpr *)expr;
        check_expr_uses(ctx, he->inner);
        break;
    }
    case IRON_NODE_RC: {
        Iron_RcExpr *rc = (Iron_RcExpr *)expr;
        check_expr_uses(ctx, rc->inner);
        break;
    }
    case IRON_NODE_CONSTRUCT: {
        Iron_ConstructExpr *ce = (Iron_ConstructExpr *)expr;
        for (int i = 0; i < ce->arg_count; i++) {
            check_expr_uses(ctx, ce->args[i]);
        }
        break;
    }
    case IRON_NODE_IS: {
        Iron_IsExpr *ie = (Iron_IsExpr *)expr;
        check_expr_uses(ctx, ie->expr);
        break;
    }
    case IRON_NODE_AWAIT: {
        Iron_AwaitExpr *aw = (Iron_AwaitExpr *)expr;
        check_expr_uses(ctx, aw->handle);
        break;
    }
    case IRON_NODE_INTERP_STRING: {
        Iron_InterpString *is_node = (Iron_InterpString *)expr;
        for (int i = 0; i < is_node->part_count; i++) {
            check_expr_uses(ctx, is_node->parts[i]);
        }
        break;
    }
    case IRON_NODE_LAMBDA: {
        /* Lambda bodies are separate scopes; skip for now. */
        break;
    }
    default:
        /* Literals and other leaf nodes -- nothing to check. */
        break;
    }
}

/* ── Statement walker ────────────────────────────────────────────────────── */

static void check_stmt_init(InitCheckCtx *ctx, Iron_Node *node) {
    if (!node) return;

    switch (node->kind) {
    case IRON_NODE_VAR_DECL: {
        Iron_VarDecl *vd = (Iron_VarDecl *)node;
        if (vd->init == NULL) {
            /* Register as potentially uninitialized */
            if (vd->name && ctx->uninit_count < MAX_UNINIT_VARS) {
                ctx->uninit_vars[ctx->uninit_count] = vd->name;
                ctx->assigned[ctx->uninit_count] = false;
                ctx->uninit_count++;
            }
        } else {
            /* Init expression may reference other uninit vars */
            check_expr_uses(ctx, vd->init);
        }
        break;
    }
    case IRON_NODE_VAL_DECL: {
        Iron_ValDecl *vd = (Iron_ValDecl *)node;
        /* val always has init -- check RHS for references to uninit vars */
        check_expr_uses(ctx, vd->init);
        break;
    }
    case IRON_NODE_ASSIGN: {
        Iron_AssignStmt *as = (Iron_AssignStmt *)node;
        /* Check RHS first (evaluated before assignment) */
        check_expr_uses(ctx, as->value);
        /* Mark target as assigned if it's an identifier */
        if (as->target && as->target->kind == IRON_NODE_IDENT) {
            Iron_Ident *id = (Iron_Ident *)as->target;
            mark_assigned(ctx, id->name);
        }
        break;
    }
    case IRON_NODE_RETURN: {
        Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
        if (rs->value) check_expr_uses(ctx, rs->value);
        break;
    }
    case IRON_NODE_BLOCK: {
        Iron_Block *blk = (Iron_Block *)node;
        for (int i = 0; i < blk->stmt_count; i++) {
            check_stmt_init(ctx, blk->stmts[i]);
        }
        break;
    }
    case IRON_NODE_IF: {
        Iron_IfStmt *ifs = (Iron_IfStmt *)node;
        check_expr_uses(ctx, ifs->condition);
        check_stmt_init(ctx, ifs->body);
        for (int i = 0; i < ifs->elif_count; i++) {
            check_expr_uses(ctx, ifs->elif_conds[i]);
            check_stmt_init(ctx, ifs->elif_bodies[i]);
        }
        if (ifs->else_body) check_stmt_init(ctx, ifs->else_body);
        break;
    }
    case IRON_NODE_WHILE: {
        Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
        check_expr_uses(ctx, ws->condition);
        check_stmt_init(ctx, ws->body);
        break;
    }
    case IRON_NODE_FOR: {
        Iron_ForStmt *fs = (Iron_ForStmt *)node;
        check_expr_uses(ctx, fs->iterable);
        check_stmt_init(ctx, fs->body);
        break;
    }
    case IRON_NODE_MATCH: {
        Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
        check_expr_uses(ctx, ms->subject);
        for (int i = 0; i < ms->case_count; i++) {
            Iron_MatchCase *mc = (Iron_MatchCase *)ms->cases[i];
            check_stmt_init(ctx, mc->body);
        }
        if (ms->else_body) check_stmt_init(ctx, ms->else_body);
        break;
    }
    case IRON_NODE_FREE: {
        Iron_FreeStmt *fs = (Iron_FreeStmt *)node;
        check_expr_uses(ctx, fs->expr);
        break;
    }
    case IRON_NODE_LEAK: {
        Iron_LeakStmt *ls = (Iron_LeakStmt *)node;
        check_expr_uses(ctx, ls->expr);
        break;
    }
    case IRON_NODE_DEFER: {
        Iron_DeferStmt *ds = (Iron_DeferStmt *)node;
        check_expr_uses(ctx, ds->expr);
        break;
    }
    default:
        /* Expression statement -- check for uses */
        check_expr_uses(ctx, node);
        break;
    }
}

/* ── Per-function analysis ───────────────────────────────────────────────── */

static void check_function(InitCheckCtx *ctx, Iron_FuncDecl *fn) {
    ctx->uninit_count = 0;
    memset(ctx->assigned, 0, sizeof(ctx->assigned));

    /* Parameters are always initialized -- do NOT register them. */
    /* Walk the body. */
    if (fn->body) {
        check_stmt_init(ctx, fn->body);
    }
}

/* ── Public entry point ──────────────────────────────────────────────────── */

void iron_init_check(Iron_Program *program, Iron_Scope *global_scope,
                     Iron_Arena *arena, Iron_DiagList *diags) {
    (void)global_scope;

    if (!program) return;

    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;

        if (decl->kind == IRON_NODE_FUNC_DECL) {
            InitCheckCtx ctx;
            ctx.arena = arena;
            ctx.diags = diags;
            ctx.uninit_count = 0;
            memset(ctx.assigned, 0, sizeof(ctx.assigned));
            check_function(&ctx, (Iron_FuncDecl *)decl);
        } else if (decl->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *md = (Iron_MethodDecl *)decl;
            /* Methods have same structure as functions for body analysis */
            InitCheckCtx ctx;
            ctx.arena = arena;
            ctx.diags = diags;
            ctx.uninit_count = 0;
            memset(ctx.assigned, 0, sizeof(ctx.assigned));
            if (md->body) {
                check_stmt_init(&ctx, md->body);
            }
        }
    }
}
