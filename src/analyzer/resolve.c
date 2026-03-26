/* resolve.c — Two-pass name resolution for Iron.
 *
 * Pass 1a: Collect top-level declarations into the global scope.
 * Pass 1b: Attach method declarations to their owning types.
 * Pass 2:  Walk the full AST resolving all Iron_Ident nodes.
 *
 * Uses direct switch dispatch (not iron_ast_walk visitor) so we have
 * full control over scope push/pop ordering.
 */

#include "analyzer/resolve.h"
#include "vendor/stb_ds.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>

/* ── Resolver context ────────────────────────────────────────────────────── */

typedef struct {
    Iron_Arena    *arena;
    Iron_DiagList *diags;
    Iron_Scope    *global_scope;
    Iron_Scope    *current_scope;
    /* Set when inside a method body. NULL if in a regular function. */
    Iron_MethodDecl *current_method;
    /* The object type_name for the current method. NULL outside methods. */
    const char      *current_type_name;
} ResolveCtx;

/* ── Forward declarations ────────────────────────────────────────────────── */

static void resolve_node(ResolveCtx *ctx, Iron_Node *node);
static void resolve_expr(ResolveCtx *ctx, Iron_Node *node);

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void push_scope(ResolveCtx *ctx, Iron_ScopeKind kind) {
    ctx->current_scope = iron_scope_create(ctx->arena, ctx->current_scope, kind);
}

static void pop_scope(ResolveCtx *ctx) {
    if (ctx->current_scope->parent) {
        ctx->current_scope = ctx->current_scope->parent;
    }
}

static void define_sym(ResolveCtx *ctx, const char *name, Iron_SymbolKind kind,
                        Iron_Node *decl_node, Iron_Span span, bool is_mutable,
                        bool is_private) {
    Iron_Symbol *sym = iron_symbol_create(ctx->arena, name, kind, decl_node, span);
    sym->is_mutable  = is_mutable;
    sym->is_private  = is_private;
    if (!iron_scope_define(ctx->current_scope, ctx->arena, sym)) {
        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                       IRON_ERR_DUPLICATE_DECL, span,
                       "duplicate declaration", NULL);
    }
}

/* Emit an "undefined variable" diagnostic for an unresolved identifier. */
static void emit_undefined(ResolveCtx *ctx, const char *name, Iron_Span span) {
    char msg[256];
    snprintf(msg, sizeof(msg), "undefined identifier '%s'", name);
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                   IRON_ERR_UNDEFINED_VAR, span,
                   iron_arena_strdup(ctx->arena, msg, strlen(msg)), NULL);
}

/* ── Pass 1a: Collect top-level declarations ─────────────────────────────── */

static void collect_decl(ResolveCtx *ctx, Iron_Node *node) {
    switch (node->kind) {
        case IRON_NODE_OBJECT_DECL: {
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)node;
            /* Create an object type and attach to symbol */
            Iron_Type *ty = iron_type_make_object(ctx->arena, od);
            Iron_Symbol *sym = iron_symbol_create(ctx->arena, od->name,
                                                   IRON_SYM_TYPE,
                                                   node, od->span);
            sym->type = ty;
            if (!iron_scope_define(ctx->global_scope, ctx->arena, sym)) {
                iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                               IRON_ERR_DUPLICATE_DECL, od->span,
                               "duplicate type declaration", NULL);
            }
            break;
        }
        case IRON_NODE_INTERFACE_DECL: {
            Iron_InterfaceDecl *id = (Iron_InterfaceDecl *)node;
            Iron_Type *ty = iron_type_make_interface(ctx->arena, id);
            Iron_Symbol *sym = iron_symbol_create(ctx->arena, id->name,
                                                   IRON_SYM_INTERFACE,
                                                   node, id->span);
            sym->type = ty;
            if (!iron_scope_define(ctx->global_scope, ctx->arena, sym)) {
                iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                               IRON_ERR_DUPLICATE_DECL, id->span,
                               "duplicate interface declaration", NULL);
            }
            break;
        }
        case IRON_NODE_ENUM_DECL: {
            Iron_EnumDecl *ed = (Iron_EnumDecl *)node;
            Iron_Type *ty = iron_type_make_enum(ctx->arena, ed);
            Iron_Symbol *enum_sym = iron_symbol_create(ctx->arena, ed->name,
                                                       IRON_SYM_ENUM,
                                                       node, ed->span);
            enum_sym->type = ty;
            if (!iron_scope_define(ctx->global_scope, ctx->arena, enum_sym)) {
                iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                               IRON_ERR_DUPLICATE_DECL, ed->span,
                               "duplicate enum declaration", NULL);
            }
            /* Register each variant as SYM_ENUM_VARIANT in global scope */
            for (int i = 0; i < ed->variant_count; i++) {
                Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[i];
                Iron_Symbol *vsym = iron_symbol_create(ctx->arena, ev->name,
                                                        IRON_SYM_ENUM_VARIANT,
                                                        ed->variants[i], ev->span);
                vsym->type = ty;
                /* Silently skip duplicate variant names — a separate check can
                 * catch this later. For now just attempt to define. */
                iron_scope_define(ctx->global_scope, ctx->arena, vsym);
            }
            break;
        }
        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)node;
            Iron_Symbol *sym = iron_symbol_create(ctx->arena, fd->name,
                                                   IRON_SYM_FUNCTION,
                                                   node, fd->span);
            sym->is_private = fd->is_private;
            if (!iron_scope_define(ctx->global_scope, ctx->arena, sym)) {
                iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                               IRON_ERR_DUPLICATE_DECL, fd->span,
                               "duplicate function declaration", NULL);
            }
            break;
        }
        case IRON_NODE_IMPORT_DECL: {
            Iron_ImportDecl *imp = (Iron_ImportDecl *)node;
            /* If there's an alias, define it in the global scope as a placeholder.
             * Actual module resolution happens in Phase 3 CLI. */
            if (imp->alias) {
                Iron_Symbol *sym = iron_symbol_create(ctx->arena, imp->alias,
                                                       IRON_SYM_TYPE,
                                                       node, imp->span);
                /* Tolerate duplicate alias silently — import ordering issues
                 * will be caught by the module resolver later. */
                iron_scope_define(ctx->global_scope, ctx->arena, sym);
            }
            break;
        }
        default:
            /* Method decls handled in pass 1b; everything else ignored here */
            break;
    }
}

/* ── Pass 1b: Attach method declarations to their owning types ───────────── */

static void attach_method(ResolveCtx *ctx, Iron_Node *node) {
    if (node->kind != IRON_NODE_METHOD_DECL) return;

    Iron_MethodDecl *md = (Iron_MethodDecl *)node;
    Iron_Symbol *owner = iron_scope_lookup(ctx->global_scope, md->type_name);
    if (!owner) {
        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNDEFINED_VAR, md->span,
                       "method declared on undeclared type", NULL);
        return;
    }
    if (owner->sym_kind != IRON_SYM_TYPE) {
        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNDEFINED_VAR, md->span,
                       "method declared on non-object type", NULL);
        return;
    }
    md->owner_sym = owner;
}

/* ── Resolve statements and expressions ──────────────────────────────────── */

static void resolve_node_list(ResolveCtx *ctx, Iron_Node **nodes, int count) {
    for (int i = 0; i < count; i++) {
        if (nodes[i]) resolve_node(ctx, nodes[i]);
    }
}

static void resolve_expr(ResolveCtx *ctx, Iron_Node *node) {
    if (!node) return;
    resolve_node(ctx, node);
}

static void resolve_node(ResolveCtx *ctx, Iron_Node *node) {
    if (!node) return;

    switch (node->kind) {
        /* ── Ident resolution ─────────────────────────────────────────────── */
        case IRON_NODE_IDENT: {
            Iron_Ident *id = (Iron_Ident *)node;

            /* Special: self */
            if (strcmp(id->name, "self") == 0) {
                if (!ctx->current_method) {
                    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                   IRON_ERR_SELF_OUTSIDE_METHOD, id->span,
                                   "'self' used outside of method", NULL);
                    return;
                }
                /* self is defined in the method scope — just look it up */
                Iron_Symbol *sym = iron_scope_lookup(ctx->current_scope, "self");
                if (sym) id->resolved_sym = sym;
                return;
            }

            /* Special: super */
            if (strcmp(id->name, "super") == 0) {
                if (!ctx->current_method) {
                    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                   IRON_ERR_SELF_OUTSIDE_METHOD, id->span,
                                   "'super' used outside of method", NULL);
                    return;
                }
                /* Check if owning type has extends_name */
                if (ctx->current_method->owner_sym &&
                    ctx->current_method->owner_sym->decl_node) {
                    Iron_ObjectDecl *od =
                        (Iron_ObjectDecl *)ctx->current_method->owner_sym->decl_node;
                    if (!od->extends_name) {
                        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                       IRON_ERR_SUPER_NO_PARENT, id->span,
                                       "'super' used in type that does not extend another type",
                                       NULL);
                        return;
                    }
                } else {
                    /* owner_sym not set means method had unresolved type — already
                     * reported; silently skip here */
                    return;
                }
                /* Look up the parent type symbol */
                Iron_ObjectDecl *od =
                    (Iron_ObjectDecl *)ctx->current_method->owner_sym->decl_node;
                Iron_Symbol *parent_sym =
                    iron_scope_lookup(ctx->global_scope, od->extends_name);
                id->resolved_sym = parent_sym; /* may be NULL; caller checks */
                return;
            }

            /* Normal identifier */
            Iron_Symbol *sym = iron_scope_lookup(ctx->current_scope, id->name);
            if (sym) {
                id->resolved_sym = sym;
            } else {
                emit_undefined(ctx, id->name, id->span);
            }
            break;
        }

        /* ── Declarations ─────────────────────────────────────────────────── */

        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)node;
            push_scope(ctx, IRON_SCOPE_FUNCTION);
            ctx->current_scope->owner_name = fd->name;

            /* Define params */
            for (int i = 0; i < fd->param_count; i++) {
                Iron_Param *p = (Iron_Param *)fd->params[i];
                Iron_Symbol *ps = iron_symbol_create(ctx->arena, p->name,
                                                      IRON_SYM_PARAM,
                                                      fd->params[i], p->span);
                ps->is_mutable = p->is_var;
                iron_scope_define(ctx->current_scope, ctx->arena, ps);
            }

            /* Resolve body */
            if (fd->body) resolve_node(ctx, fd->body);

            pop_scope(ctx);
            break;
        }

        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *md = (Iron_MethodDecl *)node;

            /* Save previous method context */
            Iron_MethodDecl *prev_method = ctx->current_method;
            const char      *prev_type   = ctx->current_type_name;

            ctx->current_method    = md;
            ctx->current_type_name = md->type_name;

            push_scope(ctx, IRON_SCOPE_FUNCTION);
            ctx->current_scope->owner_name = md->method_name;

            /* Define implicit 'self' */
            if (md->owner_sym) {
                Iron_Symbol *self_sym = iron_symbol_create(ctx->arena, "self",
                                                            IRON_SYM_VARIABLE,
                                                            node, md->span);
                self_sym->is_mutable = true;
                self_sym->type = md->owner_sym->type;
                iron_scope_define(ctx->current_scope, ctx->arena, self_sym);
            }

            /* Define params */
            for (int i = 0; i < md->param_count; i++) {
                Iron_Param *p = (Iron_Param *)md->params[i];
                Iron_Symbol *ps = iron_symbol_create(ctx->arena, p->name,
                                                      IRON_SYM_PARAM,
                                                      md->params[i], p->span);
                ps->is_mutable = p->is_var;
                iron_scope_define(ctx->current_scope, ctx->arena, ps);
            }

            /* Resolve body */
            if (md->body) resolve_node(ctx, md->body);

            pop_scope(ctx);

            ctx->current_method    = prev_method;
            ctx->current_type_name = prev_type;
            break;
        }

        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            /* Resolve init first, then define (prevents self-referencing) */
            if (vd->init) resolve_expr(ctx, vd->init);
            define_sym(ctx, vd->name, IRON_SYM_VARIABLE, node, vd->span,
                       /*is_mutable=*/false, /*is_private=*/false);
            break;
        }

        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            if (vd->init) resolve_expr(ctx, vd->init);
            define_sym(ctx, vd->name, IRON_SYM_VARIABLE, node, vd->span,
                       /*is_mutable=*/true, /*is_private=*/false);
            break;
        }

        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)node;
            push_scope(ctx, IRON_SCOPE_BLOCK);
            for (int i = 0; i < b->stmt_count; i++) {
                if (b->stmts[i]) resolve_node(ctx, b->stmts[i]);
            }
            pop_scope(ctx);
            break;
        }

        /* ── Statements ───────────────────────────────────────────────────── */

        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            resolve_expr(ctx, as->target);
            resolve_expr(ctx, as->value);
            break;
        }

        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            if (rs->value) resolve_expr(ctx, rs->value);
            break;
        }

        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            resolve_expr(ctx, is->condition);
            if (is->body) resolve_node(ctx, is->body);
            /* elif branches */
            for (int i = 0; i < is->elif_count; i++) {
                resolve_expr(ctx, is->elif_conds[i]);
                if (is->elif_bodies[i]) resolve_node(ctx, is->elif_bodies[i]);
            }
            if (is->else_body) resolve_node(ctx, is->else_body);
            break;
        }

        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            resolve_expr(ctx, ws->condition);
            if (ws->body) resolve_node(ctx, ws->body);
            break;
        }

        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            /* Resolve iterable in current scope (not the for's inner scope) */
            resolve_expr(ctx, fs->iterable);
            push_scope(ctx, IRON_SCOPE_BLOCK);
            /* Define loop variable in the inner scope */
            Iron_Span var_span = fs->span; /* use for-stmt span for the var */
            define_sym(ctx, fs->var_name, IRON_SYM_VARIABLE, node, var_span,
                       /*is_mutable=*/true, /*is_private=*/false);
            if (fs->body) resolve_node(ctx, fs->body);
            pop_scope(ctx);
            break;
        }

        case IRON_NODE_MATCH: {
            Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
            resolve_expr(ctx, ms->subject);
            for (int i = 0; i < ms->case_count; i++) {
                if (ms->cases[i]) resolve_node(ctx, ms->cases[i]);
            }
            if (ms->else_body) resolve_node(ctx, ms->else_body);
            break;
        }

        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *mc = (Iron_MatchCase *)node;
            if (mc->pattern) resolve_expr(ctx, mc->pattern);
            if (mc->body) resolve_node(ctx, mc->body);
            break;
        }

        case IRON_NODE_DEFER: {
            Iron_DeferStmt *ds = (Iron_DeferStmt *)node;
            resolve_expr(ctx, ds->expr);
            break;
        }

        case IRON_NODE_FREE: {
            Iron_FreeStmt *fs = (Iron_FreeStmt *)node;
            resolve_expr(ctx, fs->expr);
            break;
        }

        case IRON_NODE_LEAK: {
            Iron_LeakStmt *ls = (Iron_LeakStmt *)node;
            resolve_expr(ctx, ls->expr);
            break;
        }

        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)node;
            if (ss->pool_expr) resolve_expr(ctx, ss->pool_expr);
            if (ss->body) resolve_node(ctx, ss->body);
            break;
        }

        /* ── Expressions ──────────────────────────────────────────────────── */

        case IRON_NODE_INT_LIT:
        case IRON_NODE_FLOAT_LIT:
        case IRON_NODE_STRING_LIT:
        case IRON_NODE_BOOL_LIT:
        case IRON_NODE_NULL_LIT:
            /* Literals have no identifiers to resolve */
            break;

        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *is = (Iron_InterpString *)node;
            resolve_node_list(ctx, is->parts, is->part_count);
            break;
        }

        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *be = (Iron_BinaryExpr *)node;
            resolve_expr(ctx, be->left);
            resolve_expr(ctx, be->right);
            break;
        }

        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *ue = (Iron_UnaryExpr *)node;
            resolve_expr(ctx, ue->operand);
            break;
        }

        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)node;
            resolve_expr(ctx, ce->callee);
            resolve_node_list(ctx, ce->args, ce->arg_count);
            break;
        }

        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            resolve_expr(ctx, mc->object);
            resolve_node_list(ctx, mc->args, mc->arg_count);
            break;
        }

        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
            resolve_expr(ctx, fa->object);
            break;
        }

        case IRON_NODE_INDEX: {
            Iron_IndexExpr *ie = (Iron_IndexExpr *)node;
            resolve_expr(ctx, ie->object);
            resolve_expr(ctx, ie->index);
            break;
        }

        case IRON_NODE_SLICE: {
            Iron_SliceExpr *se = (Iron_SliceExpr *)node;
            resolve_expr(ctx, se->object);
            resolve_expr(ctx, se->start);
            resolve_expr(ctx, se->end);
            break;
        }

        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *le = (Iron_LambdaExpr *)node;
            push_scope(ctx, IRON_SCOPE_FUNCTION);
            for (int i = 0; i < le->param_count; i++) {
                Iron_Param *p = (Iron_Param *)le->params[i];
                Iron_Symbol *ps = iron_symbol_create(ctx->arena, p->name,
                                                      IRON_SYM_PARAM,
                                                      le->params[i], p->span);
                ps->is_mutable = p->is_var;
                iron_scope_define(ctx->current_scope, ctx->arena, ps);
            }
            if (le->body) resolve_node(ctx, le->body);
            pop_scope(ctx);
            break;
        }

        case IRON_NODE_HEAP: {
            Iron_HeapExpr *he = (Iron_HeapExpr *)node;
            resolve_expr(ctx, he->inner);
            break;
        }

        case IRON_NODE_RC: {
            Iron_RcExpr *re = (Iron_RcExpr *)node;
            resolve_expr(ctx, re->inner);
            break;
        }

        case IRON_NODE_COMPTIME: {
            Iron_ComptimeExpr *ce = (Iron_ComptimeExpr *)node;
            resolve_expr(ctx, ce->inner);
            break;
        }

        case IRON_NODE_IS: {
            Iron_IsExpr *ie = (Iron_IsExpr *)node;
            resolve_expr(ctx, ie->expr);
            break;
        }

        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *ae = (Iron_AwaitExpr *)node;
            resolve_expr(ctx, ae->handle);
            break;
        }

        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;
            /* Resolve type_name as an identifier lookup */
            Iron_Symbol *type_sym = iron_scope_lookup(ctx->current_scope, ce->type_name);
            if (!type_sym) {
                emit_undefined(ctx, ce->type_name, ce->span);
            }
            resolve_node_list(ctx, ce->args, ce->arg_count);
            break;
        }

        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)node;
            if (al->size) resolve_expr(ctx, al->size);
            resolve_node_list(ctx, al->elements, al->element_count);
            break;
        }

        /* ── Top-level declarations (no inner resolution needed here) ──────── */
        case IRON_NODE_OBJECT_DECL:
        case IRON_NODE_INTERFACE_DECL:
        case IRON_NODE_ENUM_DECL:
        case IRON_NODE_IMPORT_DECL:
            /* Already handled in Pass 1; nothing to walk for resolution */
            break;

        /* ── Structural helpers ───────────────────────────────────────────── */
        case IRON_NODE_PARAM:
        case IRON_NODE_FIELD:
        case IRON_NODE_TYPE_ANNOTATION:
        case IRON_NODE_ENUM_VARIANT:
            /* No identifier resolution needed inside these */
            break;

        case IRON_NODE_ERROR:
        case IRON_NODE_PROGRAM:
            /* Should not appear in isolation; handled at entry point */
            break;

        default:
            break;
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

Iron_Scope *iron_resolve(Iron_Program *program, Iron_Arena *arena,
                          Iron_DiagList *diags) {
    if (!program) return NULL;

    ResolveCtx ctx;
    ctx.arena              = arena;
    ctx.diags              = diags;
    ctx.global_scope       = iron_scope_create(arena, NULL, IRON_SCOPE_GLOBAL);
    ctx.current_scope      = ctx.global_scope;
    ctx.current_method     = NULL;
    ctx.current_type_name  = NULL;

    /* Initialize type system */
    iron_types_init(arena);

    /* Register built-in functions in global scope.
     * print/println map to printf in codegen; register as func(String)->Void
     * so the resolver and type-checker accept them without errors.
     * Using a single-String-param signature works for all current test fixtures. */
    {
        Iron_Type *str_t  = iron_type_make_primitive(IRON_TYPE_STRING);
        Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);
        Iron_Type *params[1];
        params[0] = str_t;
        Iron_Type *println_type = iron_type_make_func(arena, params, 1, void_t);

        static const char *print_builtins[] = { "print", "println" };
        for (int bi = 0; bi < 2; bi++) {
            Iron_Span no_span = {0};
            Iron_Symbol *bsym = iron_symbol_create(arena, print_builtins[bi],
                                                    IRON_SYM_FUNCTION,
                                                    NULL, no_span);
            bsym->type = println_type;
            iron_scope_define(ctx.global_scope, arena, bsym);
        }
    }

    /* Register remaining builtins: len(String)->Int, min/max(Int,Int)->Int,
     * clamp(Int,Int,Int)->Int, abs(Int)->Int, assert(Bool)->Void.
     * These are handled by the codegen but must be in scope so the resolver
     * and type-checker accept call sites without emitting undefined-identifier
     * errors.  Signatures use Int/String for simplicity; the type-checker
     * only verifies that a matching IRON_SYM_FUNCTION symbol exists. */
    {
        Iron_Span no_span = {0};
        Iron_Type *int_t   = iron_type_make_primitive(IRON_TYPE_INT);
        Iron_Type *str_t   = iron_type_make_primitive(IRON_TYPE_STRING);
        Iron_Type *bool_t  = iron_type_make_primitive(IRON_TYPE_BOOL);
        Iron_Type *void_t  = iron_type_make_primitive(IRON_TYPE_VOID);

        /* len(String) -> Int */
        {
            Iron_Type *params[1] = { str_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 1, int_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "len",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            sym->type = fn;
            iron_scope_define(ctx.global_scope, arena, sym);
        }
        /* min(Int, Int) -> Int */
        {
            Iron_Type *params[2] = { int_t, int_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 2, int_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "min",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            sym->type = fn;
            iron_scope_define(ctx.global_scope, arena, sym);
        }
        /* max(Int, Int) -> Int */
        {
            Iron_Type *params[2] = { int_t, int_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 2, int_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "max",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            sym->type = fn;
            iron_scope_define(ctx.global_scope, arena, sym);
        }
        /* clamp(Int, Int, Int) -> Int */
        {
            Iron_Type *params[3] = { int_t, int_t, int_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 3, int_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "clamp",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            sym->type = fn;
            iron_scope_define(ctx.global_scope, arena, sym);
        }
        /* abs(Int) -> Int */
        {
            Iron_Type *params[1] = { int_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 1, int_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "abs",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            sym->type = fn;
            iron_scope_define(ctx.global_scope, arena, sym);
        }
        /* assert(Bool) -> Void */
        {
            Iron_Type *params[1] = { bool_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 1, void_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "assert",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            sym->type = fn;
            iron_scope_define(ctx.global_scope, arena, sym);
        }
    }

    /* Pass 1a: Collect all top-level declarations into global scope */
    for (int i = 0; i < program->decl_count; i++) {
        if (program->decls[i]) {
            collect_decl(&ctx, program->decls[i]);
        }
    }

    /* Pass 1b: Attach methods to their owning types */
    for (int i = 0; i < program->decl_count; i++) {
        if (program->decls[i]) {
            attach_method(&ctx, program->decls[i]);
        }
    }

    /* Pass 2: Resolve identifiers in all declaration bodies */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;

        switch (decl->kind) {
            case IRON_NODE_FUNC_DECL:
                resolve_node(&ctx, decl);
                break;
            case IRON_NODE_METHOD_DECL:
                resolve_node(&ctx, decl);
                break;
            case IRON_NODE_OBJECT_DECL:
            case IRON_NODE_INTERFACE_DECL:
            case IRON_NODE_ENUM_DECL:
            case IRON_NODE_IMPORT_DECL:
                /* Bodies of these are handled during type checking passes */
                break;
            default:
                resolve_node(&ctx, decl);
                break;
        }
    }

    return ctx.global_scope;
}
