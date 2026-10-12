/* Initialization order of global bindings. See global_order.h. */

#include "analyzer/global_order.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    Iron_Program *program;
    struct { Iron_Node *key; int value; } *global_index; /* decl -> index */
    struct { Iron_Node *key; int value; } *seen;         /* bodies walked */
    Iron_Node **pending;                                  /* bodies to walk */
    bool       *deps;                                     /* [global count] */
} GoWalk;

static bool is_global_decl(Iron_Node *d) {
    if (!d) return false;
    if (d->kind == IRON_NODE_VAR_DECL) return ((Iron_VarDecl *)d)->name != NULL;
    if (d->kind == IRON_NODE_VAL_DECL) {
        Iron_ValDecl *vd = (Iron_ValDecl *)d;
        return vd->name != NULL && vd->binding_count == 0;
    }
    return false;
}

static Iron_Node *global_init(Iron_Node *d) {
    if (d->kind == IRON_NODE_VAR_DECL) return ((Iron_VarDecl *)d)->init;
    return ((Iron_ValDecl *)d)->init;
}

static const char *global_name(Iron_Node *d) {
    if (d->kind == IRON_NODE_VAR_DECL) return ((Iron_VarDecl *)d)->name;
    return ((Iron_ValDecl *)d)->name;
}

static void queue_body(GoWalk *w, Iron_Node *decl) {
    if (!decl || hmgeti(w->seen, decl) >= 0) return;
    hmput(w->seen, decl, 1);
    arrput(w->pending, decl);
}

/* The methods named `method` (or every init when method is NULL) of the
 * object `type_name`. */
static void queue_methods(GoWalk *w, const char *type_name, const char *method) {
    if (!type_name) return;
    for (int i = 0; i < w->program->decl_count; i++) {
        Iron_Node *d = w->program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (!md->type_name || strcmp(md->type_name, type_name) != 0) continue;
        if (method ? (md->method_name && strcmp(md->method_name, method) == 0) : md->is_init)
            queue_body(w, d);
    }
}

static const char *object_name(Iron_Type *t) {
    if (t && t->kind == IRON_TYPE_OBJECT && t->object.decl) return t->object.decl->name;
    return NULL;
}

static bool go_visit(Iron_Visitor *v, Iron_Node *node) {
    GoWalk *w = (GoWalk *)v->ctx;
    if (!node) return false;
    if (node->kind == IRON_NODE_IDENT) {
        Iron_Ident *id = (Iron_Ident *)node;
        Iron_Symbol *s = id->resolved_sym;
        if (!s || !s->decl_node) return true;
        if (s->sym_kind == IRON_SYM_VARIABLE) {
            ptrdiff_t gi = hmgeti(w->global_index, s->decl_node);
            if (gi >= 0) w->deps[w->global_index[gi].value] = true;
        } else if (s->sym_kind == IRON_SYM_FUNCTION) {
            queue_body(w, s->decl_node);
        } else if (s->sym_kind == IRON_SYM_TYPE) {
            /* Name(args): the object's initializers run. */
            if (s->decl_node->kind == IRON_NODE_OBJECT_DECL)
                queue_methods(w, ((Iron_ObjectDecl *)s->decl_node)->name, NULL);
        }
        return true;
    }
    if (node->kind == IRON_NODE_METHOD_CALL) {
        Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
        Iron_Node *obj = mc->object;
        Iron_Type *t = obj ? ((Iron_ExprNode *)obj)->resolved_type : NULL;
        const char *tn = object_name(t);
        /* Type.named_init(args) */
        if (!tn && obj && obj->kind == IRON_NODE_IDENT) {
            Iron_Symbol *s = ((Iron_Ident *)obj)->resolved_sym;
            if (s && s->sym_kind == IRON_SYM_TYPE && s->decl_node &&
                s->decl_node->kind == IRON_NODE_OBJECT_DECL)
                tn = ((Iron_ObjectDecl *)s->decl_node)->name;
        }
        if (tn) queue_methods(w, tn, mc->method);
        return true;
    }
    return true;
}

/* Which globals `init` reads, directly or through the functions, methods
 * and initializers it calls (statically known calls only). */
static void global_deps(GoWalk *w, Iron_Node *init, int n) {
    memset(w->deps, 0, sizeof(bool) * (size_t)n);
    hmfree(w->seen);
    w->seen = NULL;
    arrsetlen(w->pending, 0);
    Iron_Visitor v = { .ctx = w, .visit_node = go_visit, .post_visit = NULL };
    iron_ast_walk(init, &v);
    while (arrlen(w->pending) > 0) {
        Iron_Node *d = arrpop(w->pending);
        Iron_Node *body = NULL;
        if (d->kind == IRON_NODE_FUNC_DECL) body = ((Iron_FuncDecl *)d)->body;
        else if (d->kind == IRON_NODE_METHOD_DECL) body = ((Iron_MethodDecl *)d)->body;
        if (body) iron_ast_walk(body, &v);
    }
}

int iron_global_init_order(Iron_Program *program, Iron_Arena *arena, Iron_DiagList *diags,
                           Iron_Node ***out_order) {
    if (out_order) *out_order = NULL;
    if (!program) return 0;
    GoWalk w;
    memset(&w, 0, sizeof(w));
    w.program = program;
    Iron_Node **globals = NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!is_global_decl(d) || hmgeti(w.global_index, d) >= 0) continue;
        hmput(w.global_index, d, (int)arrlen(globals));
        arrput(globals, d);
    }
    int n = (int)arrlen(globals);
    int cycles = 0;
    if (n > 0) {
        bool *dep = (bool *)calloc((size_t)n * (size_t)n, sizeof(bool));
        w.deps = (bool *)calloc((size_t)n, sizeof(bool));
        int *state = (int *)calloc((size_t)n, sizeof(int));   /* 0 new, 1 open, 2 done */
        int *stack = (int *)calloc((size_t)n, sizeof(int));
        int *next = (int *)calloc((size_t)n, sizeof(int));
        bool *reported = (bool *)calloc((size_t)n, sizeof(bool));
        if (!dep || !w.deps || !state || !stack || !next || !reported) {
            free(dep); free(w.deps); free(state); free(stack); free(next); free(reported);
            hmfree(w.global_index); hmfree(w.seen); arrfree(w.pending); arrfree(globals);
            return 0;
        }
        for (int g = 0; g < n; g++) {
            Iron_Node *init = global_init(globals[g]);
            if (!init) continue;
            global_deps(&w, init, n);
            memcpy(&dep[(size_t)g * (size_t)n], w.deps, sizeof(bool) * (size_t)n);
        }
        /* Depth-first, in declaration order: a global comes after the
         * globals it depends on; otherwise declaration order is kept. */
        Iron_Node **order = NULL;
        for (int root = 0; root < n; root++) {
            if (state[root]) continue;
            int sp = 0;
            stack[sp++] = root;
            state[root] = 1;
            next[root] = 0;
            while (sp > 0) {
                int g = stack[sp - 1];
                int d = next[g];
                while (d < n && !dep[(size_t)g * (size_t)n + (size_t)d]) d++;
                if (d < n) {
                    next[g] = d + 1;
                    if (state[d] == 0) {
                        state[d] = 1;
                        next[d] = 0;
                        stack[sp++] = d;
                    } else if (state[d] == 1 && !reported[d]) {
                        /* d is open on the stack: d .. g .. d is a cycle. */
                        reported[d] = true;
                        cycles++;
                        if (diags && arena) {
                            char msg[512];
                            if (d == g)
                                snprintf(msg, sizeof(msg),
                                         "global '%s' is read while it is being initialized",
                                         global_name(globals[d]));
                            else
                                snprintf(msg, sizeof(msg),
                                         "initialization cycle: global '%s' depends on '%s', "
                                         "which depends on '%s'",
                                         global_name(globals[d]), global_name(globals[g]),
                                         global_name(globals[d]));
                            const char *m = iron_arena_strdup(arena, msg, strlen(msg));
                            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                                           IRON_ERR_GLOBAL_INIT_CYCLE, globals[d]->span,
                                           m ? m : "initialization cycle",
                                           "a global may not depend on itself through the "
                                           "functions its initializer calls");
                        }
                    }
                } else {
                    state[g] = 2;
                    arrput(order, globals[g]);
                    sp--;
                }
            }
        }
        if (out_order) *out_order = order;
        else arrfree(order);
        free(dep); free(w.deps); free(state); free(stack); free(next); free(reported);
    }
    hmfree(w.global_index);
    hmfree(w.seen);
    arrfree(w.pending);
    arrfree(globals);
    return cycles;
}
