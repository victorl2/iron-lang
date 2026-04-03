/* escape.c — Escape analysis pass for Iron.
 *
 * Pass 3 of the semantic pipeline: determines whether heap-allocated values
 * outlive their declaring scope.
 *
 * For each function:
 *   1. Collect heap allocations and their binding names.
 *   2. Collect free/leak targets.
 *   3. Determine escape: return expressions, assignments to outer-scope variables.
 *   4. Mark auto_free on non-escaping heap nodes.
 *   5. Emit E0207 for escaping heaps without free/leak.
 *   6. Validate free/leak operands (E0212, E0213, E0214).
 */

#include "analyzer/escape.h"
#include "vendor/stb_ds.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>

/* ── Context ─────────────────────────────────────────────────────────────── */

/* Associates a bound name with its heap expression node. */
typedef struct {
    const char    *name;        /* variable name that holds the heap value */
    Iron_HeapExpr *heap_node;   /* the IRON_NODE_HEAP expression */
} HeapBinding;

typedef struct {
    Iron_Arena    *arena;
    Iron_DiagList *diags;

    /* Heap bindings in the current function */
    HeapBinding   *heap_bindings;  /* stb_ds dynamic array */

    /* Names that have been freed (via free stmt) */
    const char   **freed_names;    /* stb_ds dynamic array */

    /* Names that have been leaked (via leak stmt) */
    const char   **leaked_names;   /* stb_ds dynamic array */

    /* Names that escape (appear in return or outer assignment) */
    const char   **escaped_names;  /* stb_ds dynamic array */
} EscapeCtx;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static bool name_in_list(const char **list, const char *name) {
    int n = arrlen(list);
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i], name) == 0) return true;
    }
    return false;
}

/* Find the HeapExpr binding for a given name, or NULL. */
static Iron_HeapExpr *find_heap_for_name(EscapeCtx *ctx, const char *name) {
    int n = arrlen(ctx->heap_bindings);
    for (int i = 0; i < n; i++) {
        if (strcmp(ctx->heap_bindings[i].name, name) == 0) {
            return ctx->heap_bindings[i].heap_node;
        }
    }
    return NULL;
}

/* Emit a diagnostic. */
static void emit_err(EscapeCtx *ctx, int code, Iron_Span span, const char *msg) {
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR, code, span,
                   iron_arena_strdup(ctx->arena, msg, strlen(msg)), NULL);
}

/* ── Name extraction from expression nodes ───────────────────────────────── */

/* Get the root identifier name from an expression.  Recurses through
 * field-access and index nodes to find the underlying identifier.
 * Returns NULL if the expression is not rooted in an identifier. */
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

/* ── Collect pass: walk a block and record heap bindings, freed, leaked ───── */

/* Forward-declare the recursive collector. */
static void collect_stmts(EscapeCtx *ctx, Iron_Node **stmts, int count);
static void collect_stmt(EscapeCtx *ctx, Iron_Node *node);

static void collect_stmt(EscapeCtx *ctx, Iron_Node *node) {
    if (!node) return;
    switch (node->kind) {
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            if (vd->init && vd->init->kind == IRON_NODE_HEAP) {
                HeapBinding b;
                b.name      = vd->name;
                b.heap_node = (Iron_HeapExpr *)vd->init;
                arrpush(ctx->heap_bindings, b);
            }
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            if (vd->init && vd->init->kind == IRON_NODE_HEAP) {
                HeapBinding b;
                b.name      = vd->name;
                b.heap_node = (Iron_HeapExpr *)vd->init;
                arrpush(ctx->heap_bindings, b);
            }
            break;
        }
        case IRON_NODE_FREE: {
            Iron_FreeStmt *fs = (Iron_FreeStmt *)node;
            const char *name = expr_ident_name(fs->expr);
            if (name) {
                arrpush(ctx->freed_names, name);
            }
            break;
        }
        case IRON_NODE_LEAK: {
            Iron_LeakStmt *ls = (Iron_LeakStmt *)node;
            const char *name = expr_ident_name(ls->expr);
            if (name) {
                arrpush(ctx->leaked_names, name);
            }
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            const char *name = expr_ident_name(rs->value);
            if (name) {
                arrpush(ctx->escaped_names, name);
            }
            break;
        }
        case IRON_NODE_ASSIGN: {
            /* If we assign a heap value to something in the outer scope,
             * the RHS name escapes. */
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            const char *rhs = expr_ident_name(as->value);
            if (rhs) {
                /* Conservative: if the rhs is a heap-bound name, mark it escaped.
                 * (We can't easily determine if lhs is outer without full scope
                 *  analysis; for correctness in phase 2 we treat any assignment
                 *  of a heap value as a potential escape.) */
                if (find_heap_for_name(ctx, rhs)) {
                    arrpush(ctx->escaped_names, rhs);
                }
            }
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            collect_stmts(ctx, blk->stmts, blk->stmt_count);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            if (is->body) collect_stmt(ctx, is->body);
            for (int i = 0; i < is->elif_count; i++) {
                collect_stmt(ctx, is->elif_bodies[i]);
            }
            if (is->else_body) collect_stmt(ctx, is->else_body);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            if (ws->body) collect_stmt(ctx, ws->body);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            if (fs->body) collect_stmt(ctx, fs->body);
            break;
        }
        default:
            break;
    }
}

static void collect_stmts(EscapeCtx *ctx, Iron_Node **stmts, int count) {
    for (int i = 0; i < count; i++) {
        collect_stmt(ctx, stmts[i]);
    }
}

/* ── Validate pass: free/leak operand checks ──────────────────────────────── */

static void validate_free_leak(EscapeCtx *ctx, Iron_Node **stmts, int count);
static void validate_node(EscapeCtx *ctx, Iron_Node *node);

static void validate_node(EscapeCtx *ctx, Iron_Node *node) {
    if (!node) return;
    switch (node->kind) {
        case IRON_NODE_FREE: {
            Iron_FreeStmt *fs = (Iron_FreeStmt *)node;
            const char *name = expr_ident_name(fs->expr);
            if (name) {
                Iron_HeapExpr *he = find_heap_for_name(ctx, name);
                if (!he) {
                    /* free on non-heap value */
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "'free' on '%s': not a heap-allocated value", name);
                    emit_err(ctx, IRON_ERR_FREE_NON_HEAP, fs->span, msg);
                }
            }
            break;
        }
        case IRON_NODE_LEAK: {
            Iron_LeakStmt *ls = (Iron_LeakStmt *)node;
            const char *name = expr_ident_name(ls->expr);
            if (name) {
                /* Check if this is a heap allocation */
                Iron_HeapExpr *he = find_heap_for_name(ctx, name);
                if (!he) {
                    /* Check if it's an rc value via resolved_sym->type */
                    Iron_Ident *id = (Iron_Ident *)ls->expr;
                    bool is_rc = false;
                    if (id->kind == IRON_NODE_IDENT && id->resolved_sym &&
                        id->resolved_sym->type &&
                        id->resolved_sym->type->kind == IRON_TYPE_RC) {
                        is_rc = true;
                    }
                    /* Also check if the leaked ident itself has RC type */
                    if (!is_rc && id->kind == IRON_NODE_IDENT && id->resolved_type &&
                        id->resolved_type->kind == IRON_TYPE_RC) {
                        is_rc = true;
                    }
                    if (is_rc) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "'leak' on '%s': rc values manage their own lifetime", name);
                        emit_err(ctx, IRON_ERR_LEAK_RC, ls->span, msg);
                    } else {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "'leak' on '%s': not a heap-allocated value", name);
                        emit_err(ctx, IRON_ERR_LEAK_NON_HEAP, ls->span, msg);
                    }
                } else {
                    /* It IS a heap allocation — check if it's also rc-typed */
                    /* (rc heap allocations are exempt from escape, not from E0214) */
                    /* A heap expr wrapping an rc: check inner type */
                    /* This is an unusual combo; skip for now */
                }
            } else {
                /* Leak of a non-identifier expression — check type */
                if (ls->expr) {
                    Iron_Node *e = ls->expr;
                    /* If it's an rc node directly */
                    if (e->kind == IRON_NODE_RC) {
                        emit_err(ctx, IRON_ERR_LEAK_RC, ls->span,
                                 "'leak' on rc value: rc values manage their own lifetime");
                    }
                }
            }
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            validate_free_leak(ctx, blk->stmts, blk->stmt_count);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            if (is->body) validate_node(ctx, is->body);
            for (int i = 0; i < is->elif_count; i++) {
                validate_node(ctx, is->elif_bodies[i]);
            }
            if (is->else_body) validate_node(ctx, is->else_body);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            if (ws->body) validate_node(ctx, ws->body);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            if (fs->body) validate_node(ctx, fs->body);
            break;
        }
        default:
            break;
    }
}

static void validate_free_leak(EscapeCtx *ctx, Iron_Node **stmts, int count) {
    for (int i = 0; i < count; i++) {
        validate_node(ctx, stmts[i]);
    }
}

/* ── Per-function analysis ────────────────────────────────────────────────── */

static void analyze_function_body(EscapeCtx *ctx, Iron_Node *body_node) {
    if (!body_node || body_node->kind != IRON_NODE_BLOCK) return;
    Iron_Block *body = (Iron_Block *)body_node;

    /* Reset per-function state */
    arrsetlen(ctx->heap_bindings, 0);
    arrsetlen(ctx->freed_names,   0);
    arrsetlen(ctx->leaked_names,  0);
    arrsetlen(ctx->escaped_names, 0);

    /* Collect heap bindings, free/leak targets, and escape sources */
    collect_stmts(ctx, body->stmts, body->stmt_count);

    /* Validate free/leak operands */
    validate_free_leak(ctx, body->stmts, body->stmt_count);

    /* Classify each heap binding */
    int n = arrlen(ctx->heap_bindings);
    for (int i = 0; i < n; i++) {
        const char    *name = ctx->heap_bindings[i].name;
        Iron_HeapExpr *he   = ctx->heap_bindings[i].heap_node;

        /* rc values are exempt from escape analysis */
        if (he->resolved_type && he->resolved_type->kind == IRON_TYPE_RC) {
            continue;
        }
        /* Check if the bound variable's type is rc (set by typechecker) */
        /* (The heap node might be wrapped in an rc expression; check inner) */

        bool is_freed  = name_in_list(ctx->freed_names,  name);
        bool is_leaked = name_in_list(ctx->leaked_names, name);
        bool is_escaped = name_in_list(ctx->escaped_names, name);

        if (is_escaped) {
            he->escapes = true;
            he->auto_free = false;
            if (!is_freed && !is_leaked) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "heap value '%s' escapes its declaring scope "
                         "without 'free' or 'leak'; possible memory leak (E0207)",
                         name);
                emit_err(ctx, IRON_ERR_ESCAPE_NO_FREE, he->span, msg);
            }
        } else {
            /* Does not escape — auto-freed at block exit */
            he->auto_free = true;
            he->escapes   = false;
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void iron_escape_analyze(Iron_Program *program, Iron_Scope *global_scope,
                         Iron_Arena *arena, Iron_DiagList *diags) {
    if (!program) return;
    (void)global_scope; /* not needed for intra-procedural analysis */

    EscapeCtx ctx;
    ctx.arena         = arena;
    ctx.diags         = diags;
    ctx.heap_bindings = NULL;
    ctx.freed_names   = NULL;
    ctx.leaked_names  = NULL;
    ctx.escaped_names = NULL;

    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;
        switch (decl->kind) {
            case IRON_NODE_FUNC_DECL: {
                Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
                if (fd->body) analyze_function_body(&ctx, fd->body);
                break;
            }
            case IRON_NODE_METHOD_DECL: {
                Iron_MethodDecl *md = (Iron_MethodDecl *)decl;
                if (md->body) analyze_function_body(&ctx, md->body);
                break;
            }
            default:
                break;
        }
    }

    arrfree(ctx.heap_bindings);
    arrfree(ctx.freed_names);
    arrfree(ctx.leaked_names);
    arrfree(ctx.escaped_names);
}
