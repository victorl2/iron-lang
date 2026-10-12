/* view_check.c: the borrow rules of View[T], ViewMut[T] and Bytes (manual
 * 2.15). See view_check.h.
 *
 * E0369, a view escapes:
 *   - a view type inside another type (a field, a global, a list or Map
 *     element, a nullable, a tuple, an rc or pointer target), wherever it
 *     is written: declarations, parameters, results, literals;
 *   - a function returning a view without exactly one view, String or list
 *     parameter, or returning a view not derived from that parameter.
 *   (Closures and spawns capturing a view are reported by capture.c.)
 *
 * E0370, the source of a live view:
 *   - is not borrowable: a temporary (a call result, a list literal) or a
 *     list or String reached through an rc, heap or pointer handle;
 *   - is resized (push, pop, insert, remove, clear, take, spare), reassigned,
 *     passed to a `var` parameter or changed by a mutating method, while a
 *     view of it is live. A view is live from its creation to its last use
 *     in the function; a use inside a loop that does not contain the
 *     view's creation keeps it live to the end of that loop. A `for` loop
 *     over a view keeps it live for the whole loop. */

#include "analyzer/view_check.h"
#include "analyzer/intrinsics.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    Iron_Program  *program;
    Iron_Arena    *arena;
    Iron_DiagList *diags;
} ViewCtx;

static uint64_t pos_of(Iron_Span s) { return ((uint64_t)s.line << 32) | s.col; }
static uint64_t end_of(Iron_Span s) {
    uint32_t l = s.end_line ? s.end_line : s.line;
    uint32_t c = s.end_line ? s.end_col : s.col;
    return ((uint64_t)l << 32) | c;
}

static void report(ViewCtx *ctx, int code, Iron_Span span, const char *msg, const char *help) {
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR, code, span,
                   iron_arena_strdup(ctx->arena, msg, strlen(msg)), help);
}

static Iron_Type *expr_type(Iron_Node *n) {
    if (!n) return NULL;
    switch ((int)n->kind) {
        case IRON_NODE_PARAM: return ((Iron_Param *)n)->resolved_type;
        case IRON_NODE_FIELD: return ((Iron_Field *)n)->resolved_type;
        case IRON_NODE_VAL_DECL: return ((Iron_ValDecl *)n)->declared_type;
        case IRON_NODE_VAR_DECL: return ((Iron_VarDecl *)n)->declared_type;
        case IRON_NODE_INT_LIT: case IRON_NODE_FLOAT_LIT: case IRON_NODE_STRING_LIT:
        case IRON_NODE_INTERP_STRING: case IRON_NODE_BOOL_LIT: case IRON_NODE_NULL_LIT:
        case IRON_NODE_IDENT: case IRON_NODE_BINARY: case IRON_NODE_UNARY:
        case IRON_NODE_CALL: case IRON_NODE_METHOD_CALL: case IRON_NODE_FIELD_ACCESS:
        case IRON_NODE_INDEX: case IRON_NODE_SLICE: case IRON_NODE_LAMBDA:
        case IRON_NODE_HEAP: case IRON_NODE_RC: case IRON_NODE_COMPTIME:
        case IRON_NODE_IS: case IRON_NODE_CONSTRUCT: case IRON_NODE_ARRAY_LIT:
        case IRON_NODE_AWAIT:
            return ((Iron_ExprNode *)n)->resolved_type;
        default:
            return NULL;
    }
}

static bool is_view(const Iron_Type *t) { return t && t->kind == IRON_TYPE_VIEW; }

/* A view anywhere inside t, below the top level (t itself may be a view). */
static bool nested_view(const Iron_Type *t, int depth) {
    if (!t || depth > 24) return false;
    switch ((int)t->kind) {
        case IRON_TYPE_VIEW:     return depth > 0 || nested_view(t->view.elem, depth + 1);
        case IRON_TYPE_NULLABLE: return nested_view(t->nullable.inner, depth + 1);
        case IRON_TYPE_ARRAY:    return nested_view(t->array.elem, depth + 1);
        case IRON_TYPE_RC:       return nested_view(t->rc.inner, depth + 1);
        case IRON_TYPE_WEAK_RC:  return nested_view(t->weak_rc.inner, depth + 1);
        case IRON_TYPE_PTR:      return nested_view(t->ptr.pointee, depth + 1);
        case IRON_TYPE_TUPLE:
            for (int i = 0; i < t->tuple.elem_count; i++)
                if (nested_view(t->tuple.elem_types[i], depth + 1)) return true;
            return false;
        case IRON_TYPE_OBJECT:
            return nested_view(t->object.elem, depth + 1) || nested_view(t->object.elem2, depth + 1);
        case IRON_TYPE_ENUM:
            for (int i = 0; i < t->enu.type_arg_count; i++)
                if (nested_view(t->enu.type_args[i], depth + 1)) return true;
            return false;
        default:
            return false;
    }
}

static void check_type_pos(ViewCtx *ctx, const Iron_Type *t, Iron_Span span, const char *where) {
    if (!nested_view(t, 0)) return;
    char msg[320];
    snprintf(msg, sizeof(msg), "a view cannot be stored in %s ('%s')", where,
             iron_type_to_string(t, ctx->arena));
    report(ctx, IRON_ERR_VIEW_ESCAPE, span, msg,
           "a view only borrows its source for the block it lives in: keep the source "
           "(a list or String) and the start and end positions instead");
}

/* ── Roots ─────────────────────────────────────────────────────────────── */

typedef enum { ROOT_NONE, ROOT_SYM, ROOT_STATIC, ROOT_TEMP, ROOT_HANDLE } RootKind;
typedef struct { RootKind kind; Iron_Symbol *sym; } Root;

typedef struct { Iron_Symbol *key; Root value; } ViewRootEntry;

typedef struct {
    ViewCtx       *ctx;
    ViewRootEntry *roots;      /* view binding -> its source root */
    struct { Iron_Symbol *key; uint64_t value; } *last_use;
    struct Borrow { Iron_Symbol *root; uint64_t start, end; Iron_Span span; } *borrows;
    Iron_Symbol  **loop_decls; /* scratch */
} FnCtx;

static Root root_of_view(FnCtx *f, Iron_Node *e);

static bool is_view_producer(int id) {
    switch (id) {
        case IRON_INTR_STRING_BYTES: case IRON_INTR_LIST_VIEW: case IRON_INTR_LIST_VIEW_MUT:
        case IRON_INTR_LIST_SPARE: case IRON_INTR_VIEW_SLICE: case IRON_INTR_VIEW_AS_READ:
        case IRON_INTR_SB_SPARE: case IRON_INTR_SB_BYTES:
            return true;
        default:
            return false;
    }
}

/* The binding a place (a variable, a field or element path) lives in. */
static Root place_root(FnCtx *f, Iron_Node *e) {
    Root r = { ROOT_NONE, NULL };
    if (!e) return r;
    switch ((int)e->kind) {
        case IRON_NODE_IDENT: {
            Iron_Symbol *s = ((Iron_Ident *)e)->resolved_sym;
            if (!s) return r;
            if (is_view(s->type) || is_view(((Iron_Ident *)e)->resolved_type)) {
                ptrdiff_t i = hmgeti(f->roots, s);
                if (i >= 0) return f->roots[i].value;
            }
            r.kind = ROOT_SYM; r.sym = s;
            return r;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)e;
            Iron_Type *ot = expr_type(fa->object);
            if (ot && (ot->kind == IRON_TYPE_RC || ot->kind == IRON_TYPE_PTR ||
                       ot->kind == IRON_TYPE_WEAK_RC)) {
                r.kind = ROOT_HANDLE;
                return r;
            }
            return place_root(f, fa->object);
        }
        case IRON_NODE_INDEX:
            return place_root(f, ((Iron_IndexExpr *)e)->object);
        case IRON_NODE_STRING_LIT:
            r.kind = ROOT_STATIC;
            return r;
        default:
            r.kind = ROOT_TEMP;
            return r;
    }
}

/* The source a view-typed expression borrows. */
static Root root_of_view(FnCtx *f, Iron_Node *e) {
    Root r = { ROOT_NONE, NULL };
    if (!e) return r;
    if (e->kind == IRON_NODE_IDENT) return place_root(f, e);
    if (e->kind == IRON_NODE_METHOD_CALL) {
        Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)e;
        if (is_view_producer(mc->intrinsic)) {
            if (mc->intrinsic == IRON_INTR_VIEW_SLICE || mc->intrinsic == IRON_INTR_VIEW_AS_READ)
                return root_of_view(f, mc->object);
            return place_root(f, mc->object);
        }
        return r;
    }
    if (e->kind == IRON_NODE_CALL) {
        /* A function returning a view returns a view of its single view,
         * String or list argument (the rule its declaration is held to). */
        Iron_CallExpr *ce = (Iron_CallExpr *)e;
        int n = 0; Iron_Node *src = NULL;
        for (int i = 0; i < ce->arg_count; i++) {
            Iron_Type *at = expr_type(ce->args[i]);
            if (at && (at->kind == IRON_TYPE_VIEW || at->kind == IRON_TYPE_STRING ||
                       at->kind == IRON_TYPE_ARRAY)) { n++; src = ce->args[i]; }
        }
        if (n == 1) {
            Iron_Type *st = expr_type(src);
            return is_view(st) ? root_of_view(f, src) : place_root(f, src);
        }
        return r;
    }
    return r;
}

/* ── Last uses ─────────────────────────────────────────────────────────── */

static uint64_t subtree_end(Iron_Node *n);

typedef struct { FnCtx *f; uint64_t loop_start, loop_end; int in_loop; } UseWalk;

static void note_use(FnCtx *f, Iron_Symbol *s, uint64_t at) {
    ptrdiff_t i = hmgeti(f->last_use, s);
    if (i < 0) hmput(f->last_use, s, at);
    else if (f->last_use[i].value < at) f->last_use[i].value = at;
}

/* Max end position of any node below n. */
typedef struct { uint64_t max; } EndAcc;
static bool end_visit(Iron_Visitor *v, Iron_Node *n) {
    EndAcc *a = (EndAcc *)v->ctx;
    uint64_t e = end_of(n->span);
    if (e > a->max) a->max = e;
    return true;
}
static uint64_t subtree_end(Iron_Node *n) {
    EndAcc a = { 0 };
    if (!n) return 0;
    Iron_Visitor v = { .ctx = &a, .visit_node = end_visit, .post_visit = NULL };
    iron_ast_walk(n, &v);
    return a.max;
}

/* Every identifier use of a view binding, with the loops enclosing it:
 * a use inside a loop that starts after the binding's declaration keeps
 * the binding live to that loop's end. */
typedef struct { uint64_t start, end; } LoopSpan;
typedef struct { FnCtx *f; LoopSpan *loops; } UsesCtx;

static bool uses_visit(Iron_Visitor *v, Iron_Node *n) {
    UsesCtx *u = (UsesCtx *)v->ctx;
    if (n->kind == IRON_NODE_WHILE || n->kind == IRON_NODE_FOR) {
        LoopSpan ls = { pos_of(n->span), subtree_end(n) };
        arrput(u->loops, ls);
    }
    if (n->kind == IRON_NODE_IDENT) {
        Iron_Symbol *s = ((Iron_Ident *)n)->resolved_sym;
        if (s && (is_view(s->type) || is_view(((Iron_Ident *)n)->resolved_type))) {
            uint64_t at = pos_of(n->span);
            uint64_t decl = pos_of(s->span);
            for (ptrdiff_t i = 0; i < arrlen(u->loops); i++)
                if (u->loops[i].start > decl && u->loops[i].start <= at &&
                    u->loops[i].end >= at && u->loops[i].end > at)
                    at = at < u->loops[i].end ? u->loops[i].end : at;
            note_use(u->f, s, at);
        }
    }
    return true;
}
static void uses_post(Iron_Visitor *v, Iron_Node *n) {
    UsesCtx *u = (UsesCtx *)v->ctx;
    if ((n->kind == IRON_NODE_WHILE || n->kind == IRON_NODE_FOR) && arrlen(u->loops) > 0)
        (void)arrpop(u->loops);
}

/* ── Borrows and conflicts ─────────────────────────────────────────────── */

static void add_borrow(FnCtx *f, Iron_Symbol *root, uint64_t start, uint64_t end, Iron_Span span) {
    struct Borrow b = { root, start, end, span };
    arrput(f->borrows, b);
}

static void conflict(FnCtx *f, Iron_Node *at, Iron_Symbol *root, const char *what) {
    uint64_t p = pos_of(at->span);
    for (ptrdiff_t i = 0; i < arrlen(f->borrows); i++) {
        struct Borrow *b = &f->borrows[i];
        if (b->root != root || p <= b->start || p > b->end) continue;
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "cannot %s '%s' while a view of it is in use (view taken at line %u)",
                 what, root->name ? root->name : "?", b->span.line);
        report(f->ctx, IRON_ERR_VIEW_SOURCE, at->span, msg,
               "finish using the view first, or take it again after this change");
        return;
    }
}

/* Is the root of a source not borrowable for a view that outlives the
 * expression? Reports E0370 when so. */
static bool check_borrowable(FnCtx *f, Iron_Node *producer, Root r) {
    if (r.kind == ROOT_TEMP) {
        report(f->ctx, IRON_ERR_VIEW_SOURCE, producer->span,
               "a view cannot borrow a temporary value: it is freed at the end of the statement",
               "bind the value to a variable first (`val s = make()`, then `s.bytes()`)");
        return false;
    }
    if (r.kind == ROOT_HANDLE) {
        report(f->ctx, IRON_ERR_VIEW_SOURCE, producer->span,
               "a view cannot borrow a list or String reached through an rc, heap or pointer "
               "handle: another owner could change or free it",
               "copy it into a local first (`val s = p.name`, or `p.items.copy()`)");
        return false;
    }
    return true;
}

static Iron_MethodDecl *find_method(ViewCtx *ctx, const char *type_name, const char *name) {
    if (!type_name || !name) return NULL;
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *d = ctx->program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *m = (Iron_MethodDecl *)d;
        if (m->type_name && m->method_name && strcmp(m->type_name, type_name) == 0 &&
            strcmp(m->method_name, name) == 0)
            return m;
    }
    return NULL;
}

static void check_mutations(FnCtx *f, Iron_Node *n);

typedef struct { FnCtx *f; } MutCtx;

static bool mut_visit(Iron_Visitor *v, Iron_Node *n) {
    FnCtx *f = ((MutCtx *)v->ctx)->f;
    switch ((int)n->kind) {
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)n;
            Iron_Type *tt = expr_type(as->target);
            bool through_view_mut = false;
            if (as->target && as->target->kind == IRON_NODE_INDEX) {
                Iron_Type *ot = expr_type(((Iron_IndexExpr *)as->target)->object);
                through_view_mut = ot && ot->kind == IRON_TYPE_VIEW && ot->view.is_mut;
            }
            if (as->target && !through_view_mut &&
                (as->target->kind == IRON_NODE_INDEX ||
                 !(tt && (iron_type_is_numeric(tt) || tt->kind == IRON_TYPE_BOOL)))) {
                Root r = place_root(f, as->target);
                if (r.kind == ROOT_SYM) conflict(f, n, r.sym, "assign to");
            }
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)n;
            Iron_Type *rt = expr_type(mc->object);
            const char *m = mc->method ? mc->method : "";
            bool mutates = false;
            if (mc->intrinsic) {
                switch (mc->intrinsic) {
                    case IRON_INTR_LIST_SPARE: case IRON_INTR_LIST_COMMIT:
                    case IRON_INTR_SB_PUSH_BYTE:
                    case IRON_INTR_SB_PUSH_BYTES: case IRON_INTR_SB_PUSH_STR:
                    case IRON_INTR_SB_PUSH_INT: case IRON_INTR_SB_PUSH_UINT:
                    case IRON_INTR_SB_SPARE: case IRON_INTR_SB_RESERVE:
                    case IRON_INTR_SB_FINISH: case IRON_INTR_SB_CLEAR:
                        mutates = true;
                        break;
                    default:
                        break;
                }
            } else if (rt && rt->kind == IRON_TYPE_ARRAY) {
                mutates = strcmp(m, "push") == 0 || strcmp(m, "pop") == 0 ||
                          strcmp(m, "set") == 0 || strcmp(m, "reverse") == 0 ||
                          strcmp(m, "sort") == 0 ||
                          strcmp(m, "insert") == 0 || strcmp(m, "remove") == 0 ||
                          strcmp(m, "clear") == 0 || strcmp(m, "take") == 0 ||
                          strcmp(m, "append") == 0 || strcmp(m, "extend") == 0 ||
                          strcmp(m, "truncate") == 0 || strcmp(m, "resize") == 0;
            } else if (rt && rt->kind == IRON_TYPE_OBJECT && rt->object.decl &&
                       rt->object.decl->name) {
                Iron_MethodDecl *md = find_method(f->ctx, rt->object.decl->name, m);
                mutates = md && !md->is_readonly && !md->is_pure;
            }
            if (mutates) {
                Root r = place_root(f, mc->object);
                if (r.kind == ROOT_SYM) {
                    char what[96];
                    snprintf(what, sizeof(what), "call %s() on", m);
                    conflict(f, n, r.sym, what);
                }
            }
            break;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)n;
            Iron_FuncDecl *fd = NULL;
            if (ce->callee && ce->callee->kind == IRON_NODE_IDENT) {
                Iron_Symbol *s = ((Iron_Ident *)ce->callee)->resolved_sym;
                if (s && s->decl_node && s->decl_node->kind == IRON_NODE_FUNC_DECL)
                    fd = (Iron_FuncDecl *)s->decl_node;
            }
            if (!fd) break;
            for (int i = 0; i < ce->arg_count && i < fd->param_count; i++) {
                Iron_Param *p = fd->params[i] && fd->params[i]->kind == IRON_NODE_PARAM
                                    ? (Iron_Param *)fd->params[i] : NULL;
                if (!p || !p->is_var) continue;
                Root r = place_root(f, ce->args[i]);
                if (r.kind == ROOT_SYM) conflict(f, ce->args[i], r.sym, "pass to a var parameter");
            }
            break;
        }
        default:
            break;
    }
    return true;
}

static void check_mutations(FnCtx *f, Iron_Node *body) {
    MutCtx mc = { f };
    Iron_Visitor v = { .ctx = &mc, .visit_node = mut_visit, .post_visit = NULL };
    iron_ast_walk(body, &v);
}

/* Borrows of view bindings, for-loop views and producers whose result
 * outlives the statement; temporary producers are checked here too. */
typedef struct { FnCtx *f; } BorrowCtx;

static void borrow_binding(FnCtx *f, Iron_Node *decl, const char *name, Iron_Node *init,
                           Iron_Type *t) {
    if (!is_view(t) || !init) return;
    Root r = root_of_view(f, init);
    if (r.kind == ROOT_TEMP || r.kind == ROOT_HANDLE) { check_borrowable(f, init, r); return; }
    /* The symbol of the binding: the first use names it. */
    Iron_Symbol *vs = NULL;
    for (ptrdiff_t i = 0; i < hmlen(f->last_use); i++) {
        Iron_Symbol *s = f->last_use[i].key;
        if (!s) continue;
        if (s->decl_node == decl ||
            (s->span.line == decl->span.line && s->name && name && strcmp(s->name, name) == 0 &&
             s->sym_kind == IRON_SYM_VARIABLE)) { vs = s; break; }
    }
    if (vs) hmput(f->roots, vs, r);
    if (r.kind != ROOT_SYM) return;
    uint64_t end = vs ? f->last_use[hmgeti(f->last_use, vs)].value : pos_of(decl->span);
    /* The borrow starts after the initializer has finished. In particular,
     * spare() changes the source in order to create its returned view; that
     * operation must not conflict with the borrow it is creating. */
    add_borrow(f, r.sym, end_of(init->span), end, decl->span);
}

static bool borrow_visit(Iron_Visitor *v, Iron_Node *n) {
    FnCtx *f = ((BorrowCtx *)v->ctx)->f;
    switch ((int)n->kind) {
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *d = (Iron_ValDecl *)n;
            if (d->binding_count == 0) borrow_binding(f, n, d->name, d->init, d->declared_type);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *d = (Iron_VarDecl *)n;
            borrow_binding(f, n, d->name, d->init, d->declared_type);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)n;
            if (as->target && as->target->kind == IRON_NODE_IDENT && is_view(expr_type(as->target))) {
                Iron_Symbol *vs = ((Iron_Ident *)as->target)->resolved_sym;
                Root r = root_of_view(f, as->value);
                if (r.kind == ROOT_TEMP || r.kind == ROOT_HANDLE) { check_borrowable(f, as->value, r); break; }
                if (vs && r.kind == ROOT_SYM) {
                    ptrdiff_t li = hmgeti(f->last_use, vs);
                    add_borrow(f, r.sym, end_of(as->value->span),
                               li >= 0 ? f->last_use[li].value : pos_of(n->span), n->span);
                }
            }
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)n;
            if (is_view(expr_type(fs->iterable))) {
                Root r = root_of_view(f, fs->iterable);
                if (r.kind == ROOT_TEMP || r.kind == ROOT_HANDLE) { check_borrowable(f, fs->iterable, r); break; }
                if (r.kind == ROOT_SYM)
                    add_borrow(f, r.sym, end_of(fs->iterable->span), subtree_end(n),
                               fs->iterable->span);
            }
            break;
        }
        case IRON_NODE_RETURN:
            break;
        default:
            break;
    }
    return true;
}

/* ── Returned views ────────────────────────────────────────────────────── */

typedef struct { FnCtx *f; Iron_Symbol *src; bool ok_param; const char *fname; } RetCtx;

static bool ret_visit(Iron_Visitor *v, Iron_Node *n) {
    RetCtx *rc = (RetCtx *)v->ctx;
    if (n->kind == IRON_NODE_LAMBDA) return false;   /* its own returns */
    if (n->kind != IRON_NODE_RETURN) return true;
    Iron_ReturnStmt *rs = (Iron_ReturnStmt *)n;
    if (!rs->value || !rc->ok_param) return true;
    Root r = root_of_view(rc->f, rs->value);
    if (r.kind == ROOT_STATIC) return true;
    if (r.kind == ROOT_SYM && r.sym == rc->src) return true;
    if (r.kind == ROOT_SYM && rc->src && r.sym && r.sym->name && rc->src->name &&
        strcmp(r.sym->name, rc->src->name) == 0 && r.sym->sym_kind == IRON_SYM_PARAM)
        return true;
    char msg[320];
    snprintf(msg, sizeof(msg),
             "'%s' returns a view that does not come from its parameter '%s'",
             rc->fname ? rc->fname : "?", rc->src && rc->src->name ? rc->src->name : "?");
    report(rc->f->ctx, IRON_ERR_VIEW_ESCAPE, rs->value->span, msg,
           "a returned view must be (a slice of) a view of the one view, String or list "
           "parameter; return positions or a copy (to_list(), to_string()) otherwise");
    return true;
}

/* Find the parameter symbol: the first identifier use naming it. */
typedef struct { const char *name; Iron_Node *param; Iron_Symbol *sym; } ParamFind;
static bool param_find_visit(Iron_Visitor *v, Iron_Node *n) {
    ParamFind *p = (ParamFind *)v->ctx;
    if (p->sym) return false;
    if (n->kind == IRON_NODE_IDENT) {
        Iron_Ident *id = (Iron_Ident *)n;
        if (id->resolved_sym && id->resolved_sym->sym_kind == IRON_SYM_PARAM &&
            id->name && p->name && strcmp(id->name, p->name) == 0)
            p->sym = id->resolved_sym;
    }
    return true;
}

static void check_function(ViewCtx *ctx, const char *fname, Iron_Node **params, int pcount,
                           int first_param, Iron_Type *ret_t, Iron_Node *ret_ann,
                           Iron_Node *body, Iron_Span span) {
    for (int i = 0; i < pcount; i++) {
        if (!params[i] || params[i]->kind != IRON_NODE_PARAM) continue;
        Iron_Param *p = (Iron_Param *)params[i];
        check_type_pos(ctx, p->resolved_type, p->span, "a parameter type");
    }
    check_type_pos(ctx, ret_t, ret_ann ? ret_ann->span : span, "a result type");
    if (!body) return;

    FnCtx f = { 0 };
    f.ctx = ctx;
    UsesCtx u = { &f, NULL };
    Iron_Visitor uv = { .ctx = &u, .visit_node = uses_visit, .post_visit = uses_post };
    iron_ast_walk(body, &uv);
    arrfree(u.loops);

    BorrowCtx bc = { &f };
    Iron_Visitor bv = { .ctx = &bc, .visit_node = borrow_visit, .post_visit = NULL };
    iron_ast_walk(body, &bv);
    check_mutations(&f, body);

    if (is_view(ret_t)) {
        int n = 0; Iron_Param *src = NULL;
        for (int i = first_param; i < pcount; i++) {
            if (!params[i] || params[i]->kind != IRON_NODE_PARAM) continue;
            Iron_Param *p = (Iron_Param *)params[i];
            Iron_Type *pt = p->resolved_type;
            if (pt && (pt->kind == IRON_TYPE_VIEW || pt->kind == IRON_TYPE_STRING ||
                       pt->kind == IRON_TYPE_ARRAY)) { n++; src = p; }
        }
        RetCtx rc = { &f, NULL, n == 1, fname };
        if (n != 1) {
            char msg[320];
            snprintf(msg, sizeof(msg),
                     "'%s' returns a view but has %d view, String or list parameters (it needs exactly one)",
                     fname ? fname : "?", n);
            report(ctx, IRON_ERR_VIEW_ESCAPE, ret_ann ? ret_ann->span : span, msg,
                   "the result can only borrow from a single source; return positions "
                   "or a copy instead");
        } else {
            ParamFind pf = { src->name, (Iron_Node *)src, NULL };
            Iron_Visitor pv = { .ctx = &pf, .visit_node = param_find_visit, .post_visit = NULL };
            iron_ast_walk(body, &pv);
            rc.src = pf.sym;
            Iron_Visitor rv = { .ctx = &rc, .visit_node = ret_visit, .post_visit = NULL };
            iron_ast_walk(body, &rv);
        }
    }

    hmfree(f.roots);
    hmfree(f.last_use);
    arrfree(f.borrows);
}

/* Expression and local types: a view inside a list literal, a tuple, a
 * nullable binding... */
typedef struct { ViewCtx *ctx; } TypePosCtx;
static bool typepos_visit(Iron_Visitor *v, Iron_Node *n) {
    ViewCtx *ctx = ((TypePosCtx *)v->ctx)->ctx;
    switch ((int)n->kind) {
        case IRON_NODE_VAL_DECL:
        case IRON_NODE_VAR_DECL:
            check_type_pos(ctx, expr_type(n), n->span, "a variable of this type");
            break;
        case IRON_NODE_ARRAY_LIT:
        case IRON_NODE_CONSTRUCT:
            check_type_pos(ctx, expr_type(n), n->span, "this value");
            break;
        default:
            break;
    }
    return true;
}

void iron_view_check(Iron_Program *program, Iron_Arena *arena, Iron_DiagList *diags,
                     const _Atomic bool *cancel_flag) {
    if (!program) return;
    ViewCtx ctx = { program, arena, diags };
    for (int i = 0; i < program->decl_count; i++) {
        if (cancel_flag && atomic_load_explicit(cancel_flag, memory_order_relaxed)) return;
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        switch ((int)d->kind) {
            case IRON_NODE_OBJECT_DECL: {
                Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
                for (int k = 0; k < od->field_count; k++) {
                    Iron_Field *fld = (Iron_Field *)od->fields[k];
                    if (!fld) continue;
                    Iron_Type *ft = fld->resolved_type ? fld->resolved_type : fld->field_type_cached;
                    if (is_view(ft) || nested_view(ft, 0)) {
                        char msg[320];
                        snprintf(msg, sizeof(msg), "field '%s' cannot hold a view ('%s')",
                                 fld->name ? fld->name : "?", iron_type_to_string(ft, arena));
                        report(&ctx, IRON_ERR_VIEW_ESCAPE, fld->span, msg,
                               "a view only borrows its source for the block it lives in: "
                               "store the source (a list or String) and positions instead");
                    }
                }
                break;
            }
            case IRON_NODE_VAL_DECL:
            case IRON_NODE_VAR_DECL: {
                Iron_Type *t = expr_type(d);
                if (is_view(t) || nested_view(t, 0)) {
                    report(&ctx, IRON_ERR_VIEW_ESCAPE, d->span,
                           "a global cannot hold a view",
                           "a view only borrows its source for the block it lives in");
                }
                break;
            }
            case IRON_NODE_FUNC_DECL: {
                Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
                if (fd->generic_param_count > 0) break;   /* checked per instance */
                check_function(&ctx, fd->name, fd->params, fd->param_count, 0,
                               fd->resolved_return_type, fd->return_type, fd->body, fd->span);
                if (fd->body) {
                    TypePosCtx tp = { &ctx };
                    Iron_Visitor tv = { .ctx = &tp, .visit_node = typepos_visit, .post_visit = NULL };
                    iron_ast_walk(fd->body, &tv);
                }
                break;
            }
            case IRON_NODE_METHOD_DECL: {
                Iron_MethodDecl *md = (Iron_MethodDecl *)d;
                if (md->generic_param_count > 0) break;
                int first = md->is_receiver_form ? 1 : 0;
                check_function(&ctx, md->method_name, md->params, md->param_count, first,
                               md->resolved_return_type, md->return_type, md->body, md->span);
                if (md->body) {
                    TypePosCtx tp = { &ctx };
                    Iron_Visitor tv = { .ctx = &tp, .visit_node = typepos_visit, .post_visit = NULL };
                    iron_ast_walk(md->body, &tv);
                }
                break;
            }
            default:
                break;
        }
    }
}

/* ── Compiler hints that cannot be honored ─────────────────────────────── */

typedef struct { Iron_Node *self; bool hit; } SelfCall;
static bool self_call_visit(Iron_Visitor *v, Iron_Node *n) {
    SelfCall *sc = (SelfCall *)v->ctx;
    if (sc->hit) return false;
    if (n->kind == IRON_NODE_CALL) {
        Iron_CallExpr *ce = (Iron_CallExpr *)n;
        if (ce->callee && ce->callee->kind == IRON_NODE_IDENT) {
            Iron_Symbol *s = ((Iron_Ident *)ce->callee)->resolved_sym;
            if (s && s->decl_node == sc->self) sc->hit = true;
        }
    }
    return true;
}

void iron_hint_check(Iron_Program *program, Iron_Arena *arena, Iron_DiagList *diags) {
    if (!program) return;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_FUNC_DECL) continue;
        Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
        if (!(fd->hints & IRON_HINT_INLINE) || !fd->body) continue;
        SelfCall sc = { d, false };
        Iron_Visitor v = { .ctx = &sc, .visit_node = self_call_visit, .post_visit = NULL };
        iron_ast_walk(fd->body, &v);
        if (!sc.hit) continue;
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "`//iron:inline` cannot be honored: '%s' calls itself",
                 fd->name ? fd->name : "?");
        iron_diag_emit(diags, arena, IRON_DIAG_WARNING, IRON_WARN_INLINE_NOT_HONORED, fd->span,
                       iron_arena_strdup(arena, msg, strlen(msg)),
                       "a recursive function is inlined at most one level; remove the hint");
    }
}

/* ── //iron:align ──────────────────────────────────────────────────────────
 * E0368: an alignment below the natural one of what it applies to, or an
 * over-aligned (above 16 bytes) value in storage that cannot honor it yet.
 * The runtime's heap, rc and arena allocators, closure and spawn
 * environments, interface payloads and Map / Set tables return 16-byte
 * aligned memory; their over-aligned variants come with the generation
 * table of the pointer safety work (#567). Stack values, fields, fixed
 * arrays and list storage honor any alignment now. */

static unsigned stated_align(uint16_t a) {
    return a == IRON_ALIGN_CACHELINE ? 128u : (unsigned)a;   /* the larger cacheline */
}

static unsigned type_align(const Iron_Type *t, int depth);

static unsigned object_align(const Iron_ObjectDecl *od, int depth) {
    if (!od || depth > 16) return 8;
    unsigned a = stated_align(od->align);
    if (a < 1) a = 1;
    for (int i = 0; i < od->field_count; i++) {
        Iron_Field *f = (Iron_Field *)od->fields[i];
        if (!f) continue;
        Iron_Type *ft = f->resolved_type ? f->resolved_type : f->field_type_cached;
        unsigned fa = type_align(ft, depth + 1);
        if (stated_align(f->align) > fa) fa = stated_align(f->align);
        if (fa > a) a = fa;
    }
    return a;
}

/* Alignment of a value of type t as Iron lays it out (an estimate that is
 * exact for the cases the checks need: scalars and over-aligned objects). */
static unsigned type_align(const Iron_Type *t, int depth) {
    if (!t || depth > 16) return 1;
    switch ((int)t->kind) {
        case IRON_TYPE_BOOL: case IRON_TYPE_INT8: case IRON_TYPE_UINT8: return 1;
        case IRON_TYPE_INT16: case IRON_TYPE_UINT16: return 2;
        case IRON_TYPE_INT32: case IRON_TYPE_UINT32: case IRON_TYPE_FLOAT32: return 4;
        case IRON_TYPE_OBJECT:
            if (t->object.elem) return 8;   /* runtime containers: handles */
            return object_align(t->object.decl, depth);
        case IRON_TYPE_ARRAY:
            return t->array.size >= 0 ? type_align(t->array.elem, depth + 1) : 8;
        case IRON_TYPE_NULLABLE: return type_align(t->nullable.inner, depth + 1);
        case IRON_TYPE_TUPLE: {
            unsigned a = 1;
            for (int i = 0; i < t->tuple.elem_count; i++) {
                unsigned e = type_align(t->tuple.elem_types[i], depth + 1);
                if (e > a) a = e;
            }
            return a;
        }
        default:
            return 8;
    }
}

static bool over_aligned(const Iron_Type *t) { return type_align(t, 0) > 16; }

/* Does a type hold an over-aligned value by value inside storage the
 * runtime allocates (Map / Set entries, enum payloads)? */
static bool over_aligned_in_container(const Iron_Type *t, int depth) {
    if (!t || depth > 16) return false;
    switch ((int)t->kind) {
        case IRON_TYPE_OBJECT:
            if (t->object.elem && t->object.decl && t->object.decl->name &&
                (strcmp(t->object.decl->name, "Map") == 0 || strcmp(t->object.decl->name, "Set") == 0 ||
                 strcmp(t->object.decl->name, "Box") == 0 || strcmp(t->object.decl->name, "Mutex") == 0 ||
                 strcmp(t->object.decl->name, "Channel") == 0 || strcmp(t->object.decl->name, "RWLock") == 0))
                return over_aligned(t->object.elem) || (t->object.elem2 && over_aligned(t->object.elem2));
            return false;
        case IRON_TYPE_ARRAY:    return over_aligned_in_container(t->array.elem, depth + 1);
        case IRON_TYPE_NULLABLE: return over_aligned_in_container(t->nullable.inner, depth + 1);
        case IRON_TYPE_ENUM:
            for (int i = 0; i < t->enu.type_arg_count; i++)
                if (over_aligned(t->enu.type_args[i])) return true;
            return false;
        default:
            return false;
    }
}

static void align_unsupported(Iron_Arena *arena, Iron_DiagList *diags, Iron_Span span,
                              const Iron_Type *t, const char *where) {
    char msg[320];
    snprintf(msg, sizeof(msg),
             "'%s' is aligned above 16 bytes, which %s cannot provide yet",
             iron_type_to_string(t, arena), where);
    iron_diag_emit(diags, arena, IRON_DIAG_ERROR, IRON_ERR_ALIGN_VALUE, span,
                   iron_arena_strdup(arena, msg, strlen(msg)),
                   "keep it on the stack, in a field of a stack value, in a fixed array "
                   "or in a list, or lower its //iron:align to 16");
}

typedef struct { Iron_Arena *arena; Iron_DiagList *diags; } AlignCtx;

static bool align_visit(Iron_Visitor *v, Iron_Node *n) {
    AlignCtx *ac = (AlignCtx *)v->ctx;
    switch ((int)n->kind) {
        case IRON_NODE_HEAP: {
            Iron_Type *t = ((Iron_HeapExpr *)n)->resolved_type;
            Iron_Type *inner = ((Iron_HeapExpr *)n)->inner ? expr_type(((Iron_HeapExpr *)n)->inner) : NULL;
            if (inner && over_aligned(inner)) align_unsupported(ac->arena, ac->diags, n->span, inner, "a heap or arena allocation");
            else if (!inner && t && t->kind == IRON_TYPE_PTR && over_aligned(t->ptr.pointee))
                align_unsupported(ac->arena, ac->diags, n->span, t->ptr.pointee, "a heap or arena allocation");
            break;
        }
        case IRON_NODE_RC: {
            Iron_Type *inner = ((Iron_RcExpr *)n)->inner ? expr_type(((Iron_RcExpr *)n)->inner) : NULL;
            if (inner && over_aligned(inner)) align_unsupported(ac->arena, ac->diags, n->span, inner, "an rc allocation");
            break;
        }
        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *le = (Iron_LambdaExpr *)n;
            for (int i = 0; i < le->capture_count; i++)
                if (le->captures && over_aligned(le->captures[i].type)) {
                    align_unsupported(ac->arena, ac->diags, n->span, le->captures[i].type,
                                      "a closure environment");
                    break;
                }
            break;
        }
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)n;
            for (int i = 0; i < ss->capture_count; i++)
                if (ss->captures && over_aligned(ss->captures[i].type)) {
                    align_unsupported(ac->arena, ac->diags, n->span, ss->captures[i].type,
                                      "a spawn environment");
                    break;
                }
            break;
        }
        case IRON_NODE_VAL_DECL:
        case IRON_NODE_VAR_DECL: {
            Iron_Type *t = expr_type(n);
            uint16_t a = n->kind == IRON_NODE_VAL_DECL ? ((Iron_ValDecl *)n)->align
                                                       : ((Iron_VarDecl *)n)->align;
            if (a && a != IRON_ALIGN_CACHELINE && t && (unsigned)a < type_align(t, 0)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "//iron:align=%u is below the natural alignment of '%s' (%u)",
                         (unsigned)a, iron_type_to_string(t, ac->arena), type_align(t, 0));
                iron_diag_emit(ac->diags, ac->arena, IRON_DIAG_ERROR, IRON_ERR_ALIGN_VALUE, n->span,
                               iron_arena_strdup(ac->arena, msg, strlen(msg)),
                               "an alignment hint only raises alignment");
            }
            if (t && over_aligned_in_container(t, 0))
                align_unsupported(ac->arena, ac->diags, n->span, t, "this container");
            break;
        }
        default:
            break;
    }
    return true;
}

void iron_align_check(Iron_Program *program, Iron_Arena *arena, Iron_DiagList *diags) {
    if (!program) return;
    AlignCtx ac = { arena, diags };
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        if (d->kind == IRON_NODE_OBJECT_DECL) {
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
            unsigned natural = 1;
            for (int k = 0; k < od->field_count; k++) {
                Iron_Field *f = (Iron_Field *)od->fields[k];
                if (!f) continue;
                Iron_Type *ft = f->resolved_type ? f->resolved_type : f->field_type_cached;
                unsigned fa = type_align(ft, 0);
                if (f->align && f->align != IRON_ALIGN_CACHELINE && (unsigned)f->align < fa) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "//iron:align=%u is below the natural alignment of field '%s' (%u)",
                             (unsigned)f->align, f->name ? f->name : "?", fa);
                    iron_diag_emit(diags, arena, IRON_DIAG_ERROR, IRON_ERR_ALIGN_VALUE, f->span,
                                   iron_arena_strdup(arena, msg, strlen(msg)),
                                   "an alignment hint only raises alignment");
                }
                if (stated_align(f->align) > fa) fa = stated_align(f->align);
                if (fa > natural) natural = fa;
                if (ft && over_aligned_in_container(ft, 0))
                    align_unsupported(arena, diags, f->span, ft, "this container");
            }
            if (od->align && od->align != IRON_ALIGN_CACHELINE && (unsigned)od->align < natural) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "//iron:align=%u is below the natural alignment of '%s' (%u)",
                         (unsigned)od->align, od->name ? od->name : "?", natural);
                iron_diag_emit(diags, arena, IRON_DIAG_ERROR, IRON_ERR_ALIGN_VALUE, od->span,
                               iron_arena_strdup(arena, msg, strlen(msg)),
                               "an alignment hint only raises alignment");
            }
            if (od->implements_count > 0 && object_align(od, 0) > 16) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "'%s' is aligned above 16 bytes, which an interface value cannot hold yet",
                         od->name ? od->name : "?");
                iron_diag_emit(diags, arena, IRON_DIAG_ERROR, IRON_ERR_ALIGN_VALUE, od->span,
                               iron_arena_strdup(arena, msg, strlen(msg)),
                               "lower its //iron:align to 16, or do not implement interfaces");
            }
            continue;
        }
        Iron_Node *body = d->kind == IRON_NODE_FUNC_DECL ? ((Iron_FuncDecl *)d)->body
                        : d->kind == IRON_NODE_METHOD_DECL ? ((Iron_MethodDecl *)d)->body : NULL;
        if (!body) continue;
        Iron_Visitor v = { .ctx = &ac, .visit_node = align_visit, .post_visit = NULL };
        iron_ast_walk(body, &v);
    }
}
