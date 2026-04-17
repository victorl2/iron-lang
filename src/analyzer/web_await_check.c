/* web_await_check.c — WEB-RUNTIME-04 analyzer pass.
 *
 * Walks the call graph from the program entry point (`main`) via BFS and
 * emits a compile-time error (E0501) if any `await` expression is reachable
 * when the build target is IRON_TARGET_WEB.
 *
 * Rationale: web builds use Emscripten's cooperative scheduler and cannot
 * suspend the main thread. `await` at runtime would deadlock; catching it
 * at compile time gives a clear, actionable error with a call-chain trace.
 *
 * On IRON_TARGET_NATIVE this pass is a zero-cost early return.
 */

#include "analyzer/web_await_check.h"
#include "vendor/stb_ds.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#define IRON_DIAG_E0501_AWAIT_ON_WEB 501

/* ── Context ─────────────────────────────────────────────────────────────── */

typedef struct {
    Iron_Arena    *arena;
    Iron_DiagList *diags;

    /* Hash map: function name -> Iron_FuncDecl * (stb_ds shmap) */
    struct { char *key; Iron_FuncDecl *value; } *func_by_name;

    /* Hash set: function names already visited, to prevent cycles (stb_ds shmap) */
    struct { char *key; int value; }            *visited;

    /* Call-chain stack for error rendering (stb_ds array of string pointers) */
    const char **chain;
} WebAwaitCtx;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Render the current call chain as "main -> A -> B -> fn_name"
 * and emit IRON_DIAG_E0501_AWAIT_ON_WEB. */
static void emit_await_error(WebAwaitCtx *ctx, const char *fn_name,
                              Iron_Span span) {
    /* Build chain string: entries in ctx->chain joined by " -> " */
    char chain_buf[512];
    chain_buf[0] = '\0';
    int chain_len = (int)arrlen(ctx->chain);
    int offset    = 0;
    for (int i = 0; i < chain_len && offset < (int)sizeof(chain_buf) - 1; i++) {
        if (i > 0) {
            int written = snprintf(chain_buf + offset,
                                   sizeof(chain_buf) - (size_t)offset, " -> ");
            if (written > 0) offset += written;
        }
        int written = snprintf(chain_buf + offset,
                               sizeof(chain_buf) - (size_t)offset,
                               "%s", ctx->chain[i]);
        if (written > 0) offset += written;
    }

    char msg[1024];
    snprintf(msg, sizeof(msg),
             "`await` is not supported for --target=web\n"
             "  in function `%s` at <source>\n"
             "  reached from main via %s\n"
             "web builds use Emscripten's cooperative scheduler and cannot "
             "suspend the main thread.",
             fn_name ? fn_name : "(anon)",
             chain_buf);

    const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
    if (!msg_copy) iron_oom_abort("web_await_check.c:emit_await_error msg");
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                   IRON_DIAG_E0501_AWAIT_ON_WEB, span,
                   msg_copy, NULL);
}

/* ── AST walker ──────────────────────────────────────────────────────────── */

/* Forward declaration so scan_node and the call-descent can be mutually recursive. */
static void scan_node(WebAwaitCtx *ctx, Iron_FuncDecl *enclosing,
                      Iron_Node *node);

static void scan_node(WebAwaitCtx *ctx, Iron_FuncDecl *enclosing,
                      Iron_Node *node) {
    if (!node) return;

    switch ((int)(node->kind)) {
        /* ── The target: flag every await expression ─────────────────────── */
        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *ae = (Iron_AwaitExpr *)node;
            emit_await_error(ctx,
                             enclosing ? enclosing->name : "(anon)",
                             ae->span);
            /* Still recurse into the handle in case it contains more awaits */
            scan_node(ctx, enclosing, ae->handle);
            break;
        }

        /* ── Call expression: follow named callees into the BFS ─────────── */
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)node;

            /* If the callee is a plain identifier, try to follow it */
            if (ce->callee && ce->callee->kind == IRON_NODE_IDENT) {
                const char *callee_name = ((Iron_Ident *)ce->callee)->name;
                if (callee_name) {
                    Iron_FuncDecl *callee_decl =
                        shget(ctx->func_by_name, callee_name);
                    int already_visited =
                        (int)(uintptr_t)shget(ctx->visited, callee_name);
                    if (callee_decl && !already_visited &&
                        callee_decl->body) {
                        shput(ctx->visited,  callee_name, 1);
                        arrpush(ctx->chain,  callee_name);
                        scan_node(ctx, callee_decl, callee_decl->body);
                        arrpop(ctx->chain);
                    }
                }
            }

            /* Unconditionally recurse into callee expr and every argument */
            scan_node(ctx, enclosing, ce->callee);
            for (int i = 0; i < ce->arg_count; i++) {
                scan_node(ctx, enclosing, ce->args[i]);
            }
            break;
        }

        /* ── Block: recurse into every statement ─────────────────────────── */
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            for (int i = 0; i < blk->stmt_count; i++) {
                scan_node(ctx, enclosing, blk->stmts[i]);
            }
            break;
        }

        /* ── Statement containers ────────────────────────────────────────── */
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            scan_node(ctx, enclosing, vd->init);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            scan_node(ctx, enclosing, vd->init);
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            scan_node(ctx, enclosing, rs->value);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            scan_node(ctx, enclosing, as->target);
            scan_node(ctx, enclosing, as->value);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            scan_node(ctx, enclosing, is->condition);
            scan_node(ctx, enclosing, is->body);
            for (int i = 0; i < is->elif_count; i++) {
                scan_node(ctx, enclosing, is->elif_conds[i]);
                scan_node(ctx, enclosing, is->elif_bodies[i]);
            }
            scan_node(ctx, enclosing, is->else_body);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            scan_node(ctx, enclosing, ws->condition);
            scan_node(ctx, enclosing, ws->body);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            scan_node(ctx, enclosing, fs->iterable);
            scan_node(ctx, enclosing, (Iron_Node *)fs->body);
            break;
        }
        /* HARD-04: graceful no-op on parser ErrorNode. */
        case IRON_NODE_ERROR:
            break;

        /* HARD-04: sentinel — never a real node kind. */
        case IRON_NODE_COUNT:
            break;

        /* ── Default: leaf or unrecognised container — no-op ─────────────── */
        /* -Wswitch-enum opt-out: web-await BFS only needs to descend into
         * expression / control-flow kinds that can contain an AWAIT. Leaf
         * kinds (literals, idents, field accesses with no child expr) are
         * intentional no-ops here. */
        default:
            break;
    }
}

/* ── Public entry point ──────────────────────────────────────────────────── */

void iron_web_await_check(Iron_Program *program, Iron_Arena *arena,
                          Iron_DiagList *diags, IronBuildTarget target) {
    /* On native targets this pass is a zero-cost no-op. */
    if (target != IRON_TARGET_WEB) return;
    if (!program || program->decl_count == 0) return;

    WebAwaitCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena = arena;
    ctx.diags = diags;

    /* Build function-by-name map from top-level declarations */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;
        if (decl->kind == IRON_NODE_FUNC_DECL) {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
            if (fd->name) {
                shput(ctx.func_by_name, fd->name, fd);
            }
        }
    }

    /* Find the program entry point: func main() */
    Iron_FuncDecl *entry = shget(ctx.func_by_name, "main");
    if (!entry || !entry->body) goto done;

    /* BFS starting from main */
    shput(ctx.visited, "main", 1);
    arrpush(ctx.chain, "main");
    scan_node(&ctx, entry, entry->body);
    arrpop(ctx.chain);

done:
    shfree(ctx.func_by_name);
    shfree(ctx.visited);
    arrfree(ctx.chain);
}
