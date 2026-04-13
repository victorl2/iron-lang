/* web_top_level_loader_check.c — WEB-ASSET-03 analyzer pass.
 *
 * Walks the top-level declarations of an Iron program and emits a
 * compile-time error (E0502) if any of the raylib resource-loader functions
 * (LoadTexture, LoadSound, LoadFont, LoadModel) appears as a direct call
 * expression outside any function body when --target=web.
 *
 * Design notes:
 *
 *   1. The pass skips IRON_NODE_FUNC_DECL nodes entirely: function declarations
 *      are not calls, and anything inside a function body is function-scoped
 *      and therefore allowed.
 *
 *   2. For every other top-level declaration (VAL_DECL, VAR_DECL, ASSIGN,
 *      standalone CALL, etc.) the pass recursively scans the subtree looking
 *      for IRON_NODE_CALL nodes whose callee is an IRON_NODE_IDENT matching
 *      one of the four forbidden names.
 *
 *   3. As of Iron v1.1.0-alpha the language does not support module-level
 *      executable statements: all executable code lives inside function
 *      bodies.  The check is therefore vacuously true in practice for the
 *      current language surface.  It is implemented as infrastructure for
 *      the case where Iron gains module-level expressions (global val/var
 *      initialisers, module-level call statements).
 *
 *   4. Name matching uses strcmp (case-sensitive, exact match).
 *      "loadtexture" or "LoadTexture2D" do NOT trigger the check.
 *
 * On IRON_TARGET_NATIVE this pass is a zero-cost early return.
 */

#include "analyzer/web_top_level_loader_check.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#define IRON_DIAG_E0502_TOP_LEVEL_LOADER_ON_WEB 502

/* ── Forbidden callee names ──────────────────────────────────────────────────
 * These are the raylib resource-loader functions that must NOT be called at
 * module level for --target=web.  They would race with --preload-file MEMFS
 * mounting, which completes asynchronously before emscripten_set_main_loop
 * fires.  Calling them before mounting completes causes silent failures or
 * hard aborts at runtime.
 */
static const char *const FORBIDDEN_LOADERS[] = {
    "LoadTexture",
    "LoadSound",
    "LoadFont",
    "LoadModel",
};
#define FORBIDDEN_LOADER_COUNT ((int)(sizeof(FORBIDDEN_LOADERS) / sizeof(FORBIDDEN_LOADERS[0])))

/* ── Context ─────────────────────────────────────────────────────────────── */

typedef struct {
    Iron_Arena    *arena;
    Iron_DiagList *diags;
} WebLoaderCtx;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static int is_forbidden_name(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < FORBIDDEN_LOADER_COUNT; i++) {
        if (strcmp(name, FORBIDDEN_LOADERS[i]) == 0) return 1;
    }
    return 0;
}

static void emit_loader_error(WebLoaderCtx *ctx, const char *callee_name,
                               Iron_Span span) {
    char msg[512];
    snprintf(msg, sizeof(msg),
             "top-level `%s` is forbidden for --target=web\n"
             "web builds preload assets asynchronously via --preload-file, "
             "so calling these functions at module level races with MEMFS mounting.\n"
             "move the call inside a function (typically `main()` after `InitWindow`).",
             callee_name);

    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                   IRON_DIAG_E0502_TOP_LEVEL_LOADER_ON_WEB, span,
                   iron_arena_strdup(ctx->arena, msg, strlen(msg)), NULL);
}

/* ── AST walker ──────────────────────────────────────────────────────────── */

/* Forward declaration for mutual recursion. */
static void scan_node(WebLoaderCtx *ctx, Iron_Node *node);

static void scan_node(WebLoaderCtx *ctx, Iron_Node *node) {
    if (!node) return;

    switch ((int)(node->kind)) {
        /* ── Call expression: check callee name ─────────────────────────── */
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)node;

            if (ce->callee && ce->callee->kind == IRON_NODE_IDENT) {
                const char *callee_name = ((Iron_Ident *)ce->callee)->name;
                if (is_forbidden_name(callee_name)) {
                    emit_loader_error(ctx, callee_name, ce->span);
                }
            }

            /* Recurse into callee expr and arguments in case of nested calls */
            scan_node(ctx, ce->callee);
            for (int i = 0; i < ce->arg_count; i++) {
                scan_node(ctx, ce->args[i]);
            }
            break;
        }

        /* ── Block: recurse into every statement ─────────────────────────── */
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            for (int i = 0; i < blk->stmt_count; i++) {
                scan_node(ctx, blk->stmts[i]);
            }
            break;
        }

        /* ── Statement containers ────────────────────────────────────────── */
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            scan_node(ctx, vd->init);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            scan_node(ctx, vd->init);
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            scan_node(ctx, rs->value);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            scan_node(ctx, as->target);
            scan_node(ctx, as->value);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            scan_node(ctx, is->condition);
            scan_node(ctx, is->body);
            for (int i = 0; i < is->elif_count; i++) {
                scan_node(ctx, is->elif_conds[i]);
                scan_node(ctx, is->elif_bodies[i]);
            }
            scan_node(ctx, is->else_body);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            scan_node(ctx, ws->condition);
            scan_node(ctx, ws->body);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            scan_node(ctx, fs->iterable);
            scan_node(ctx, (Iron_Node *)fs->body);
            break;
        }
        /* ── Default: leaf or unrecognised container — no-op ─────────────── */
        /* -Wswitch-enum opt-out: web-top-level-loader BFS only needs to
         * descend into kinds that can call an async loader. */
        default:
            break;
    }
}

/* ── Public entry point ──────────────────────────────────────────────────── */

void iron_web_top_level_loader_check(Iron_Program *program, Iron_Arena *arena,
                                     Iron_DiagList *diags,
                                     IronBuildTarget target) {
    /* On native targets this pass is a zero-cost no-op. */
    if (target != IRON_TARGET_WEB) return;
    if (!program || program->decl_count == 0) return;

    WebLoaderCtx ctx;
    ctx.arena = arena;
    ctx.diags = diags;

    /* Walk only module-level declarations.
     *
     * IRON_NODE_FUNC_DECL nodes are SKIPPED: function declarations are not
     * calls, and anything inside their body is function-scoped (allowed).
     *
     * Any other kind of top-level decl (VAL_DECL, VAR_DECL, ASSIGN, CALL,
     * etc.) is recursively scanned for forbidden call expressions.
     *
     * Note: as of v1.1.0-alpha Iron does not support module-level executable
     * statements — all executable code lives inside function bodies — so this
     * loop is vacuously a no-op for the current language surface.  It is
     * implemented as infrastructure for when module-level expressions are
     * added.
     */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;

        /* Skip function declarations: calls inside a function body are fine. */
        if (decl->kind == IRON_NODE_FUNC_DECL) continue;

        scan_node(&ctx, decl);
    }
}
