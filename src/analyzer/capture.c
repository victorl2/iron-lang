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
#include "lexer/lexer.h"
#include "parser/printer.h"
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
    /* nocopy captures (#512): how the lambda being reached is used, and
     * whether the spawn being reached is surely awaited in its block. */
    int                lambda_use;       /* LambdaUse */
    const char        *lambda_use_name;  /* binding, field, type or callee named in the error */
    bool               spawn_strict_await;
    bool               spawn_is_stmt;    /* a spawn whose handle is discarded */
    const char        *func_name;        /* the function whose body is walked */
    Iron_Program      *program;
    struct ParamEsc   *param_escape;     /* stb_ds map: parameter node -> escapes */
    struct MethodEnt  *methods;          /* stb_ds map: "Type.method" -> decl */
    struct SpanSet    *reported;         /* capture spans already reported */
} CaptureCtx;

/* How a lambda literal is used by the expression around it. */
typedef enum {
    LUSE_OTHER = 0,     /* anything not listed below */
    LUSE_ARG_BORROW,    /* direct argument of a callee that only calls it */
    LUSE_ARG_KEEPS,     /* direct argument of a callee that may keep it */
    LUSE_BOUND,         /* initializer of a val / var */
    LUSE_ASSIGNED,      /* assigned to a binding */
    LUSE_RETURNED,      /* returned */
    LUSE_FIELD,         /* constructor argument (a field of the object) */
    LUSE_FIELD_ASSIGN,  /* assigned to a field */
    LUSE_LIST           /* list element, or handed to push / insert / set */
} LambdaUse;

typedef struct ParamEsc { Iron_Node *key; bool value; } ParamEsc;
typedef struct MethodEnt { char *key; Iron_MethodDecl *value; } MethodEnt;
typedef struct SpanSet { char *key; int value; } SpanSet;

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


static void report_list_capture(CaptureCtx *ctx, Iron_Span span, const char *name,
                                const char *what, const char *help) {
    char msg[320];
    snprintf(msg, sizeof(msg), "%s captures list '%s', which it may only borrow",
             what, name ? name : "?");
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                   IRON_ERR_LIST_IMPLICIT_COPY, span, msg, help);
}

/* ── nocopy captures (#512) ──────────────────────────────────────────────
 *
 * A nocopy value has a single owner, so a closure or a task may only
 * borrow it: a lambda passed directly as an argument to a callee that
 * only calls it during the call, or a spawn awaited in the block that
 * started it (on every path: no return, break or continue between). The
 * env then holds the bytes for reads and never copies or drops them. Any
 * other capture would need a second owner and is E0286; `rc` shares one.
 *
 * Not affected: a captured `var` (it moves into a counted cell shared by
 * every holder), a `heap` binding (the env holds its handle), an `rc` or
 * pointer value, and the runtime's shared handles Mutex, Channel, RWLock
 * and Arena, which a closure or task holds its own reference to. */

/* A captured value the rule governs: a val binding or parameter (not a
 * `var`, which is shared through its cell, nor a `heap` binding) whose type
 * is or holds a nocopy object. Returns that object type. */
static const struct Iron_Type *capture_nocopy_part(const Iron_CaptureEntry *c) {
    if (!c || c->is_mutable) return NULL;
    if (c->sym && c->sym->decl_node && c->sym->decl_node->kind == IRON_NODE_VAL_DECL) {
        Iron_ValDecl *vd = (Iron_ValDecl *)c->sym->decl_node;
        if (vd->init && vd->init->kind == IRON_NODE_HEAP) return NULL;
    }
    return iron_type_nocopy_part(c->type);
}

/* ── Does a callee keep a closure it is given? ───────────────────────────
 *
 * A function-typed parameter escapes when the body uses it other than by
 * calling it, or handing it on as an argument to a parameter that does not
 * escape, outside any closure or spawn that outlives the call. Computed for
 * every function and method with a body as a least fixpoint (mutual
 * recursion starts from "does not escape"). A callee without a body (extern,
 * runtime-backed stdlib) keeps what it is given, except the list methods
 * map, filter, reduce and forEach, which only call it. */

typedef enum { VERDICT_BORROWS, VERDICT_KEEPS, VERDICT_FIELD, VERDICT_LIST } ArgVerdict;

static bool node_is_lambda(const Iron_Node *n) { return n && n->kind == IRON_NODE_LAMBDA; }

static Iron_MethodDecl *find_method(CaptureCtx *ctx, const char *type_name, const char *method) {
    if (!type_name || !method) return NULL;
    char key[512];
    snprintf(key, sizeof(key), "%s.%s", type_name, method);
    ptrdiff_t i = shgeti(ctx->methods, key);
    return i >= 0 ? ctx->methods[i].value : NULL;
}

static bool body_is_empty(Iron_Node *body) {
    return !body || (body->kind == IRON_NODE_BLOCK && ((Iron_Block *)body)->stmt_count == 0);
}

/* The parameter at `idx` escapes (true when unknown). */
static bool param_escapes(CaptureCtx *ctx, Iron_Node **params, int count, int idx) {
    if (idx < 0 || idx >= count || !params[idx]) return true;
    ptrdiff_t i = hmgeti(ctx->param_escape, params[idx]);
    return i < 0 ? true : ctx->param_escape[i].value;
}

static const char *call_callee_name(Iron_CallExpr *ce) {
    if (ce->callee && ce->callee->kind == IRON_NODE_IDENT) return ((Iron_Ident *)ce->callee)->name;
    if (ce->callee && ce->callee->kind == IRON_NODE_FIELD_ACCESS) return ((Iron_FieldAccess *)ce->callee)->field;
    return NULL;
}

/* What the callee of `call` does with the lambda given as argument `argi`. */
static ArgVerdict lambda_arg_verdict(CaptureCtx *ctx, Iron_Node *call, int argi,
                                     const char **name_out) {
    *name_out = NULL;
    if (call->kind == IRON_NODE_CALL) {
        Iron_CallExpr *ce = (Iron_CallExpr *)call;
        *name_out = call_callee_name(ce);
        if (ce->desugared) return VERDICT_KEEPS;
        if (!ce->callee || ce->callee->kind != IRON_NODE_IDENT) return VERDICT_KEEPS;
        Iron_Symbol *sym = ((Iron_Ident *)ce->callee)->resolved_sym;
        if (!sym || !sym->decl_node) return VERDICT_KEEPS;
        if (sym->decl_node->kind == IRON_NODE_OBJECT_DECL) return VERDICT_FIELD;
        if (sym->sym_kind != IRON_SYM_FUNCTION || sym->decl_node->kind != IRON_NODE_FUNC_DECL)
            return VERDICT_KEEPS;
        Iron_FuncDecl *fd = (Iron_FuncDecl *)sym->decl_node;
        if (fd->is_extern || body_is_empty(fd->body)) return VERDICT_KEEPS;
        return param_escapes(ctx, fd->params, fd->param_count, argi) ? VERDICT_KEEPS
                                                                     : VERDICT_BORROWS;
    }
    if (call->kind != IRON_NODE_METHOD_CALL) return VERDICT_KEEPS;
    Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)call;
    *name_out = mc->method;
    const char *m = mc->method ? mc->method : "";
    bool storing = strcmp(m, "push") == 0 || strcmp(m, "insert") == 0 ||
                   strcmp(m, "set") == 0 || strcmp(m, "append") == 0 ||
                   strcmp(m, "put") == 0 || strcmp(m, "add") == 0 ||
                   strcmp(m, "push_front") == 0 || strcmp(m, "prepend") == 0;
    const char *type_name = NULL;
    const struct Iron_Type *rt = NULL;
    if (mc->object && mc->object->kind == IRON_NODE_IDENT) {
        Iron_Symbol *os = ((Iron_Ident *)mc->object)->resolved_sym;
        if (os && os->sym_kind == IRON_SYM_TYPE) type_name = os->name;   /* Type.method(...) */
    }
    if (!type_name && mc->object) {
        rt = ((Iron_Ident *)mc->object)->resolved_type;   /* expression prefix */
        while (rt && (rt->kind == IRON_TYPE_PTR || rt->kind == IRON_TYPE_RC))
            rt = rt->kind == IRON_TYPE_PTR ? rt->ptr.pointee : rt->rc.inner;
        if (rt && rt->kind == IRON_TYPE_ARRAY) {
            Iron_MethodDecl *ext = NULL;
            for (ptrdiff_t k = 0; k < shlen(ctx->methods); k++) {
                Iron_MethodDecl *md = ctx->methods[k].value;
                if (md->is_array_extension && md->method_name && strcmp(md->method_name, m) == 0 &&
                    !body_is_empty(md->body)) { ext = md; break; }
            }
            if (ext)
                return param_escapes(ctx, ext->params, ext->param_count,
                                     ext->is_receiver_form ? argi + 1 : argi)
                           ? VERDICT_KEEPS : VERDICT_BORROWS;
            if (strcmp(m, "map") == 0 || strcmp(m, "filter") == 0 ||
                strcmp(m, "reduce") == 0 || strcmp(m, "forEach") == 0)
                return VERDICT_BORROWS;
            return storing ? VERDICT_LIST : VERDICT_KEEPS;
        }
        if (rt && rt->kind == IRON_TYPE_OBJECT && rt->object.decl) type_name = rt->object.decl->name;
    }
    Iron_MethodDecl *md = find_method(ctx, type_name, m);
    if (!md || body_is_empty(md->body)) return storing ? VERDICT_LIST : VERDICT_KEEPS;
    return param_escapes(ctx, md->params, md->param_count,
                         md->is_receiver_form ? argi + 1 : argi)
               ? VERDICT_KEEPS : VERDICT_BORROWS;
}

/* One pass over a body, marking its tracked parameters that escape. */
typedef struct {
    CaptureCtx *ctx;
    Iron_Node **params;
    int         count;
    int         outliving;   /* > 0 inside a closure or spawn that may outlive the call */
    bool        changed;
} EscWalk;

static void esc_walk(EscWalk *w, Iron_Node *node);

static int tracked_param(EscWalk *w, Iron_Node *n) {
    if (!n || n->kind != IRON_NODE_IDENT) return -1;
    Iron_Symbol *s = ((Iron_Ident *)n)->resolved_sym;
    if (!s || !s->decl_node) return -1;
    for (int i = 0; i < w->count; i++)
        if (w->params[i] == s->decl_node) return i;
    return -1;
}

static void esc_mark(EscWalk *w, int i) {
    ptrdiff_t k = hmgeti(w->ctx->param_escape, w->params[i]);
    if (k >= 0 && !w->ctx->param_escape[k].value) {
        w->ctx->param_escape[k].value = true;
        w->changed = true;
    }
}

static bool esc_visit(Iron_Visitor *v, Iron_Node *node) {
    EscWalk *w = (EscWalk *)v->ctx;
    int pi = tracked_param(w, node);
    if (pi >= 0) { esc_mark(w, pi); return false; }
    switch ((int)node->kind) {
        case IRON_NODE_CALL:
        case IRON_NODE_METHOD_CALL: {
            bool is_call = node->kind == IRON_NODE_CALL;
            Iron_Node **args = is_call ? ((Iron_CallExpr *)node)->args
                                       : ((Iron_MethodCallExpr *)node)->args;
            int argc = is_call ? ((Iron_CallExpr *)node)->arg_count
                               : ((Iron_MethodCallExpr *)node)->arg_count;
            if (is_call) {
                Iron_Node *callee = ((Iron_CallExpr *)node)->callee;
                /* calling the parameter is the use it is for */
                if (!(w->outliving == 0 && tracked_param(w, callee) >= 0)) esc_walk(w, callee);
                if (((Iron_CallExpr *)node)->desugared) esc_walk(w, ((Iron_CallExpr *)node)->desugared);
            } else {
                esc_walk(w, ((Iron_MethodCallExpr *)node)->object);
            }
            for (int i = 0; i < argc; i++) {
                Iron_Node *a = args[i];
                if (!a) continue;
                int ap = tracked_param(w, a);
                bool lam = node_is_lambda(a);
                if (w->outliving == 0 && (ap >= 0 || lam)) {
                    const char *nm;
                    if (lambda_arg_verdict(w->ctx, node, i, &nm) == VERDICT_BORROWS) {
                        if (lam) esc_walk(w, ((Iron_LambdaExpr *)a)->body);
                        continue;   /* handed on to a parameter that only calls it */
                    }
                }
                esc_walk(w, a);
            }
            return false;
        }
        case IRON_NODE_LAMBDA:
        case IRON_NODE_SPAWN:
            /* a closure or task reached here may outlive the call */
            w->outliving++;
            return true;
        default:
            return true;
    }
}

static void esc_post(Iron_Visitor *v, Iron_Node *node) {
    EscWalk *w = (EscWalk *)v->ctx;
    if (node->kind == IRON_NODE_LAMBDA || node->kind == IRON_NODE_SPAWN) w->outliving--;
}

static void esc_walk(EscWalk *w, Iron_Node *node) {
    if (!node) return;
    Iron_Visitor v = { w, esc_visit, esc_post };
    iron_ast_walk(node, &v);
}

static bool param_tracked_type(Iron_Node *p) {
    if (!p || p->kind != IRON_NODE_PARAM) return false;
    const struct Iron_Type *t = ((Iron_Param *)p)->resolved_type;
    return !t || t->kind == IRON_TYPE_FUNC || t->kind == IRON_TYPE_GENERIC_PARAM ||
           (t->kind == IRON_TYPE_NULLABLE && t->nullable.inner &&
            t->nullable.inner->kind == IRON_TYPE_FUNC);
}

static void compute_param_escapes(CaptureCtx *ctx) {
    Iron_Program *prog = ctx->program;
    int total = prog->decl_count + prog->prelude_decl_count;
    for (int i = 0; i < total; i++) {
        Iron_Node *d = prog->decls[i];
        if (!d) continue;
        if (d->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *md = (Iron_MethodDecl *)d;
            const char *tn = md->type_name;
            if (tn && md->method_name) {
                char key[512];
                snprintf(key, sizeof(key), "%s.%s", tn, md->method_name);
                if (shgeti(ctx->methods, key) < 0) shput(ctx->methods, key, md);
                else if (body_is_empty(ctx->methods[shgeti(ctx->methods, key)].value->body))
                    shput(ctx->methods, key, md);
            } else if (md->is_array_extension && md->method_name) {
                char key[512];
                snprintf(key, sizeof(key), "[].%s", md->method_name);
                if (shgeti(ctx->methods, key) < 0) shput(ctx->methods, key, md);
            }
            for (int k = 0; k < md->param_count; k++)
                if (param_tracked_type(md->params[k])) hmput(ctx->param_escape, md->params[k], false);
        } else if (d->kind == IRON_NODE_FUNC_DECL) {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
            for (int k = 0; k < fd->param_count; k++)
                if (param_tracked_type(fd->params[k])) hmput(ctx->param_escape, fd->params[k], false);
        }
    }
    for (int round = 0; round < 64; round++) {
        bool changed = false;
        for (int i = 0; i < total; i++) {
            if (iron_cancel_requested(ctx->cancel_flag)) return;
            Iron_Node *d = prog->decls[i];
            if (!d) continue;
            EscWalk w = { ctx, NULL, 0, 0, false };
            Iron_Node *body = NULL;
            if (d->kind == IRON_NODE_METHOD_DECL) {
                Iron_MethodDecl *md = (Iron_MethodDecl *)d;
                w.params = md->params; w.count = md->param_count; body = md->body;
            } else if (d->kind == IRON_NODE_FUNC_DECL) {
                Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
                w.params = fd->params; w.count = fd->param_count; body = fd->body;
            } else {
                continue;
            }
            bool any = false;
            for (int k = 0; k < w.count; k++)
                if (hmgeti(ctx->param_escape, w.params[k]) >= 0) any = true;
            if (!any || !body) continue;
            esc_walk(&w, body);
            if (w.changed) changed = true;
        }
        if (!changed) break;
    }
}

/* ── Is a spawn surely awaited in its block? ─────────────────────────────── */

/* Does evaluating expression `n` always await `name`? (`a && await t`
 * may skip the await; closures and spawn bodies run elsewhere.) */
static bool expr_surely_awaits(Iron_Node *n, const char *name) {
    if (!n) return false;
    switch ((int)n->kind) {
        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *ae = (Iron_AwaitExpr *)n;
            if (ae->handle && ae->handle->kind == IRON_NODE_IDENT &&
                strcmp(((Iron_Ident *)ae->handle)->name, name) == 0)
                return true;
            return expr_surely_awaits(ae->handle, name);
        }
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *be = (Iron_BinaryExpr *)n;
            if (expr_surely_awaits(be->left, name)) return true;
            if (be->op == IRON_TOK_AND || be->op == IRON_TOK_OR) return false;
            return expr_surely_awaits(be->right, name);
        }
        case IRON_NODE_UNARY: return expr_surely_awaits(((Iron_UnaryExpr *)n)->operand, name);
        case IRON_NODE_FIELD_ACCESS: return expr_surely_awaits(((Iron_FieldAccess *)n)->object, name);
        case IRON_NODE_INDEX:
            return expr_surely_awaits(((Iron_IndexExpr *)n)->object, name) ||
                   expr_surely_awaits(((Iron_IndexExpr *)n)->index, name);
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)n;
            if (expr_surely_awaits(ce->callee, name)) return true;
            for (int i = 0; i < ce->arg_count; i++)
                if (expr_surely_awaits(ce->args[i], name)) return true;
            return false;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)n;
            if (expr_surely_awaits(mc->object, name)) return true;
            for (int i = 0; i < mc->arg_count; i++)
                if (expr_surely_awaits(mc->args[i], name)) return true;
            return false;
        }
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)n;
            for (int i = 0; i < ce->arg_count; i++)
                if (expr_surely_awaits(ce->args[i], name)) return true;
            return false;
        }
        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *is = (Iron_InterpString *)n;
            for (int i = 0; i < is->part_count; i++)
                if (expr_surely_awaits(is->parts[i], name)) return true;
            return false;
        }
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)n;
            for (int i = 0; i < al->element_count; i++)
                if (expr_surely_awaits(al->elements[i], name)) return true;
            return false;
        }
        case IRON_NODE_RC: return expr_surely_awaits(((Iron_RcExpr *)n)->inner, name);
        case IRON_NODE_HEAP: return expr_surely_awaits(((Iron_HeapExpr *)n)->inner, name);
        case IRON_NODE_IS: return expr_surely_awaits(((Iron_IsExpr *)n)->expr, name);
        default: return false;
    }
}

/* A statement of the block that always awaits `name` when it runs. */
static bool stmt_surely_awaits(Iron_Node *st, const char *name) {
    if (!st) return false;
    switch ((int)st->kind) {
        case IRON_NODE_VAL_DECL: return expr_surely_awaits(((Iron_ValDecl *)st)->init, name);
        case IRON_NODE_VAR_DECL: return expr_surely_awaits(((Iron_VarDecl *)st)->init, name);
        case IRON_NODE_ASSIGN:
            return expr_surely_awaits(((Iron_AssignStmt *)st)->value, name) ||
                   expr_surely_awaits(((Iron_AssignStmt *)st)->target, name);
        case IRON_NODE_RETURN: return expr_surely_awaits(((Iron_ReturnStmt *)st)->value, name);
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)st;
            for (int i = 0; i < b->stmt_count; i++)
                if (stmt_surely_awaits(b->stmts[i], name)) return true;
            return false;
        }
        case IRON_NODE_IF: case IRON_NODE_WHILE: case IRON_NODE_FOR: case IRON_NODE_MATCH:
        case IRON_NODE_DEFER: case IRON_NODE_SPAWN: case IRON_NODE_LAMBDA:
            return false;
        default:
            return expr_surely_awaits(st, name);
    }
}

/* Can statement `st` leave the enclosing block (return, or a break /
 * continue not inside a loop of its own)? */
static bool stmt_may_exit(Iron_Node *st, bool in_loop);
typedef struct { bool in_loop; bool found; } ExitWalk;
static bool exit_visit(Iron_Visitor *v, Iron_Node *n) {
    ExitWalk *e = (ExitWalk *)v->ctx;
    if (e->found) return false;
    switch ((int)n->kind) {
        case IRON_NODE_RETURN: e->found = true; return false;
        case IRON_NODE_BREAK: case IRON_NODE_CONTINUE:
            if (!e->in_loop) e->found = true;
            return false;
        case IRON_NODE_LAMBDA: case IRON_NODE_SPAWN: return false;
        case IRON_NODE_WHILE: case IRON_NODE_FOR: {
            Iron_Node *body = n->kind == IRON_NODE_WHILE ? ((Iron_WhileStmt *)n)->body
                                                         : ((Iron_ForStmt *)n)->body;
            if (stmt_may_exit(body, true)) e->found = true;
            return false;
        }
        default: return true;
    }
}
static bool stmt_may_exit(Iron_Node *st, bool in_loop) {
    if (!st) return false;
    ExitWalk e = { in_loop, false };
    Iron_Visitor v = { &e, exit_visit, NULL };
    iron_ast_walk(st, &v);
    return e.found;
}

/* The spawn bound by stmts[i] (`val h = spawn ...`) is awaited by a later
 * statement of the same block that always runs once the spawn has. */
static bool spawn_surely_awaited(Iron_Block *blk, int i, const char *name) {
    for (int j = i + 1; j < blk->stmt_count; j++) {
        if (stmt_surely_awaits(blk->stmts[j], name)) return true;
        if (stmt_may_exit(blk->stmts[j], false)) return false;
    }
    return false;
}

/* ── The E0286 report ─────────────────────────────────────────────────────── */

static const char *nocopy_type_name(const struct Iron_Type *t) {
    return t && t->kind == IRON_TYPE_OBJECT && t->object.decl && t->object.decl->name
        ? t->object.decl->name : "the value";
}

/* `val h = rc <init>` for a val binding whose initializer can be shown. */
static const char *rc_rewrite(CaptureCtx *ctx, const Iron_CaptureEntry *c) {
    if (!c->sym || !c->sym->decl_node || c->sym->decl_node->kind != IRON_NODE_VAL_DECL) return NULL;
    Iron_ValDecl *vd = (Iron_ValDecl *)c->sym->decl_node;
    if (!vd->init || vd->binding_count > 0 || !vd->name) return NULL;
    if (vd->init->kind == IRON_NODE_RC) return NULL;
    char *txt = iron_print_ast(vd->init, NULL, ctx->arena);
    if (!txt || !txt[0]) return NULL;
    size_t n = strlen(txt);
    while (n > 0 && (txt[n - 1] == '\n' || txt[n - 1] == ' ' || txt[n - 1] == ';')) txt[--n] = '\0';
    if (n == 0 || n > 200 || strchr(txt, '\n')) return NULL;
    size_t sz = n + strlen(vd->name) + 32;
    char *out = (char *)iron_arena_alloc(ctx->arena, sz, 1);
    if (!out) return NULL;
    snprintf(out, sz, "val %s = rc %s", vd->name, txt);
    return out;
}

static bool already_reported(CaptureCtx *ctx, Iron_Span sp) {
    char key[96];
    snprintf(key, sizeof(key), "%p:%u:%u", (const void *)sp.filename, sp.line, sp.col);
    if (shgeti(ctx->reported, key) >= 0) return true;
    shput(ctx->reported, key, 1);
    return false;
}

typedef enum {
    NOCOPY_CAPTURE_CLOSURE,
    NOCOPY_CAPTURE_SPAWN,
    NOCOPY_CAPTURE_PARALLEL_FOR
} NocopyCaptureKind;

static void report_nocopy_capture(CaptureCtx *ctx, const Iron_CaptureEntry *c,
                                  const struct Iron_Type *nc, NocopyCaptureKind kind,
                                  Iron_Span fallback) {
    Iron_Span at = c->span.line > 0 ? c->span : fallback;
    if (already_reported(ctx, at)) return;
    const char *name = c->name ? c->name : "?";
    const char *tn = nocopy_type_name(nc);
    const struct Iron_Type *ct = c->type;
    bool direct = ct && ct->kind == IRON_TYPE_OBJECT && ct->object.decl == nc->object.decl;

    char msg[320], label[320], rel[400], help[900];
    if (kind == NOCOPY_CAPTURE_SPAWN)
        snprintf(msg, sizeof(msg), "spawn captures the nocopy value `%s`, but the task can outlive it", name);
    else if (kind == NOCOPY_CAPTURE_PARALLEL_FOR)
        snprintf(msg, sizeof(msg), "parallel for captures the nocopy value `%s`, which cannot be shared with its workers", name);
    else
        snprintf(msg, sizeof(msg), "closure captures the nocopy value `%s`, but the closure can outlive it", name);

    const char *un = ctx->lambda_use_name ? ctx->lambda_use_name : "?";
    if (kind == NOCOPY_CAPTURE_SPAWN) {
        snprintf(label, sizeof(label), ctx->spawn_is_stmt
                 ? "captured here by a spawn whose handle is discarded, so it is never awaited"
                 : "captured here by a spawn that is not surely awaited in the block that starts it");
    } else if (kind == NOCOPY_CAPTURE_PARALLEL_FOR) {
        snprintf(label, sizeof(label),
                 "captured here by a parallel for, which is not a permitted nocopy borrow");
    } else {
        switch (ctx->lambda_use) {
            case LUSE_BOUND: case LUSE_ASSIGNED:
                snprintf(label, sizeof(label), "captured here by a closure that is stored in `%s`", un); break;
            case LUSE_RETURNED:
                snprintf(label, sizeof(label), "captured here by a closure that is returned from `%s`", un); break;
            case LUSE_FIELD:
                snprintf(label, sizeof(label), "captured here by a closure that is stored in a field of `%s`", un); break;
            case LUSE_FIELD_ASSIGN:
                snprintf(label, sizeof(label), "captured here by a closure that is stored in the field `%s`", un); break;
            case LUSE_LIST:
                snprintf(label, sizeof(label), "captured here by a closure that is stored in a list or map"); break;
            case LUSE_ARG_KEEPS:
                snprintf(label, sizeof(label), "captured here by a closure passed to `%s`, which may keep it after the call returns", un); break;
            default:
                snprintf(label, sizeof(label), "captured here by a closure that is not passed directly as a call argument"); break;
        }
    }

    bool is_param = c->sym && c->sym->decl_node && c->sym->decl_node->kind == IRON_NODE_PARAM;
    if (direct)
        snprintf(rel, sizeof(rel), "`%s` is %s here; %s is nocopy, so it has a single owner",
                 name, is_param ? "a parameter" : "declared", tn);
    else
        snprintf(rel, sizeof(rel), "`%s` is %s here; it holds a %s, which is nocopy, so it has a single owner",
                 name, is_param ? "a parameter" : "declared", tn);

    const char *note = "a nocopy value can only be borrowed by a lambda passed directly as a call "
                       "argument, or by a spawn awaited in the same block";
    const char *rw = rc_rewrite(ctx, c);
    const char *ctn = direct ? tn : nocopy_type_name(ct);
    char share[300], guarded[300];
    if (rw) {
        snprintf(share, sizeof(share), "    %s", rw);
        snprintf(guarded, sizeof(guarded), "    val %s = Mutex(%s)", name, rw + strlen(name) + 7);
    } else {
        snprintf(share, sizeof(share), "    create it as `rc %s(...)` and take `rc %s` here instead of `%s`",
                 ctn, ctn, ctn);
        snprintf(guarded, sizeof(guarded), "    val m = Mutex(rc %s(...))", ctn);
    }
    if (kind == NOCOPY_CAPTURE_SPAWN) {
        snprintf(help, sizeof(help),
                 "await the task in this block (`val t = spawn(\"t\") { ... }`, then `await t`) to lend it to the task;\n"
                 "to share it with a task that outlives this block, make it reference counted:\n"
                 "%s\n"
                 "the task then holds its own reference, and the value is dropped once, when the last holder is gone;\n"
                 "if both threads use it, also guard it with a Mutex, since two threads must not use one %s at the same time:\n"
                 "%s\n"
                 "and lock it around each use (`val g = %s.lock()`, then `g.get()`)",
                 share, tn, guarded, rw ? name : "m");
    } else if (kind == NOCOPY_CAPTURE_PARALLEL_FOR) {
        snprintf(help, sizeof(help),
                 "make the value reference counted and guard it with a Mutex, since several worker threads must not use one %s at the same time:\n"
                 "%s\n"
                 "lock it in the parallel body (`val g = %s.lock()`, then use `g.get()`); "
                 "each worker then holds a reference, and the value is dropped once, when the last holder is gone",
                 tn, guarded, rw ? name : "m");
    } else {
        snprintf(help, sizeof(help),
                 "to share it with a closure that is stored, returned or outlives this block, make it reference counted:\n"
                 "%s\n"
                 "the closure then holds its own reference, and the value is dropped once, when the last holder is gone",
                 share);
    }
    Iron_Span decl = c->sym ? c->sym->span : (Iron_Span){0};
    /* A val / var symbol sits at its keyword: point at the name. */
    if (c->sym && c->sym->decl_node &&
        (c->sym->decl_node->kind == IRON_NODE_VAL_DECL || c->sym->decl_node->kind == IRON_NODE_VAR_DECL) &&
        decl.line == c->sym->decl_node->span.line && decl.col == c->sym->decl_node->span.col) {
        decl.col += 4;
        decl.end_line = decl.line;
        decl.end_col = decl.col + (uint32_t)strlen(name);
    }
    iron_diag_emit_labeled(ctx->diags, ctx->arena, IRON_DIAG_ERROR, IRON_ERR_COPY_OF_NOCOPY_TYPE,
                           at, msg, label, decl, rel, note, help);
}

/* stb_ds string hashmap entry (int value — we just use it as a set) */
typedef struct { char *key; int value; } StrSet;

/* Temporary capture accumulator entry (uses stb_ds dynamic array) */
typedef struct {
    const char      *name;
    struct Iron_Type *type;
    bool             is_mutable;
    bool             is_boxed;
    Iron_Span        span;
    struct Iron_Symbol *sym;
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

/* ── Capture collection ───────────────────────────────────────────────────── */

/* Forward declaration for mutual recursion. */
static void walk_node_for_lambdas(CaptureCtx *ctx, Iron_Node *node);

static StrSet *strset_copy(StrSet *s) {
    StrSet *out = NULL;
    for (ptrdiff_t i = 0; i < shlen(s); i++) shput(out, s[i].key, 1);
    return out;
}

/* The names a match pattern binds, nested patterns included. */
static void pattern_locals(Iron_Node *pat, StrSet **locals) {
    if (!pat || pat->kind != IRON_NODE_PATTERN) return;
    Iron_Pattern *p = (Iron_Pattern *)pat;
    for (int i = 0; i < p->binding_count; i++) {
        if (p->binding_names && p->binding_names[i])
            shput(*locals, (char *)p->binding_names[i], 1);
        if (p->nested_patterns && p->nested_patterns[i])
            pattern_locals(p->nested_patterns[i], locals);
    }
}

/* Walk `node` collecting all IRON_NODE_IDENT references that are outer-scope
 * captures into `captures`. `locals` is the set of names defined inside this
 * lambda (params + local decls). */
/* `free x` / `leak x` of a binding the body captures, seen by the last
 * collect_idents walk (#545): a lambda or spawn body works on its own copy
 * of a captured heap binding, so it cannot free or leak the owner's. */
static Iron_Node **s_captured_release;

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
            cap.span       = id->span;
            cap.sym        = id->resolved_sym;
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
                cap.span       = ic->span;
                cap.sym        = ic->sym;
                arrput(*captures, cap);
            }
            break;
        }

        /* Recurse into all other node types that can contain expressions.
         * A name declared in a block is local from its declaration to the
         * end of that block (manual 4.1): before it, and outside the block,
         * the same name still reaches the binding of an enclosing scope,
         * which is then a capture (`val v = v + 1` reads the outer v). */
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            StrSet *saved = strset_copy(*locals);
            for (int i = 0; i < blk->stmt_count; i++) {
                collect_idents(blk->stmts[i], locals, captures, seen);
            }
            shfree(*locals);
            *locals = saved;
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            collect_idents(vd->init, locals, captures, seen);
            if (vd->name) shput(*locals, (char *)vd->name, 1);
            for (int i = 0; i < vd->binding_count; i++) {
                if (vd->binding_names && vd->binding_names[i])
                    shput(*locals, (char *)vd->binding_names[i], 1);
            }
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            collect_idents(vd->init, locals, captures, seen);
            if (vd->name) shput(*locals, (char *)vd->name, 1);
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
            StrSet *saved = strset_copy(*locals);
            if (fs->var_name) shput(*locals, (char *)fs->var_name, 1);
            if (fs->var_name2) shput(*locals, (char *)fs->var_name2, 1);
            collect_idents(fs->body,     locals, captures, seen);
            shfree(*locals);
            *locals = saved;
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
            StrSet *saved = strset_copy(*locals);
            pattern_locals(mc->pattern, locals);
            collect_idents(mc->body,    locals, captures, seen);
            shfree(*locals);
            *locals = saved;
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
            /* `heap(in: a) T()` reads the arena binding too (inside a spawn
             * or parallel for body it was not captured: C compile error). */
            collect_idents(he->arena_expr, locals, captures, seen);
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
        case IRON_NODE_ENUM_CONSTRUCT: {
            /* `Tok.Pair(a, b)`: the payload arguments may be captures. */
            Iron_EnumConstruct *ec = (Iron_EnumConstruct *)node;
            for (int i = 0; i < ec->arg_count; i++) {
                collect_idents(ec->args[i], locals, captures, seen);
            }
            break;
        }
        case IRON_NODE_IN_ARENA: {
            Iron_InArenaBlock *ia = (Iron_InArenaBlock *)node;
            collect_idents(ia->arena_expr, locals, captures, seen);
            collect_idents(ia->body,       locals, captures, seen);
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
            if (fs->expr && fs->expr->kind == IRON_NODE_IDENT &&
                shgeti(*locals, ((Iron_Ident *)fs->expr)->name) < 0)
                arrput(s_captured_release, node);
            collect_idents(fs->expr, locals, captures, seen);
            break;
        }
        case IRON_NODE_LEAK: {
            Iron_LeakStmt *ls = (Iron_LeakStmt *)node;
            if (ls->expr && ls->expr->kind == IRON_NODE_IDENT &&
                shgeti(*locals, ((Iron_Ident *)ls->expr)->name) < 0)
                arrput(s_captured_release, node);
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

/* Report every `free` / `leak` of a captured binding the walk saw. */
static void report_captured_release(CaptureCtx *ctx, const char *what) {
    for (int i = 0; i < (int)arrlen(s_captured_release); i++) {
        Iron_Node *n = s_captured_release[i];
        bool is_free = n->kind == IRON_NODE_FREE;
        Iron_Node *e = is_free ? ((Iron_FreeStmt *)n)->expr : ((Iron_LeakStmt *)n)->expr;
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot %s '%s' inside a %s that captured it",
                 is_free ? "free" : "leak", ((Iron_Ident *)e)->name, what);
        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR, IRON_ERR_FREE_NON_HEAP,
                       n->span, msg,
                       "the body has its own copy of the binding; free or leak the heap"
                       " value in the function that allocated it");
    }
    arrfree(s_captured_release);
    s_captured_release = NULL;
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
    /* The body's own declarations become local as the walk reaches them
     * (collect_idents), so a name read before a shadowing declaration is
     * still captured from the enclosing scope. */

    /* Collect captures via ident walk */
    TmpCapture *captures = NULL;
    StrSet     *seen     = NULL;
    arrfree(s_captured_release);
    s_captured_release = NULL;
    collect_idents(le->body, &locals, &captures, &seen);
    report_captured_release(ctx, "lambda");

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
            arr[i].span       = captures[i].span;
            arr[i].sym        = captures[i].sym;
        }
        le->captures      = arr;
        le->capture_count = count;

        /* A callee that keeps the closure (stores it, a constructor, push)
         * would keep the borrow past the call too. */
        if (ctx->lambda_use != LUSE_ARG_BORROW) {
            /* A view borrows its source for the block it lives in (manual
             * 2.15): the closure may use it only during the call. */
            for (int ci = 0; ci < count; ci++) {
                if (!arr[ci].type || arr[ci].type->kind != IRON_TYPE_VIEW) continue;
                char msg[320];
                snprintf(msg, sizeof(msg),
                         "closure captures view '%s', which must not outlive its block",
                         arr[ci].name ? arr[ci].name : "?");
                iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR, IRON_ERR_VIEW_ESCAPE,
                               le->span, msg,
                               "pass the closure directly as a call argument, or capture the "
                               "source (a list or String) instead of the view");
                break;
            }
            for (int ci = 0; ci < count; ci++) {
                if (!type_holds_owned_list(arr[ci].type, 0)) continue;
                report_list_capture(ctx, le->span, arr[ci].name, "closure",
                    ctx->lambda_use == LUSE_ARG_KEEPS || ctx->lambda_use == LUSE_FIELD ||
                    ctx->lambda_use == LUSE_LIST
                    ? "the callee keeps the closure (it stores it), so the closure outlives the borrow: "
                      "share the list as `rc [T]`"
                    : "a closure bound, returned or stored outlives the borrow: "
                      "share the list as `rc [T]`, or pass the closure directly as an argument");
                break;
            }
        }

        /* nocopy values (#512): borrowed by a closure the callee only
         * calls during the call, an error anywhere else. */
        for (int ci = 0; ci < count; ci++) {
            const struct Iron_Type *nc = capture_nocopy_part(&arr[ci]);
            if (!nc) continue;
            if (ctx->lambda_use == LUSE_ARG_BORROW) arr[ci].is_borrowed = true;
            else report_nocopy_capture(ctx, &arr[ci], nc, NOCOPY_CAPTURE_CLOSURE, le->span);
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
    /* The body's own declarations become local as the walk reaches them
     * (collect_idents), as for lambdas: a name read before a shadowing
     * declaration in the body is still captured from the enclosing scope
     * (collected up front, it was taken for the local and the capture was
     * missed: C compile error). */
    StrSet *locals = NULL;

    /* Collect captures via ident walk */
    TmpCapture *captures = NULL;
    StrSet     *seen     = NULL;
    arrfree(s_captured_release);
    s_captured_release = NULL;
    collect_idents(ss->body, &locals, &captures, &seen);
    report_captured_release(ctx, "spawn body");

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
            arr[i].span       = captures[i].span;
            arr[i].sym        = captures[i].sym;
        }
        ss->captures      = arr;
        ss->capture_count = count;

        /* An await that a return, break or continue may skip, or that only
         * a branch runs, leaves the task running past the list. */
        if (!ctx->spawn_strict_await) {
            for (int ci = 0; ci < count; ci++) {
                if (!arr[ci].type || arr[ci].type->kind != IRON_TYPE_VIEW) continue;
                char msg[320];
                snprintf(msg, sizeof(msg),
                         "spawn captures view '%s', which must not outlive its block",
                         arr[ci].name ? arr[ci].name : "?");
                iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR, IRON_ERR_VIEW_ESCAPE,
                               ss->span, msg,
                               "await the handle in the same block (`val h = spawn(...) {...}` "
                               "then `await h`)");
                break;
            }
            for (int ci = 0; ci < count; ci++) {
                if (!type_holds_owned_list(arr[ci].type, 0)) continue;
                report_list_capture(ctx, ss->span, arr[ci].name, "spawn",
                    "await the handle in the same block (`val h = spawn(...) {...}` "
                    "then `await h`, with no return, break or continue before it), "
                    "or share the list as `rc [T]`");
                break;
            }
        }

        /* nocopy values (#512): borrowed by a task surely awaited in the
         * block that starts it, an error otherwise. */
        for (int ci = 0; ci < count; ci++) {
            const struct Iron_Type *nc = capture_nocopy_part(&arr[ci]);
            if (!nc) continue;
            if (ctx->spawn_strict_await) arr[ci].is_borrowed = true;
            else report_nocopy_capture(ctx, &arr[ci], nc, NOCOPY_CAPTURE_SPAWN, ss->span);
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
    /* Locals: the loop variable; the body's own declarations become local
     * as collect_idents reaches them (see find_spawn_captures). */
    StrSet *locals = NULL;
    shput(locals, fs->var_name, 1);
    if (fs->var_name2) shput(locals, fs->var_name2, 1);

    /* Collect captures via ident walk (also walk iterable for range expr) */
    TmpCapture *captures = NULL;
    StrSet     *seen     = NULL;
    arrfree(s_captured_release);
    s_captured_release = NULL;
    collect_idents(fs->body, &locals, &captures, &seen);
    report_captured_release(ctx, "parallel for body");

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
            arr[i].span       = captures[i].span;
            arr[i].sym        = captures[i].sym;
        }
        fs->pfor_captures      = arr;
        fs->pfor_capture_count = count;

        /* The nocopy borrow rule deliberately has only two forms: a lambda
         * argument whose callee only calls it, and a surely awaited spawn.
         * A parallel for has several workers, so sharing requires rc plus a
         * lock rather than copying the single-owner value (#703). */
        for (int ci = 0; ci < count; ci++) {
            const struct Iron_Type *nc = capture_nocopy_part(&arr[ci]);
            if (nc)
                report_nocopy_capture(ctx, &arr[ci], nc,
                                      NOCOPY_CAPTURE_PARALLEL_FOR, fs->span);
        }
    } else {
        fs->pfor_captures      = NULL;
        fs->pfor_capture_count = 0;
    }

    shfree(locals);
    shfree(seen);
    arrfree(captures);
}

/* ── Lambda walker ────────────────────────────────────────────────────────── */

/* Walk `child` with the lambda use `use` (it applies only if `child` is
 * itself a lambda literal: walk_node_for_lambdas clears it otherwise). */
static void walk_with_use(CaptureCtx *ctx, Iron_Node *child, int use, const char *name) {
    int su = ctx->lambda_use;
    const char *sn = ctx->lambda_use_name;
    ctx->lambda_use = use;
    ctx->lambda_use_name = name;
    walk_node_for_lambdas(ctx, child);
    ctx->lambda_use = su;
    ctx->lambda_use_name = sn;
}

/* Walk the arguments of a call or method call: a lambda literal argument
 * borrows only when the callee does not keep it. */
static void walk_call_args(CaptureCtx *ctx, Iron_Node *call, Iron_Node **args, int argc) {
    for (int i = 0; i < argc; i++) {
        bool prev = ctx->lambda_is_call_arg;
        ctx->lambda_is_call_arg = args[i] && args[i]->kind == IRON_NODE_LAMBDA;
        int use = LUSE_OTHER;
        const char *nm = NULL;
        if (ctx->lambda_is_call_arg) {
            switch (lambda_arg_verdict(ctx, call, i, &nm)) {
                case VERDICT_BORROWS: use = LUSE_ARG_BORROW; break;
                case VERDICT_FIELD:   use = LUSE_FIELD; break;
                case VERDICT_LIST:    use = LUSE_LIST; break;
                case VERDICT_KEEPS:   use = LUSE_ARG_KEEPS; break;
            }
        }
        walk_with_use(ctx, args[i], use, nm);
        ctx->lambda_is_call_arg = prev;
    }
}

static void walk_node_for_lambdas(CaptureCtx *ctx, Iron_Node *node) {
    if (!node) return;
    /* HARD-05: cancel poll at recursive walker entry. */
    if (iron_cancel_requested(ctx->cancel_flag)) return;
    /* A lambda use describes the node it was set for, never a node below. */
    if (node->kind != IRON_NODE_LAMBDA && ctx->lambda_use != LUSE_OTHER) {
        walk_with_use(ctx, node, LUSE_OTHER, NULL);
        return;
    }
    switch ((int)(node->kind)) {
        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *le = (Iron_LambdaExpr *)node;
            bool is_arg = ctx->lambda_is_call_arg;
            int use = ctx->lambda_use;
            const char *use_name = ctx->lambda_use_name;
            /* Process nested lambdas first */
            ctx->lambda_is_call_arg = false;
            ctx->lambda_use = LUSE_OTHER;
            ctx->lambda_use_name = NULL;
            walk_node_for_lambdas(ctx, le->body);
            ctx->lambda_is_call_arg = is_arg;
            ctx->lambda_use = use;
            ctx->lambda_use_name = use_name;
            /* Now analyze this lambda */
            find_captures(ctx, le);
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            for (int i = 0; i < blk->stmt_count; i++) {
                Iron_Node *st = blk->stmts[i];
                /* `val h = spawn(...) {...}` awaited later in this block. */
                bool prev_strict = ctx->spawn_strict_await;
                bool prev_stmt = ctx->spawn_is_stmt;
                ctx->spawn_strict_await = false;
                ctx->spawn_is_stmt = st && st->kind == IRON_NODE_SPAWN;
                if (st && st->kind == IRON_NODE_VAL_DECL) {
                    Iron_ValDecl *vd = (Iron_ValDecl *)st;
                    if (vd->init && vd->init->kind == IRON_NODE_SPAWN && vd->name)
                        ctx->spawn_strict_await = spawn_surely_awaited(blk, i, vd->name);
                } else if (st && st->kind == IRON_NODE_VAR_DECL) {
                    Iron_VarDecl *vd = (Iron_VarDecl *)st;
                    if (vd->init && vd->init->kind == IRON_NODE_SPAWN && vd->name)
                        ctx->spawn_strict_await = spawn_surely_awaited(blk, i, vd->name);
                }
                walk_node_for_lambdas(ctx, st);
                ctx->spawn_strict_await = prev_strict;
                ctx->spawn_is_stmt = prev_stmt;
            }
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            walk_with_use(ctx, vd->init, LUSE_BOUND, vd->name ? vd->name : "the tuple");
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            walk_with_use(ctx, vd->init, LUSE_BOUND, vd->name);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            Iron_Node *t = as->target;
            if (t && t->kind == IRON_NODE_IDENT)
                walk_with_use(ctx, as->value, LUSE_ASSIGNED, ((Iron_Ident *)t)->name);
            else if (t && t->kind == IRON_NODE_FIELD_ACCESS)
                walk_with_use(ctx, as->value, LUSE_FIELD_ASSIGN, ((Iron_FieldAccess *)t)->field);
            else if (t && t->kind == IRON_NODE_INDEX)
                walk_with_use(ctx, as->value, LUSE_LIST, NULL);
            else
                walk_node_for_lambdas(ctx, as->value);
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            walk_with_use(ctx, rs->value, LUSE_RETURNED, ctx->func_name);
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
            walk_call_args(ctx, node, ce->args, ce->arg_count);
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            walk_node_for_lambdas(ctx, mc->object);
            walk_call_args(ctx, node, mc->args, mc->arg_count);
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
            bool prev_strict = ctx->spawn_strict_await;
            bool prev_stmt = ctx->spawn_is_stmt;
            if (ae->handle && ae->handle->kind == IRON_NODE_SPAWN) {
                ctx->spawn_strict_await = true;
                ctx->spawn_is_stmt = false;
            }
            walk_node_for_lambdas(ctx, ae->handle);
            ctx->spawn_strict_await = prev_strict;
            ctx->spawn_is_stmt = prev_stmt;
            break;
        }
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;
            for (int i = 0; i < ce->arg_count; i++) {
                walk_with_use(ctx, ce->args[i], LUSE_FIELD, ce->type_name);
            }
            break;
        }
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)node;
            walk_node_for_lambdas(ctx, al->size);
            for (int i = 0; i < al->element_count; i++) {
                walk_with_use(ctx, al->elements[i], LUSE_LIST, NULL);
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
            /* Whether this spawn is surely awaited describes it alone. */
            bool strict = ctx->spawn_strict_await, is_stmt = ctx->spawn_is_stmt;
            ctx->spawn_strict_await = false;
            ctx->spawn_is_stmt = false;
            walk_node_for_lambdas(ctx, ss->pool_expr);
            walk_node_for_lambdas(ctx, ss->body);
            ctx->spawn_strict_await = strict;
            ctx->spawn_is_stmt = is_stmt;
            /* Perform capture analysis for spawn block body */
            find_spawn_captures(ctx, ss);
            break;
        }
        /* An `in a { }` body: a lambda, spawn or parallel for in it had no
         * capture analysis (its captures were undeclared in the C). */
        case IRON_NODE_IN_ARENA: {
            Iron_InArenaBlock *ia = (Iron_InArenaBlock *)node;
            walk_node_for_lambdas(ctx, ia->arena_expr);
            walk_node_for_lambdas(ctx, ia->body);
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
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena            = arena;
    ctx.diags            = diags;
    ctx.lambda_is_call_arg = false;
    ctx.cancel_flag      = cancel_flag;
    ctx.readonly_context = false;
    ctx.program          = program;
    sh_new_strdup(ctx.methods);
    sh_new_strdup(ctx.reported);
    compute_param_escapes(&ctx);

    for (int i = 0; i < program->decl_count; i++) {
        /* HARD-05: cancel poll inside top-level decl loop. */
        if (iron_cancel_requested(cancel_flag)) break;
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;
        switch ((int)(decl->kind)) {
            case IRON_NODE_FUNC_DECL: {
                Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
                /* Phase 22 OQ-04: set readonly_context from AST is_readonly flag
                 * per RESEARCH Pitfall 4 (TypeCtx not available post-typecheck). */
                bool prev_readonly = ctx.readonly_context;
                ctx.readonly_context = fd->is_readonly;
                ctx.func_name = fd->name;
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
                ctx.func_name = md->method_name;
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
    hmfree(ctx.param_escape);
    shfree(ctx.methods);
    shfree(ctx.reported);
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
