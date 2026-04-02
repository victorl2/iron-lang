/* capture.c — Free variable (capture) analysis pass for Iron.
 *
 * Pass 3b of the semantic pipeline: runs after type-checking and before
 * escape analysis. Identifies all outer-scope variables referenced inside
 * each lambda body and annotates the Iron_LambdaExpr node with the
 * capture set.
 *
 * Algorithm per lambda:
 *   1. Collect names declared INSIDE the lambda (params + val/var decls).
 *   2. Walk every IRON_NODE_IDENT in the body.
 *   3. If the ident's resolved_sym is non-NULL, not a local, and not a
 *      function/type/enum/field/extern symbol, it's a capture.
 *   4. Deduplicate by name, then arena-allocate the final array.
 */

#include "analyzer/capture.h"
#include "vendor/stb_ds.h"

#include <string.h>
#include <stddef.h>

/* ── Context ─────────────────────────────────────────────────────────────── */

typedef struct {
    Iron_Arena    *arena;
    Iron_DiagList *diags;
} CaptureCtx;

/* stb_ds string hashmap entry (int value — we just use it as a set) */
typedef struct { char *key; int value; } StrSet;

/* Temporary capture accumulator entry (uses stb_ds dynamic array) */
typedef struct {
    const char      *name;
    struct Iron_Type *type;
    bool             is_mutable;
} TmpCapture;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Returns true if a symbol kind should never be treated as a capture. */
static bool sym_kind_is_non_capture(Iron_SymbolKind k) {
    switch (k) {
        case IRON_SYM_FUNCTION:
        case IRON_SYM_METHOD:
        case IRON_SYM_TYPE:
        case IRON_SYM_ENUM:
        case IRON_SYM_ENUM_VARIANT:
        case IRON_SYM_INTERFACE:
        case IRON_SYM_FIELD:
            return true;
        default:
            return false;
    }
}

/* ── Local name collection ────────────────────────────────────────────────── */

/* Recursively collect all variable/param names declared inside `node` into
 * the `locals` set. This includes params (handled at call site) and any
 * val/var decls inside the lambda body. */
static void collect_locals(Iron_Node *node, StrSet **locals) {
    if (!node) return;
    switch (node->kind) {
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            shput(*locals, vd->name, 1);
            /* Recurse into init expression (it may contain nested lambdas,
             * but their locals are handled by their own find_captures call) */
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            shput(*locals, vd->name, 1);
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            for (int i = 0; i < blk->stmt_count; i++) {
                collect_locals(blk->stmts[i], locals);
            }
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            collect_locals(is->body, locals);
            for (int i = 0; i < is->elif_count; i++) {
                collect_locals(is->elif_bodies[i], locals);
            }
            collect_locals(is->else_body, locals);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            collect_locals(ws->body, locals);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            /* The loop variable is a local in the for body */
            if (fs->var_name) shput(*locals, fs->var_name, 1);
            collect_locals(fs->body, locals);
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
            for (int i = 0; i < ms->case_count; i++) {
                collect_locals(ms->cases[i], locals);
            }
            collect_locals(ms->else_body, locals);
            break;
        }
        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *mc = (Iron_MatchCase *)node;
            collect_locals(mc->body, locals);
            break;
        }
        /* Do NOT recurse into nested IRON_NODE_LAMBDA — its locals are
         * handled when find_captures processes it separately. */
        default:
            break;
    }
}

/* ── Capture collection ───────────────────────────────────────────────────── */

/* Forward declaration for mutual recursion. */
static void walk_node_for_lambdas(CaptureCtx *ctx, Iron_Node *node);

/* Walk `node` collecting all IRON_NODE_IDENT references that are outer-scope
 * captures into `captures`. `locals` is the set of names defined inside this
 * lambda (params + local decls). */
static void collect_idents(Iron_Node *node, StrSet **locals,
                            TmpCapture **captures, StrSet **seen) {
    if (!node) return;
    switch (node->kind) {
        case IRON_NODE_IDENT: {
            Iron_Ident *id = (Iron_Ident *)node;
            if (!id->resolved_sym) break;
            if (sym_kind_is_non_capture(id->resolved_sym->sym_kind)) break;
            if (id->resolved_sym->is_extern) break;
            /* Skip if declared inside this lambda */
            if (shgeti(*locals, id->name) >= 0) break;
            /* Skip if already recorded */
            if (shgeti(*seen, id->name) >= 0) break;
            /* It's a new capture */
            shput(*seen, id->name, 1);
            TmpCapture cap;
            cap.name       = id->resolved_sym->name;
            cap.type       = id->resolved_sym->type;
            cap.is_mutable = id->resolved_sym->is_mutable;
            arrput(*captures, cap);
            break;
        }
        /* Do NOT recurse into nested IRON_NODE_LAMBDA — the inner lambda
         * will be processed separately by walk_node_for_lambdas. Its own
         * captures from the outer scope will include variables the inner
         * lambda closes over via the outer one. */
        case IRON_NODE_LAMBDA:
            break;

        /* Recurse into all other node types that can contain expressions */
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            for (int i = 0; i < blk->stmt_count; i++) {
                collect_idents(blk->stmts[i], locals, captures, seen);
            }
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            collect_idents(vd->init, locals, captures, seen);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            collect_idents(vd->init, locals, captures, seen);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            collect_idents(as->target, locals, captures, seen);
            collect_idents(as->value,  locals, captures, seen);
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            collect_idents(rs->value, locals, captures, seen);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            collect_idents(is->condition, locals, captures, seen);
            collect_idents(is->body,      locals, captures, seen);
            for (int i = 0; i < is->elif_count; i++) {
                collect_idents(is->elif_conds[i],  locals, captures, seen);
                collect_idents(is->elif_bodies[i], locals, captures, seen);
            }
            collect_idents(is->else_body, locals, captures, seen);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            collect_idents(ws->condition, locals, captures, seen);
            collect_idents(ws->body,      locals, captures, seen);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            collect_idents(fs->iterable, locals, captures, seen);
            collect_idents(fs->body,     locals, captures, seen);
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
            collect_idents(ms->subject, locals, captures, seen);
            for (int i = 0; i < ms->case_count; i++) {
                collect_idents(ms->cases[i], locals, captures, seen);
            }
            collect_idents(ms->else_body, locals, captures, seen);
            break;
        }
        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *mc = (Iron_MatchCase *)node;
            collect_idents(mc->pattern, locals, captures, seen);
            collect_idents(mc->body,    locals, captures, seen);
            break;
        }
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *be = (Iron_BinaryExpr *)node;
            collect_idents(be->left,  locals, captures, seen);
            collect_idents(be->right, locals, captures, seen);
            break;
        }
        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *ue = (Iron_UnaryExpr *)node;
            collect_idents(ue->operand, locals, captures, seen);
            break;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)node;
            collect_idents(ce->callee, locals, captures, seen);
            for (int i = 0; i < ce->arg_count; i++) {
                collect_idents(ce->args[i], locals, captures, seen);
            }
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            collect_idents(mc->object, locals, captures, seen);
            for (int i = 0; i < mc->arg_count; i++) {
                collect_idents(mc->args[i], locals, captures, seen);
            }
            break;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
            collect_idents(fa->object, locals, captures, seen);
            break;
        }
        case IRON_NODE_INDEX: {
            Iron_IndexExpr *ie = (Iron_IndexExpr *)node;
            collect_idents(ie->object, locals, captures, seen);
            collect_idents(ie->index,  locals, captures, seen);
            break;
        }
        case IRON_NODE_SLICE: {
            Iron_SliceExpr *se = (Iron_SliceExpr *)node;
            collect_idents(se->object, locals, captures, seen);
            collect_idents(se->start,  locals, captures, seen);
            collect_idents(se->end,    locals, captures, seen);
            break;
        }
        case IRON_NODE_HEAP: {
            Iron_HeapExpr *he = (Iron_HeapExpr *)node;
            collect_idents(he->inner, locals, captures, seen);
            break;
        }
        case IRON_NODE_RC: {
            Iron_RcExpr *re = (Iron_RcExpr *)node;
            collect_idents(re->inner, locals, captures, seen);
            break;
        }
        case IRON_NODE_COMPTIME: {
            Iron_ComptimeExpr *ce = (Iron_ComptimeExpr *)node;
            collect_idents(ce->inner, locals, captures, seen);
            break;
        }
        case IRON_NODE_IS: {
            Iron_IsExpr *ie = (Iron_IsExpr *)node;
            collect_idents(ie->expr, locals, captures, seen);
            break;
        }
        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *ae = (Iron_AwaitExpr *)node;
            collect_idents(ae->handle, locals, captures, seen);
            break;
        }
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;
            for (int i = 0; i < ce->arg_count; i++) {
                collect_idents(ce->args[i], locals, captures, seen);
            }
            break;
        }
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)node;
            collect_idents(al->size, locals, captures, seen);
            for (int i = 0; i < al->element_count; i++) {
                collect_idents(al->elements[i], locals, captures, seen);
            }
            break;
        }
        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *is = (Iron_InterpString *)node;
            for (int i = 0; i < is->part_count; i++) {
                collect_idents(is->parts[i], locals, captures, seen);
            }
            break;
        }
        case IRON_NODE_FREE: {
            Iron_FreeStmt *fs = (Iron_FreeStmt *)node;
            collect_idents(fs->expr, locals, captures, seen);
            break;
        }
        case IRON_NODE_LEAK: {
            Iron_LeakStmt *ls = (Iron_LeakStmt *)node;
            collect_idents(ls->expr, locals, captures, seen);
            break;
        }
        case IRON_NODE_DEFER: {
            Iron_DeferStmt *ds = (Iron_DeferStmt *)node;
            collect_idents(ds->expr, locals, captures, seen);
            break;
        }
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)node;
            collect_idents(ss->pool_expr, locals, captures, seen);
            collect_idents(ss->body,      locals, captures, seen);
            break;
        }
        default:
            break;
    }
}

/* Core capture analysis for a single lambda node. */
static void find_captures(CaptureCtx *ctx, Iron_LambdaExpr *le) {
    /* Build the locals set: lambda's own params */
    StrSet *locals = NULL;
    for (int i = 0; i < le->param_count; i++) {
        Iron_Node *p = le->params[i];
        if (!p) continue;
        if (p->kind == IRON_NODE_PARAM) {
            Iron_Param *param = (Iron_Param *)p;
            shput(locals, param->name, 1);
        }
    }
    /* Also collect all val/var decls inside the body as locals */
    collect_locals(le->body, &locals);

    /* Collect captures via ident walk */
    TmpCapture *captures = NULL;
    StrSet     *seen     = NULL;
    collect_idents(le->body, &locals, &captures, &seen);

    /* Copy to arena-allocated array */
    int count = arrlen(captures);
    if (count > 0) {
        Iron_CaptureEntry *arr = iron_arena_alloc(
            ctx->arena, (size_t)count * sizeof(Iron_CaptureEntry),
            _Alignof(Iron_CaptureEntry));
        for (int i = 0; i < count; i++) {
            arr[i].name       = iron_arena_strdup(ctx->arena, captures[i].name,
                                                  strlen(captures[i].name));
            arr[i].type       = captures[i].type;
            arr[i].is_mutable = captures[i].is_mutable;
        }
        le->captures      = arr;
        le->capture_count = count;
    } else {
        le->captures      = NULL;
        le->capture_count = 0;
    }

    /* Cleanup stb_ds temporaries */
    shfree(locals);
    shfree(seen);
    arrfree(captures);
}

/* ── Lambda walker ────────────────────────────────────────────────────────── */

/* Recursively walk `node` searching for IRON_NODE_LAMBDA nodes. When found,
 * first recurse into the lambda body to process nested lambdas (inner-out
 * ordering), then process the lambda itself. */
static void walk_node_for_lambdas(CaptureCtx *ctx, Iron_Node *node) {
    if (!node) return;
    switch (node->kind) {
        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *le = (Iron_LambdaExpr *)node;
            /* Process nested lambdas first */
            walk_node_for_lambdas(ctx, le->body);
            /* Now analyze this lambda */
            find_captures(ctx, le);
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            for (int i = 0; i < blk->stmt_count; i++) {
                walk_node_for_lambdas(ctx, blk->stmts[i]);
            }
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            walk_node_for_lambdas(ctx, vd->init);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            walk_node_for_lambdas(ctx, vd->init);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            walk_node_for_lambdas(ctx, as->value);
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            walk_node_for_lambdas(ctx, rs->value);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            walk_node_for_lambdas(ctx, is->condition);
            walk_node_for_lambdas(ctx, is->body);
            for (int i = 0; i < is->elif_count; i++) {
                walk_node_for_lambdas(ctx, is->elif_conds[i]);
                walk_node_for_lambdas(ctx, is->elif_bodies[i]);
            }
            walk_node_for_lambdas(ctx, is->else_body);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            walk_node_for_lambdas(ctx, ws->condition);
            walk_node_for_lambdas(ctx, ws->body);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            walk_node_for_lambdas(ctx, fs->iterable);
            walk_node_for_lambdas(ctx, fs->body);
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
            walk_node_for_lambdas(ctx, ms->subject);
            for (int i = 0; i < ms->case_count; i++) {
                walk_node_for_lambdas(ctx, ms->cases[i]);
            }
            walk_node_for_lambdas(ctx, ms->else_body);
            break;
        }
        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *mc = (Iron_MatchCase *)node;
            walk_node_for_lambdas(ctx, mc->body);
            break;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)node;
            walk_node_for_lambdas(ctx, ce->callee);
            for (int i = 0; i < ce->arg_count; i++) {
                walk_node_for_lambdas(ctx, ce->args[i]);
            }
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            walk_node_for_lambdas(ctx, mc->object);
            for (int i = 0; i < mc->arg_count; i++) {
                walk_node_for_lambdas(ctx, mc->args[i]);
            }
            break;
        }
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *be = (Iron_BinaryExpr *)node;
            walk_node_for_lambdas(ctx, be->left);
            walk_node_for_lambdas(ctx, be->right);
            break;
        }
        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *ue = (Iron_UnaryExpr *)node;
            walk_node_for_lambdas(ctx, ue->operand);
            break;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
            walk_node_for_lambdas(ctx, fa->object);
            break;
        }
        case IRON_NODE_INDEX: {
            Iron_IndexExpr *ie = (Iron_IndexExpr *)node;
            walk_node_for_lambdas(ctx, ie->object);
            walk_node_for_lambdas(ctx, ie->index);
            break;
        }
        case IRON_NODE_SLICE: {
            Iron_SliceExpr *se = (Iron_SliceExpr *)node;
            walk_node_for_lambdas(ctx, se->object);
            walk_node_for_lambdas(ctx, se->start);
            walk_node_for_lambdas(ctx, se->end);
            break;
        }
        case IRON_NODE_HEAP: {
            Iron_HeapExpr *he = (Iron_HeapExpr *)node;
            walk_node_for_lambdas(ctx, he->inner);
            break;
        }
        case IRON_NODE_RC: {
            Iron_RcExpr *re = (Iron_RcExpr *)node;
            walk_node_for_lambdas(ctx, re->inner);
            break;
        }
        case IRON_NODE_COMPTIME: {
            Iron_ComptimeExpr *ce = (Iron_ComptimeExpr *)node;
            walk_node_for_lambdas(ctx, ce->inner);
            break;
        }
        case IRON_NODE_IS: {
            Iron_IsExpr *ie = (Iron_IsExpr *)node;
            walk_node_for_lambdas(ctx, ie->expr);
            break;
        }
        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *ae = (Iron_AwaitExpr *)node;
            walk_node_for_lambdas(ctx, ae->handle);
            break;
        }
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;
            for (int i = 0; i < ce->arg_count; i++) {
                walk_node_for_lambdas(ctx, ce->args[i]);
            }
            break;
        }
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)node;
            walk_node_for_lambdas(ctx, al->size);
            for (int i = 0; i < al->element_count; i++) {
                walk_node_for_lambdas(ctx, al->elements[i]);
            }
            break;
        }
        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *is = (Iron_InterpString *)node;
            for (int i = 0; i < is->part_count; i++) {
                walk_node_for_lambdas(ctx, is->parts[i]);
            }
            break;
        }
        case IRON_NODE_FREE: {
            Iron_FreeStmt *fs = (Iron_FreeStmt *)node;
            walk_node_for_lambdas(ctx, fs->expr);
            break;
        }
        case IRON_NODE_LEAK: {
            Iron_LeakStmt *ls = (Iron_LeakStmt *)node;
            walk_node_for_lambdas(ctx, ls->expr);
            break;
        }
        case IRON_NODE_DEFER: {
            Iron_DeferStmt *ds = (Iron_DeferStmt *)node;
            walk_node_for_lambdas(ctx, ds->expr);
            break;
        }
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)node;
            walk_node_for_lambdas(ctx, ss->pool_expr);
            walk_node_for_lambdas(ctx, ss->body);
            break;
        }
        default:
            break;
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void iron_capture_analyze(Iron_Program *program, Iron_Scope *global_scope,
                          Iron_Arena *arena, Iron_DiagList *diags) {
    if (!program) return;
    (void)global_scope; /* not needed: resolved_sym already attached to idents */

    CaptureCtx ctx;
    ctx.arena = arena;
    ctx.diags = diags;

    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;
        switch (decl->kind) {
            case IRON_NODE_FUNC_DECL: {
                Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
                if (fd->body) walk_node_for_lambdas(&ctx, fd->body);
                break;
            }
            case IRON_NODE_METHOD_DECL: {
                Iron_MethodDecl *md = (Iron_MethodDecl *)decl;
                if (md->body) walk_node_for_lambdas(&ctx, md->body);
                break;
            }
            default:
                break;
        }
    }
}
