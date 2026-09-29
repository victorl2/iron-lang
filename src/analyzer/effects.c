/* Transitive effect inference for the purity tiers. See effects.h. */
#include "analyzer/effects.h"
#include "analyzer/types.h"
#include "vendor/stb_ds.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* HARD-05: cooperative cancellation (same helper as the other passes). */
static inline bool iron_cancel_requested(const _Atomic bool *flag) {
    return flag && atomic_load_explicit(flag, memory_order_relaxed);
}

/* One node of the effect graph: a user free function or method. */
typedef struct {
    Iron_Node  *decl;      /* Iron_FuncDecl or Iron_MethodDecl */
    Iron_Node  *body;
    bool        io;        /* can perform I/O */
    bool        global_write;
    int        *callees;   /* stb_ds: indices into the node array */
} EffNode;

typedef struct {
    Iron_Program  *program;
    EffNode       *nodes;       /* stb_ds */
    struct { Iron_Node *key; int value; } *index;   /* decl -> node */
    struct { Iron_Node *key; bool value; } *globals; /* top-level bindings */
    int            cur;         /* node being scanned */
    /* reporting */
    Iron_Arena    *arena;
    Iron_DiagList *diags;
    bool           in_pure;
} EffCtx;

static bool is_io_builtin(const char *name) {
    return name && (strcmp(name, "println") == 0 || strcmp(name, "print") == 0 ||
                    strcmp(name, "readline") == 0);
}

/* Stdlib modules whose calls do I/O (same set the readonly check uses). */
static bool is_io_module(const char *name) {
    return name && (strcmp(name, "IO") == 0 || strcmp(name, "Log") == 0 ||
                    strcmp(name, "Net") == 0 || strcmp(name, "Raylib") == 0);
}

static Iron_Ident *root_ident(Iron_Node *n) {
    while (n) {
        if (n->kind == IRON_NODE_FIELD_ACCESS) n = ((Iron_FieldAccess *)n)->object;
        else if (n->kind == IRON_NODE_INDEX) n = ((Iron_IndexExpr *)n)->object;
        else break;
    }
    return (n && n->kind == IRON_NODE_IDENT) ? (Iron_Ident *)n : NULL;
}

static bool is_global_ident(EffCtx *c, Iron_Ident *id) {
    return id && id->resolved_sym && id->resolved_sym->decl_node &&
           hmgeti(c->globals, id->resolved_sym->decl_node) >= 0;
}

/* The method decl a call resolves to, from the receiver's static type. */
static Iron_MethodDecl *method_target(EffCtx *c, Iron_MethodCallExpr *mc) {
    if (!mc->object || !mc->method) return NULL;
    Iron_Type *t = ((Iron_ExprNode *)mc->object)->resolved_type;
    if (t && t->kind == IRON_TYPE_RC) t = t->rc.inner;
    if (t && t->kind == IRON_TYPE_PTR) t = t->ptr.pointee;
    if (!t || t->kind != IRON_TYPE_OBJECT || !t->object.decl) return NULL;
    const char *tn = t->object.decl->name;
    for (int i = 0; i < c->program->decl_count; i++) {
        Iron_Node *d = c->program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (md->type_name && md->method_name && tn &&
            strcmp(md->type_name, tn) == 0 && strcmp(md->method_name, mc->method) == 0)
            return md;
    }
    return NULL;
}

static bool is_list_mutator(const char *m) {
    static const char *const k[] = { "push", "pop", "set", "insert", "remove",
                                     "clear", "reverse", "sort", NULL };
    for (int i = 0; k[i]; i++) if (m && strcmp(m, k[i]) == 0) return true;
    return false;
}

/* ── Traversal ──────────────────────────────────────────────────────────── */

typedef void (*EffVisit)(EffCtx *c, Iron_Node *n);

static void walk(EffCtx *c, Iron_Node *n, EffVisit visit);

static void walk_all(EffCtx *c, Iron_Node **ns, int count, EffVisit visit) {
    for (int i = 0; ns && i < count; i++) walk(c, ns[i], visit);
}

static void walk(EffCtx *c, Iron_Node *n, EffVisit visit) {
    if (!n) return;
    visit(c, n);
    switch ((int)n->kind) {
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)n;
            walk_all(c, b->stmts, b->stmt_count, visit);
            break;
        }
        case IRON_NODE_VAR_DECL: walk(c, ((Iron_VarDecl *)n)->init, visit); break;
        case IRON_NODE_VAL_DECL: walk(c, ((Iron_ValDecl *)n)->init, visit); break;
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)n;
            walk(c, as->target, visit);
            walk(c, as->value, visit);
            break;
        }
        case IRON_NODE_RETURN: walk(c, ((Iron_ReturnStmt *)n)->value, visit); break;
        case IRON_NODE_IF: {
            Iron_IfStmt *is_ = (Iron_IfStmt *)n;
            walk(c, is_->condition, visit);
            walk(c, is_->body, visit);
            walk_all(c, is_->elif_conds, is_->elif_count, visit);
            walk_all(c, is_->elif_bodies, is_->elif_count, visit);
            walk(c, is_->else_body, visit);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *w = (Iron_WhileStmt *)n;
            walk(c, w->condition, visit);
            walk(c, w->body, visit);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *f = (Iron_ForStmt *)n;
            walk(c, f->iterable, visit);
            walk(c, f->body, visit);
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *m = (Iron_MatchStmt *)n;
            walk(c, m->subject, visit);
            walk_all(c, m->cases, m->case_count, visit);
            walk(c, m->else_body, visit);
            break;
        }
        case IRON_NODE_MATCH_CASE: walk(c, ((Iron_MatchCase *)n)->body, visit); break;
        case IRON_NODE_DEFER: walk(c, ((Iron_DeferStmt *)n)->expr, visit); break;
        case IRON_NODE_SPAWN: walk(c, ((Iron_SpawnStmt *)n)->body, visit); break;
        case IRON_NODE_LAMBDA: walk(c, ((Iron_LambdaExpr *)n)->body, visit); break;
        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *is_ = (Iron_InterpString *)n;
            walk_all(c, is_->parts, is_->part_count, visit);
            break;
        }
        case IRON_NODE_BINARY:
            walk(c, ((Iron_BinaryExpr *)n)->left, visit);
            walk(c, ((Iron_BinaryExpr *)n)->right, visit);
            break;
        case IRON_NODE_UNARY: walk(c, ((Iron_UnaryExpr *)n)->operand, visit); break;
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)n;
            walk(c, ce->callee, visit);
            walk_all(c, ce->args, ce->arg_count, visit);
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)n;
            walk(c, mc->object, visit);
            walk_all(c, mc->args, mc->arg_count, visit);
            break;
        }
        case IRON_NODE_FIELD_ACCESS: walk(c, ((Iron_FieldAccess *)n)->object, visit); break;
        case IRON_NODE_INDEX:
            walk(c, ((Iron_IndexExpr *)n)->object, visit);
            walk(c, ((Iron_IndexExpr *)n)->index, visit);
            break;
        case IRON_NODE_SLICE:
            walk(c, ((Iron_SliceExpr *)n)->object, visit);
            walk(c, ((Iron_SliceExpr *)n)->start, visit);
            walk(c, ((Iron_SliceExpr *)n)->end, visit);
            break;
        case IRON_NODE_HEAP: walk(c, ((Iron_HeapExpr *)n)->inner, visit); break;
        case IRON_NODE_RC: walk(c, ((Iron_RcExpr *)n)->inner, visit); break;
        case IRON_NODE_COMPTIME: walk(c, ((Iron_ComptimeExpr *)n)->inner, visit); break;
        case IRON_NODE_IS: walk(c, ((Iron_IsExpr *)n)->expr, visit); break;
        case IRON_NODE_AWAIT: walk(c, ((Iron_AwaitExpr *)n)->handle, visit); break;
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ct = (Iron_ConstructExpr *)n;
            walk_all(c, ct->args, ct->arg_count, visit);
            break;
        }
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)n;
            walk_all(c, al->elements, al->element_count, visit);
            break;
        }
        case IRON_NODE_ENUM_CONSTRUCT: {
            Iron_EnumConstruct *ec = (Iron_EnumConstruct *)n;
            walk_all(c, ec->args, ec->arg_count, visit);
            break;
        }
        /* -Wswitch-enum opt-out: leaves and declarations have no calls. */
        default:
            break;
    }
}

/* ── Direct effects and call edges ──────────────────────────────────────── */

static void add_edge(EffCtx *c, Iron_Node *callee_decl) {
    ptrdiff_t k = hmgeti(c->index, callee_decl);
    if (k >= 0) arrput(c->nodes[c->cur].callees, c->index[k].value);
}

static void visit_direct(EffCtx *c, Iron_Node *n) {
    EffNode *self = &c->nodes[c->cur];
    switch ((int)n->kind) {
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)n;
            if (!ce->callee || ce->callee->kind != IRON_NODE_IDENT) break;
            Iron_Ident *id = (Iron_Ident *)ce->callee;
            Iron_Symbol *sym = id->resolved_sym;
            Iron_Node *d = sym ? sym->decl_node : NULL;
            if (d && d->kind == IRON_NODE_FUNC_DECL) {
                if (((Iron_FuncDecl *)d)->is_extern) self->io = true;
                else add_edge(c, d);
            } else if (!d && is_io_builtin(id->name)) {
                self->io = true;
            }
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)n;
            if (mc->object && mc->object->kind == IRON_NODE_IDENT) {
                Iron_Ident *oid = (Iron_Ident *)mc->object;
                if (oid->resolved_sym && oid->resolved_sym->sym_kind == IRON_SYM_TYPE &&
                    is_io_module(oid->name)) {
                    self->io = true;
                    break;
                }
            }
            Iron_MethodDecl *md = method_target(c, mc);
            if (md) add_edge(c, (Iron_Node *)md);
            /* A mutating call on a global writes global state. */
            Iron_Ident *root = root_ident(mc->object);
            if (is_global_ident(c, root)) {
                Iron_Type *t = ((Iron_ExprNode *)mc->object)->resolved_type;
                bool mutating = md ? (!md->is_readonly && !md->is_pure)
                                   : (t && t->kind == IRON_TYPE_ARRAY &&
                                      is_list_mutator(mc->method));
                if (mutating) self->global_write = true;
            }
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)n;
            if (is_global_ident(c, root_ident(as->target))) self->global_write = true;
            break;
        }
        /* -Wswitch-enum opt-out: only calls and assignments carry effects. */
        default:
            break;
    }
}

/* ── Reporting ──────────────────────────────────────────────────────────── */

static EffNode *node_of(EffCtx *c, Iron_Node *decl) {
    ptrdiff_t k = hmgeti(c->index, decl);
    return k >= 0 ? &c->nodes[c->index[k].value] : NULL;
}

static void visit_report(EffCtx *c, Iron_Node *n) {
    if (n->kind != IRON_NODE_CALL) return;
    Iron_CallExpr *ce = (Iron_CallExpr *)n;
    if (!ce->callee || ce->callee->kind != IRON_NODE_IDENT) return;
    Iron_Ident *id = (Iron_Ident *)ce->callee;
    Iron_Node *d = id->resolved_sym ? id->resolved_sym->decl_node : NULL;
    if (!d || d->kind != IRON_NODE_FUNC_DECL) return;
    Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
    bool io, gw;
    if (fd->is_extern) {
        io = true; gw = false;
    } else {
        EffNode *e = node_of(c, d);
        if (!e) return;
        io = e->io; gw = e->global_write;
    }
    char msg[320];
    if (c->in_pure && (io || gw)) {
        snprintf(msg, sizeof(msg),
                 "cannot call '%s' from a pure method: it %s",
                 fd->name ? fd->name : "?",
                 io && gw ? "performs I/O and writes global state"
                 : io ? "performs I/O" : "writes global state");
        iron_diag_emit(c->diags, c->arena, IRON_DIAG_ERROR,
                       IRON_ERR_PURE_NON_PURE_CALL, ce->span,
                       iron_arena_strdup(c->arena, msg, strlen(msg)),
                       "pure methods may only call functions without I/O "
                       "or global writes");
    } else if (!c->in_pure && io) {
        snprintf(msg, sizeof(msg),
                 "cannot call '%s' from a readonly method: it performs I/O",
                 fd->name ? fd->name : "?");
        iron_diag_emit(c->diags, c->arena, IRON_DIAG_ERROR,
                       IRON_ERR_READONLY_IO, ce->span,
                       iron_arena_strdup(c->arena, msg, strlen(msg)),
                       "readonly methods may not perform I/O, directly or "
                       "through the functions they call");
    }
}

void iron_effects_check(Iron_Program *program,
                        Iron_Scope *global_scope,
                        Iron_Arena *arena,
                        Iron_DiagList *diags,
                        const _Atomic bool *cancel_flag) {
    (void)global_scope;
    if (!program) return;
    EffCtx c;
    memset(&c, 0, sizeof(c));
    c.program = program;
    c.arena = arena;
    c.diags = diags;

    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        if (d->kind == IRON_NODE_VAR_DECL || d->kind == IRON_NODE_VAL_DECL) {
            hmput(c.globals, d, true);
        } else if (d->kind == IRON_NODE_FUNC_DECL || d->kind == IRON_NODE_METHOD_DECL) {
            Iron_Node *body = d->kind == IRON_NODE_FUNC_DECL
                ? ((Iron_FuncDecl *)d)->body : ((Iron_MethodDecl *)d)->body;
            EffNode e;
            memset(&e, 0, sizeof(e));
            e.decl = d;
            e.body = body;
            hmput(c.index, d, (int)arrlen(c.nodes));
            arrput(c.nodes, e);
        }
    }

    /* Direct effects and edges. */
    for (int i = 0; i < (int)arrlen(c.nodes); i++) {
        if (iron_cancel_requested(cancel_flag)) goto done;
        c.cur = i;
        walk(&c, c.nodes[i].body, visit_direct);
    }

    /* Propagate along call edges to a fixed point. */
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = 0; i < (int)arrlen(c.nodes); i++) {
            EffNode *e = &c.nodes[i];
            for (int k = 0; k < (int)arrlen(e->callees); k++) {
                EffNode *t = &c.nodes[e->callees[k]];
                if (t->io && !e->io) { e->io = true; changed = true; }
                if (t->global_write && !e->global_write) {
                    e->global_write = true; changed = true;
                }
            }
        }
    }

    /* Report calls from pure / readonly methods. */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (!md->is_pure && !md->is_readonly) continue;
        c.in_pure = md->is_pure;
        walk(&c, md->body, visit_report);
    }

done:
    for (int i = 0; i < (int)arrlen(c.nodes); i++) arrfree(c.nodes[i].callees);
    arrfree(c.nodes);
    hmfree(c.index);
    hmfree(c.globals);
}
