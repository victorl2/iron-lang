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
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>

/* ── Cancellation helper (HARD-05) ─────────────────────────────────────────── */
static inline bool iron_cancel_requested(const _Atomic bool *flag) {
    return flag != NULL && atomic_load_explicit(flag, memory_order_relaxed);
}

/* ── Context ─────────────────────────────────────────────────────────────── */

typedef struct {
    Iron_Arena         *arena;
    Iron_DiagList      *diags;
    const _Atomic bool *cancel_flag;  /* HARD-05 */
    /* Phase 22 OQ-04: set true when entering a readonly method/func body
     * (or readonly lambda); propagated AST-side per RESEARCH Pitfall 4
     * (TypeCtx not available post-typecheck). */
    bool               readonly_context;
    /* List ownership (#174): a closure that captures an owned list may
     * only borrow it for a call, so a lambda literal passed directly as an
     * argument is fine and any other lambda capturing one is an error. A
     * spawn capturing one must be awaited in the block that starts it. */
    bool               lambda_is_call_arg;
    bool               spawn_awaited_here;
} CaptureCtx;

/* An owned list, or an object that holds one (recursively). */
static bool type_holds_owned_list(const struct Iron_Type *t, int depth) {
    if (!t || depth > 16) return false;
    if (t->kind == IRON_TYPE_NULLABLE) return type_holds_owned_list(t->nullable.inner, depth + 1);
    if (t->kind == IRON_TYPE_ARRAY) return t->array.size < 0 && !t->array.is_bounded;
    if (t->kind == IRON_TYPE_OBJECT && t->object.decl) {
        Iron_ObjectDecl *od = t->object.decl;
        for (int i = 0; i < od->field_count; i++) {
            Iron_Field *f = (Iron_Field *)od->fields[i];
            if (f && type_holds_owned_list(f->resolved_type, depth + 1)) return true;
        }
    }
    return false;
}

/* Does `node` contain `await <name>`? */
static bool awaits_ident(Iron_Node *node, const char *name);

static void report_list_capture(CaptureCtx *ctx, Iron_Span span, const char *name,
                                const char *what, const char *help) {
    char msg[320];
    snprintf(msg, sizeof(msg), "%s captures list '%s', which it may only borrow",
             what, name ? name : "?");
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                   IRON_ERR_LIST_IMPLICIT_COPY, span, msg, help);
}

/* stb_ds string hashmap entry (int value — we just use it as a set) */
typedef struct { char *key; int value; } StrSet;

/* Temporary capture accumulator entry (uses stb_ds dynamic array) */
typedef struct {
    const char      *name;
    struct Iron_Type *type;
    bool             is_mutable;
    bool             is_boxed;
} TmpCapture;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Returns true if a symbol kind should never be treated as a capture. */
static bool sym_kind_is_non_capture(Iron_SymbolKind k) {
    switch ((int)(k)) {
        case IRON_SYM_FUNCTION:
        case IRON_SYM_METHOD:
        case IRON_SYM_TYPE:
        case IRON_SYM_ENUM:
        case IRON_SYM_ENUM_VARIANT:
        case IRON_SYM_INTERFACE:
        case IRON_SYM_FIELD:
            return true;
        /* -Wswitch-enum opt-out: IRON_SYM_VARIABLE and IRON_SYM_PARAM ARE
         * capturable and intentionally fall through to `false`. */
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
    switch ((int)(node->kind)) {
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            /* Tuple destructure (`val (a, b) = ...`) has name == NULL and
             * carries its bindings in binding_names[] (NULL entry = `_`).
             * shput on a NULL key would crash (strlen(NULL)). */
            if (vd->name) shput(*locals, vd->name, 1);
            for (int i = 0; i < vd->binding_count; i++) {
                if (vd->binding_names && vd->binding_names[i])
                    shput(*locals, vd->binding_names[i], 1);
            }
            /* Recurse into init expression (it may contain nested lambdas,
             * but their locals are handled by their own find_captures call) */
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            if (vd->name) shput(*locals, vd->name, 1);
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
            if (fs->var_name2) shput(*locals, fs->var_name2, 1);
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
        /* HARD-04: graceful no-op on parser ErrorNode. */
        case IRON_NODE_ERROR:
            break;

        /* HARD-04: sentinel — never a real node kind. */
        case IRON_NODE_COUNT:
            break;

        /* -Wswitch-enum opt-out: collect_locals only visits statement-shaped
         * kinds that can declare a new name; every other Iron_NodeKind is
         * ignored. */
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
    switch ((int)(node->kind)) {
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
            /* Prefer the typechecker-annotated resolved_type on the ident node.
             * resolver-owned symbols have type=NULL for variables (types are set
             * during type-checking in the typechecker's own scope chain, not back-
             * propagated to the resolver symbol). */
            cap.type       = id->resolved_type
                                 ? id->resolved_type
                                 : id->resolved_sym->type;
            cap.is_mutable = id->resolved_sym->is_mutable;
            /* A captured `var` local moves into a counted cell (#210). */
            cap.is_boxed = false;
            if (cap.is_mutable && id->resolved_sym->decl_node &&
                id->resolved_sym->decl_node->kind == IRON_NODE_VAR_DECL) {
                ((Iron_VarDecl *)id->resolved_sym->decl_node)->is_boxed = true;
                cap.is_boxed = true;
            }
            arrput(*captures, cap);
            break;
        }
        /* Do NOT recurse into a nested IRON_NODE_LAMBDA: it was analyzed
         * first (walk_node_for_lambdas goes inner-out). But every variable it
         * captures from outside this lambda must be captured here too, or
         * the inner closure has nothing to take it from (its env field was
         * filled from an undeclared value). */
        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *inner = (Iron_LambdaExpr *)node;
            for (int i = 0; i < inner->capture_count; i++) {
                Iron_CaptureEntry *ic = &inner->captures[i];
                if (!ic->name || shgeti(*locals, ic->name) >= 0) continue;
                if (shgeti(*seen, ic->name) >= 0) {
                    for (ptrdiff_t k = 0; k < arrlen(*captures); k++)
                        if (strcmp((*captures)[k].name, ic->name) == 0 && ic->is_mutable) {
                            (*captures)[k].is_mutable = true;
                            if (ic->is_boxed) (*captures)[k].is_boxed = true;
                        }
                    continue;
                }
                shput(*seen, ic->name, 1);
                TmpCapture cap;
                cap.name       = ic->name;
                cap.type       = ic->type;
                cap.is_mutable = ic->is_mutable;
                cap.is_boxed   = ic->is_boxed;
                arrput(*captures, cap);
            }
            break;
        }

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
        /* -Wswitch-enum opt-out: collect_idents walks every expression-bearing
         * kind; leaf nodes (literals, self, super) have no identifiers of
         * their own and intentionally fall through. */
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
        if (!arr) { /* HARD-09 REPLACE (capture.c:find_captures arr) */ return; }
        for (int i = 0; i < count; i++) {
            arr[i].name       = iron_arena_strdup(ctx->arena, captures[i].name,
                                                  strlen(captures[i].name));
            if (!arr[i].name) { /* HARD-09 REPLACE (capture.c:find_captures name) */ return; }
            arr[i].type       = captures[i].type;
            arr[i].is_mutable = captures[i].is_mutable;
            arr[i].is_boxed   = captures[i].is_boxed;
        }
        le->captures      = arr;
        le->capture_count = count;

        if (!ctx->lambda_is_call_arg) {
            for (int ci = 0; ci < count; ci++) {
                if (!type_holds_owned_list(arr[ci].type, 0)) continue;
                report_list_capture(ctx, le->span, arr[ci].name, "closure",
                    "a closure bound, returned or stored outlives the borrow: "
                    "share the list as `rc [T]`, or pass the closure directly as an argument");
                break;
            }
        }

        /* Phase 22 OQ-04: if this lambda inherits readonly context, reject
         * mutable captures (var bindings + *var T pointers).
         * §6: closures in readonly methods must not capture var bindings or
         * *var T pointers. Reuses Plan 22-01 IRON_ERR_READONLY_PARAM_MUTATION
         * (277) per CONTEXT decision (closures share parent codes); the hint
         * substring `§6: closures in readonly...` distinguishes closure-capture
         * from body-mutation in error messages. */
        if (ctx->readonly_context) {
            for (int ci = 0; ci < count; ci++) {
                if (arr[ci].is_mutable) {
                    char cap_msg[256];
                    snprintf(cap_msg, sizeof(cap_msg),
                             "cannot capture mutable binding '%s' in readonly closure",
                             arr[ci].name ? arr[ci].name : "?");
                    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                   IRON_ERR_READONLY_PARAM_MUTATION,
                                   le->span, cap_msg,
                                   "a closure in a readonly method cannot capture a var binding or a *var T pointer: capture a val copy instead");
                }
            }
        }
    } else {
        le->captures      = NULL;
        le->capture_count = 0;
    }

    /* Cleanup stb_ds temporaries */
    shfree(locals);
    shfree(seen);
    arrfree(captures);
}

/* Core capture analysis for a spawn block (Iron_SpawnStmt).
 * Spawn blocks can capture outer-scope variables by value (read-only captures
 * for val-bindings, or pointer captures for var-bindings). */
static void find_spawn_captures(CaptureCtx *ctx, Iron_SpawnStmt *ss) {
    /* Build locals: all val/var decls inside the spawn body */
    StrSet *locals = NULL;
    collect_locals(ss->body, &locals);

    /* Collect captures via ident walk */
    TmpCapture *captures = NULL;
    StrSet     *seen     = NULL;
    collect_idents(ss->body, &locals, &captures, &seen);

    /* Copy to arena-allocated array */
    int count = (int)arrlen(captures);
    if (count > 0) {
        Iron_CaptureEntry *arr = iron_arena_alloc(
            ctx->arena, (size_t)count * sizeof(Iron_CaptureEntry),
            _Alignof(Iron_CaptureEntry));
        if (!arr) { /* HARD-09 REPLACE (capture.c:find_spawn_captures arr) */ return; }
        for (int i = 0; i < count; i++) {
            arr[i].name       = iron_arena_strdup(ctx->arena, captures[i].name,
                                                  strlen(captures[i].name));
            if (!arr[i].name) { /* HARD-09 REPLACE (capture.c:find_spawn_captures name) */ return; }
            arr[i].type       = captures[i].type;
            arr[i].is_mutable = captures[i].is_mutable;
            arr[i].is_boxed   = captures[i].is_boxed;
        }
        ss->captures      = arr;
        ss->capture_count = count;

        if (!ctx->spawn_awaited_here) {
            for (int ci = 0; ci < count; ci++) {
                if (!type_holds_owned_list(arr[ci].type, 0)) continue;
                report_list_capture(ctx, ss->span, arr[ci].name, "spawn",
                    "await the handle in the same block (`val h = spawn(...) {...}` "
                    "then `await h`), or share the list as `rc [T]`");
                break;
            }
        }
    } else {
        ss->captures      = NULL;
        ss->capture_count = 0;
    }

    shfree(locals);
    shfree(seen);
    arrfree(captures);
}

/* Core capture analysis for a parallel-for body (Iron_ForStmt with is_parallel).
 * The loop variable is a local; any other outer-scope references are captures. */
static void find_pfor_captures(CaptureCtx *ctx, Iron_ForStmt *fs) {
    /* Build locals: the loop variable + all decls inside the body */
    StrSet *locals = NULL;
    shput(locals, fs->var_name, 1);
    if (fs->var_name2) shput(locals, fs->var_name2, 1);
    collect_locals(fs->body, &locals);

    /* Collect captures via ident walk (also walk iterable for range expr) */
    TmpCapture *captures = NULL;
    StrSet     *seen     = NULL;
    collect_idents(fs->body, &locals, &captures, &seen);

    /* Copy to arena-allocated array */
    int count = (int)arrlen(captures);
    if (count > 0) {
        Iron_CaptureEntry *arr = iron_arena_alloc(
            ctx->arena, (size_t)count * sizeof(Iron_CaptureEntry),
            _Alignof(Iron_CaptureEntry));
        if (!arr) { /* HARD-09 REPLACE (capture.c:find_pfor_captures arr) */ return; }
        for (int i = 0; i < count; i++) {
            arr[i].name       = iron_arena_strdup(ctx->arena, captures[i].name,
                                                  strlen(captures[i].name));
            if (!arr[i].name) { /* HARD-09 REPLACE (capture.c:find_pfor_captures name) */ return; }
            arr[i].type       = captures[i].type;
            arr[i].is_mutable = captures[i].is_mutable;
            arr[i].is_boxed   = captures[i].is_boxed;
        }
        fs->pfor_captures      = arr;
        fs->pfor_capture_count = count;
    } else {
        fs->pfor_captures      = NULL;
        fs->pfor_capture_count = 0;
    }

    shfree(locals);
    shfree(seen);
    arrfree(captures);
}

/* ── Lambda walker ────────────────────────────────────────────────────────── */

/* Recursively walk `node` searching for IRON_NODE_LAMBDA nodes. When found,
 * first recurse into the lambda body to process nested lambdas (inner-out
 * ordering), then process the lambda itself. */
static bool awaits_ident(Iron_Node *node, const char *name) {
    if (!node || !name) return false;
    switch ((int)(node->kind)) {
        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *le = (Iron_LambdaExpr *)node;
            (void)le;
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            for (int i = 0; i < blk->stmt_count; i++) {
                if (awaits_ident(blk->stmts[i], name)) return true;
            }
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            if (awaits_ident(vd->init, name)) return true;
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            if (awaits_ident(vd->init, name)) return true;
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            if (awaits_ident(as->value, name)) return true;
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            if (awaits_ident(rs->value, name)) return true;
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            if (awaits_ident(is->condition, name)) return true;
            if (awaits_ident(is->body, name)) return true;
            for (int i = 0; i < is->elif_count; i++) {
                if (awaits_ident(is->elif_conds[i], name)) return true;
                if (awaits_ident(is->elif_bodies[i], name)) return true;
            }
            if (awaits_ident(is->else_body, name)) return true;
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            if (awaits_ident(ws->condition, name)) return true;
            if (awaits_ident(ws->body, name)) return true;
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            if (awaits_ident(fs->iterable, name)) return true;
            if (awaits_ident(fs->body, name)) return true;
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
            if (awaits_ident(ms->subject, name)) return true;
            for (int i = 0; i < ms->case_count; i++) {
                if (awaits_ident(ms->cases[i], name)) return true;
            }
            if (awaits_ident(ms->else_body, name)) return true;
            break;
        }
        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *mc = (Iron_MatchCase *)node;
            if (awaits_ident(mc->body, name)) return true;
            break;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)node;
            if (awaits_ident(ce->callee, name)) return true;
            for (int i = 0; i < ce->arg_count; i++) {
                if (awaits_ident(ce->args[i], name)) return true;
            }
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            if (awaits_ident(mc->object, name)) return true;
            for (int i = 0; i < mc->arg_count; i++) {
                if (awaits_ident(mc->args[i], name)) return true;
            }
            break;
        }
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *be = (Iron_BinaryExpr *)node;
            if (awaits_ident(be->left, name)) return true;
            if (awaits_ident(be->right, name)) return true;
            break;
        }
        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *ue = (Iron_UnaryExpr *)node;
            if (awaits_ident(ue->operand, name)) return true;
            break;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
            if (awaits_ident(fa->object, name)) return true;
            break;
        }
        case IRON_NODE_INDEX: {
            Iron_IndexExpr *ie = (Iron_IndexExpr *)node;
            if (awaits_ident(ie->object, name)) return true;
            if (awaits_ident(ie->index, name)) return true;
            break;
        }
        case IRON_NODE_SLICE: {
            Iron_SliceExpr *se = (Iron_SliceExpr *)node;
            if (awaits_ident(se->object, name)) return true;
            if (awaits_ident(se->start, name)) return true;
            if (awaits_ident(se->end, name)) return true;
            break;
        }
        case IRON_NODE_HEAP: {
            Iron_HeapExpr *he = (Iron_HeapExpr *)node;
            if (awaits_ident(he->inner, name)) return true;
            break;
        }
        case IRON_NODE_RC: {
            Iron_RcExpr *re = (Iron_RcExpr *)node;
            if (awaits_ident(re->inner, name)) return true;
            break;
        }
        case IRON_NODE_COMPTIME: {
            Iron_ComptimeExpr *ce = (Iron_ComptimeExpr *)node;
            if (awaits_ident(ce->inner, name)) return true;
            break;
        }
        case IRON_NODE_IS: {
            Iron_IsExpr *ie = (Iron_IsExpr *)node;
            if (awaits_ident(ie->expr, name)) return true;
            break;
        }
        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *ae = (Iron_AwaitExpr *)node;
            if (ae->handle && ae->handle->kind == IRON_NODE_IDENT &&
                ((Iron_Ident *)ae->handle)->name &&
                strcmp(((Iron_Ident *)ae->handle)->name, name) == 0)
                return true;
            if (awaits_ident(ae->handle, name)) return true;
            break;
        }
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;
            for (int i = 0; i < ce->arg_count; i++) {
                if (awaits_ident(ce->args[i], name)) return true;
            }
            break;
        }
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)node;
            if (awaits_ident(al->size, name)) return true;
            for (int i = 0; i < al->element_count; i++) {
                if (awaits_ident(al->elements[i], name)) return true;
            }
            break;
        }
        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *is = (Iron_InterpString *)node;
            for (int i = 0; i < is->part_count; i++) {
                if (awaits_ident(is->parts[i], name)) return true;
            }
            break;
        }
        case IRON_NODE_FREE: {
            Iron_FreeStmt *fs = (Iron_FreeStmt *)node;
            if (awaits_ident(fs->expr, name)) return true;
            break;
        }
        case IRON_NODE_LEAK: {
            Iron_LeakStmt *ls = (Iron_LeakStmt *)node;
            if (awaits_ident(ls->expr, name)) return true;
            break;
        }
        case IRON_NODE_DEFER: {
            Iron_DeferStmt *ds = (Iron_DeferStmt *)node;
            if (awaits_ident(ds->expr, name)) return true;
            break;
        }
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)node;
            (void)ss;
            break;
        }
        /* -Wswitch-enum opt-out: walk_node_for_lambdas is a generic AST
         * walker; every kind that does not contain a lambda / spawn / pfor
         * is a valid no-op. */
        default:
            break;
    }
    return false;
}

static void walk_node_for_lambdas(CaptureCtx *ctx, Iron_Node *node) {
    if (!node) return;
    /* HARD-05: cancel poll at recursive walker entry. */
    if (iron_cancel_requested(ctx->cancel_flag)) return;
    switch ((int)(node->kind)) {
        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *le = (Iron_LambdaExpr *)node;
            bool is_arg = ctx->lambda_is_call_arg;
            /* Process nested lambdas first */
            ctx->lambda_is_call_arg = false;
            walk_node_for_lambdas(ctx, le->body);
            ctx->lambda_is_call_arg = is_arg;
            /* Now analyze this lambda */
            find_captures(ctx, le);
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            for (int i = 0; i < blk->stmt_count; i++) {
                Iron_Node *st = blk->stmts[i];
                /* `val h = spawn(...) {...}` awaited later in this block. */
                bool prev = ctx->spawn_awaited_here;
                if (st && st->kind == IRON_NODE_VAL_DECL) {
                    Iron_ValDecl *vd = (Iron_ValDecl *)st;
                    if (vd->init && vd->init->kind == IRON_NODE_SPAWN && vd->name) {
                        bool awaited = false;
                        for (int j = i + 1; j < blk->stmt_count && !awaited; j++)
                            awaited = awaits_ident(blk->stmts[j], vd->name);
                        ctx->spawn_awaited_here = awaited;
                    }
                }
                walk_node_for_lambdas(ctx, st);
                ctx->spawn_awaited_here = prev;
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
            /* Perform capture analysis for parallel-for bodies */
            if (fs->is_parallel) {
                find_pfor_captures(ctx, fs);
            }
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
                bool prev = ctx->lambda_is_call_arg;
                ctx->lambda_is_call_arg = ce->args[i] && ce->args[i]->kind == IRON_NODE_LAMBDA;
                walk_node_for_lambdas(ctx, ce->args[i]);
                ctx->lambda_is_call_arg = prev;
            }
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            walk_node_for_lambdas(ctx, mc->object);
            for (int i = 0; i < mc->arg_count; i++) {
                bool prev = ctx->lambda_is_call_arg;
                ctx->lambda_is_call_arg = mc->args[i] && mc->args[i]->kind == IRON_NODE_LAMBDA;
                walk_node_for_lambdas(ctx, mc->args[i]);
                ctx->lambda_is_call_arg = prev;
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
            bool prev = ctx->spawn_awaited_here;
            if (ae->handle && ae->handle->kind == IRON_NODE_SPAWN) ctx->spawn_awaited_here = true;
            walk_node_for_lambdas(ctx, ae->handle);
            ctx->spawn_awaited_here = prev;
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
            /* Perform capture analysis for spawn block body */
            find_spawn_captures(ctx, ss);
            break;
        }
        /* -Wswitch-enum opt-out: walk_node_for_lambdas is a generic AST
         * walker; every kind that does not contain a lambda / spawn / pfor
         * is a valid no-op. */
        default:
            break;
    }
}

/* ── Verbose report helpers ───────────────────────────────────────────────── */

/* Walk the AST and print capture info for every lambda/spawn/pfor found.
 * Called by iron_capture_verbose_report only when --verbose is active. */
static void verbose_walk(Iron_Node *node, Iron_Arena *arena, int depth) {
    if (!node) return;
    switch ((int)(node->kind)) {
        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *le = (Iron_LambdaExpr *)node;
            fprintf(stderr, "  [capture] lambda at %s:%u captures %d variable(s):\n",
                    le->span.filename ? le->span.filename : "<unknown>",
                    le->span.line,
                    le->capture_count);
            for (int i = 0; i < le->capture_count; i++) {
                const char *kind = le->captures[i].is_mutable ? "var (ref)" : "val (copy)";
                const char *type_str = le->captures[i].type
                    ? iron_type_to_string(le->captures[i].type, arena)
                    : "?";
                fprintf(stderr, "            %s: %s  [%s]\n",
                        le->captures[i].name, type_str, kind);
            }
            /* Recurse into the body for nested lambdas */
            verbose_walk(le->body, arena, depth + 1);
            break;
        }
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)node;
            fprintf(stderr, "  [capture] spawn at %s:%u captures %d variable(s):\n",
                    ss->span.filename ? ss->span.filename : "<unknown>",
                    ss->span.line,
                    ss->capture_count);
            for (int i = 0; i < ss->capture_count; i++) {
                const char *kind = ss->captures[i].is_mutable ? "var (ref)" : "val (copy)";
                const char *type_str = ss->captures[i].type
                    ? iron_type_to_string(ss->captures[i].type, arena)
                    : "?";
                fprintf(stderr, "            %s: %s  [%s]\n",
                        ss->captures[i].name, type_str, kind);
            }
            verbose_walk(ss->pool_expr, arena, depth);
            verbose_walk(ss->body, arena, depth);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            if (fs->is_parallel && fs->pfor_capture_count > 0) {
                fprintf(stderr, "  [capture] pfor at %s:%u captures %d variable(s):\n",
                        fs->span.filename ? fs->span.filename : "<unknown>",
                        fs->span.line,
                        fs->pfor_capture_count);
                for (int i = 0; i < fs->pfor_capture_count; i++) {
                    const char *kind = fs->pfor_captures[i].is_mutable ? "var (ref)" : "val (copy)";
                    const char *type_str = fs->pfor_captures[i].type
                        ? iron_type_to_string(fs->pfor_captures[i].type, arena)
                        : "?";
                    fprintf(stderr, "            %s: %s  [%s]\n",
                            fs->pfor_captures[i].name, type_str, kind);
                }
            }
            verbose_walk(fs->iterable, arena, depth);
            verbose_walk(fs->body, arena, depth);
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            for (int i = 0; i < blk->stmt_count; i++) {
                verbose_walk(blk->stmts[i], arena, depth);
            }
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            verbose_walk(vd->init, arena, depth);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            verbose_walk(vd->init, arena, depth);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            verbose_walk(is->condition, arena, depth);
            verbose_walk(is->body, arena, depth);
            for (int i = 0; i < is->elif_count; i++) {
                verbose_walk(is->elif_bodies[i], arena, depth);
            }
            verbose_walk(is->else_body, arena, depth);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            verbose_walk(ws->body, arena, depth);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            verbose_walk(as->value, arena, depth);
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            verbose_walk(rs->value, arena, depth);
            break;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)node;
            verbose_walk(ce->callee, arena, depth);
            for (int i = 0; i < ce->arg_count; i++) {
                verbose_walk(ce->args[i], arena, depth);
            }
            break;
        }
        /* -Wswitch-enum opt-out: verbose_walk is a debugging dumper that
         * only descends into the kinds it explicitly handles. */
        default:
            break;
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void iron_capture_analyze(Iron_Program *program, Iron_Scope *global_scope,
                          Iron_Arena *arena, Iron_DiagList *diags,
                          const _Atomic bool *cancel_flag) {
    if (!program) return;
    /* HARD-05: pre-entry cancel check. */
    if (iron_cancel_requested(cancel_flag)) return;
    (void)global_scope; /* not needed: resolved_sym already attached to idents */

    CaptureCtx ctx;
    ctx.arena            = arena;
    ctx.diags            = diags;
    ctx.lambda_is_call_arg = false;
    ctx.spawn_awaited_here = false;
    ctx.cancel_flag      = cancel_flag;
    ctx.readonly_context = false;

    for (int i = 0; i < program->decl_count; i++) {
        /* HARD-05: cancel poll inside top-level decl loop. */
        if (iron_cancel_requested(cancel_flag)) return;
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;
        switch ((int)(decl->kind)) {
            case IRON_NODE_FUNC_DECL: {
                Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
                /* Phase 22 OQ-04: set readonly_context from AST is_readonly flag
                 * per RESEARCH Pitfall 4 (TypeCtx not available post-typecheck). */
                bool prev_readonly = ctx.readonly_context;
                ctx.readonly_context = fd->is_readonly;
                if (fd->body) walk_node_for_lambdas(&ctx, fd->body);
                ctx.readonly_context = prev_readonly;
                break;
            }
            case IRON_NODE_METHOD_DECL: {
                Iron_MethodDecl *md = (Iron_MethodDecl *)decl;
                /* Phase 22 OQ-04: set readonly_context from AST is_readonly flag;
                 * pure methods also set readonly_context since pure >= readonly. */
                bool prev_readonly = ctx.readonly_context;
                ctx.readonly_context = (md->is_readonly || md->is_pure);
                if (md->body) walk_node_for_lambdas(&ctx, md->body);
                ctx.readonly_context = prev_readonly;
                break;
            }
            /* -Wswitch-enum opt-out: capture analysis only runs on function /
             * method bodies. */
            default:
                break;
        }
    }
}

void iron_capture_verbose_report(Iron_Program *program, Iron_Arena *arena) {
    if (!program) return;

    int total_lambdas = 0;
    int capturing_lambdas = 0;

    /* Count pass: walk all top-level declarations */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;
        switch ((int)(decl->kind)) {
            case IRON_NODE_FUNC_DECL: {
                Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
                if (fd->body) verbose_walk(fd->body, arena, 0);
                break;
            }
            case IRON_NODE_METHOD_DECL: {
                Iron_MethodDecl *md = (Iron_MethodDecl *)decl;
                if (md->body) verbose_walk(md->body, arena, 0);
                break;
            }
            /* -Wswitch-enum opt-out: verbose report only visits func / method
             * bodies; every other top-level decl kind is intentionally ignored. */
            default:
                break;
        }
    }
    (void)total_lambdas;
    (void)capturing_lambdas;
}
