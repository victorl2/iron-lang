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
#include "analyzer/typo_candidate.h"
#include "parser/ast.h"
#include "vendor/stb_ds.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>

/* ── Cancellation helper (HARD-05) ─────────────────────────────────────────── */
static inline bool iron_cancel_requested(const _Atomic bool *flag) {
    return flag != NULL && atomic_load_explicit(flag, memory_order_relaxed);
}

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
    /* HARD-05: cooperative cancellation flag (NULL means never cancel). */
    const _Atomic bool *cancel_flag;
    /* HARD-09 CR-03: counter of push_scope calls that failed (arena OOM)
     * and silently aliased to the current scope. pop_scope must skip the
     * same number of pops so the scope stack stays balanced. */
    int skipped_scope_pushes;
    /* HARD-09 CR-03: set true when an OOM path was taken so diagnostics
     * can be suppressed cascades-style without aborting. Mirrors the
     * parser's p->in_error_recovery flag. */
    bool in_error_recovery;

    /* Rate-limit the audit-hint note attached to E0320. The note
     * ("audit your public surface with: grep -n '^pub '") is useful once
     * per build but spammy on subsequent occurrences. */
    bool emitted_first_e0320;

    /* Stdlib carve-out: line number where the user's source begins. Decls
     * whose span.line is below this value are stdlib (prepended via
     * build.c / check.c pipelines) and are treated as implicitly pub. Set
     * to 0 when no stdlib was prepended (no carve-out active). Threaded
     * from Iron_Parser.user_source_start_line via Iron_Program at the
     * resolver entry point. */
    int user_source_start_line;
} ResolveCtx;

/* ── Forward declarations ────────────────────────────────────────────────── */

static void resolve_node(ResolveCtx *ctx, Iron_Node *node);
static void resolve_expr(ResolveCtx *ctx, Iron_Node *node);

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void push_scope(ResolveCtx *ctx, Iron_ScopeKind kind) {
    /* HARD-09 CR-03: iron_scope_create may return NULL on arena OOM. Fall
     * back to the current scope (alias it as the "new" scope) and record
     * the skipped push so the matching pop_scope does not unbalance the
     * stack. The degraded path causes duplicate-decl diagnostics where a
     * nested scope would have shadowed, but avoids aborting compilation. */
    Iron_Scope *ns = iron_scope_create(ctx->arena, ctx->current_scope, kind);
    if (ns) {
        ctx->current_scope = ns;
    } else {
        ctx->skipped_scope_pushes++;
        ctx->in_error_recovery = true;
    }
}

static void pop_scope(ResolveCtx *ctx) {
    /* HARD-09 CR-03: consume skipped-push counter first so the matching
     * pops for failed pushes are no-ops. */
    if (ctx->skipped_scope_pushes > 0) {
        ctx->skipped_scope_pushes--;
        return;
    }
    if (ctx->current_scope && ctx->current_scope->parent) {
        ctx->current_scope = ctx->current_scope->parent;
    }
}

static void define_sym(ResolveCtx *ctx, const char *name, Iron_SymbolKind kind,
                        Iron_Node *decl_node, Iron_Span span, bool is_mutable,
                        bool is_private) {
    Iron_Symbol *sym = iron_symbol_create(ctx->arena, name, kind, decl_node, span);
    /* HARD-09 CR-03: iron_symbol_create may return NULL on arena OOM.
     * Skip insertion; subsequent lookups emit "undefined identifier" which
     * is a safe, non-crashing degraded mode. */
    if (!sym) { ctx->in_error_recovery = true; return; }
    sym->is_mutable  = is_mutable;
    sym->is_private  = is_private;
    if (!iron_scope_define(ctx->current_scope, ctx->arena, sym)) {
        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                       IRON_ERR_DUPLICATE_DECL, span,
                       "duplicate declaration", NULL);
    }
}

/* Emit an "undefined variable" diagnostic for an unresolved identifier.
 * Phase 4 Plan 04-01 (EDIT-07): enrich with .suggestion = best Levenshtein
 * candidate from the visible scope chain (max distance 2). */
static void emit_undefined(ResolveCtx *ctx, const char *name, Iron_Span span) {
    char msg[256];
    snprintf(msg, sizeof(msg), "undefined identifier '%s'", name);
    const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
    if (!msg_copy) { /* HARD-09 REPLACE (resolve.c:emit_undefined msg) */ msg_copy = "analyzer error"; }
    const char *suggestion = iron_best_typo_candidate(ctx->current_scope,
                                                       ctx->arena, name);
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                   IRON_ERR_UNDEFINED_VAR, span, msg_copy, suggestion);
}

/* Phase 93 VIS-03: classify a symbol as stdlib via the line-offset carve-out.
 * Stdlib decls are prepended to the user source by build.c / check.c; their
 * AST span.line is below ctx->user_source_start_line. Returns true when the
 * symbol's declaring line is in the stdlib prefix. When the carve-out is
 * inactive (user_source_start_line <= 0) every decl tests false so the gate
 * still works on synthetic cross-module test inputs (e.g. unit tests). */
static bool is_stdlib_decl(ResolveCtx *ctx, Iron_Symbol *sym) {
    if (ctx->user_source_start_line <= 0) return false;
    return sym->span.line > 0 &&
           sym->span.line < (uint32_t)ctx->user_source_start_line;
}

/* Phase 93 VIS-03: emit E0320 for a cross-module reference to a non-pub
 * symbol. Includes the declaring file, the use-site file, and (on the first
 * emission per build only) a help+note pair recommending `grep -n '^pub '`
 * to audit the public surface. The note is rate-limited so multi-error
 * compiles stay readable. */
static void emit_cross_module_private(ResolveCtx *ctx,
                                      Iron_Symbol *sym,
                                      Iron_Ident  *id) {
    char msg[512];
    snprintf(msg, sizeof(msg),
             "private symbol '%s' (declared in %s) is not visible from %s",
             id->name,
             sym->span.filename ? sym->span.filename : "<unknown>",
             id->span.filename  ? id->span.filename  : "<unknown>");
    const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
    if (!msg_copy) iron_oom_abort("resolve.c:emit_cross_module_private msg");

    const char *hint;
    if (!ctx->emitted_first_e0320) {
        hint = "add `pub` to the declaration to expose it across module boundaries\n"
               "note: audit your public surface with: grep -n '^pub ' src/*.iron";
    } else {
        hint = "add `pub` to the declaration to expose it across module boundaries";
    }

    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                   IRON_ERR_CROSS_MODULE_PRIVATE, id->span, msg_copy, hint);
    ctx->emitted_first_e0320 = true;
}

/* ── Pass 1a: Collect top-level declarations ─────────────────────────────── */

static void collect_decl(ResolveCtx *ctx, Iron_Node *node) {
    switch ((int)(node->kind)) {
        case IRON_NODE_OBJECT_DECL: {
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)node;
            /* Phase 86 PATCH-02: patches are NOT type declarations. An
             * is_patch=true ObjectDecl contributes methods/inits to an
             * existing type; treating it as a new type would collide on
             * the real target's name and shadow the original. The patch
             * registry (iron_type_patch_registry_build) keys on
             * target_type_name, so no global symbol is needed here. */
            if (od->is_patch) break;
            /* Create an object type and attach to symbol */
            Iron_Type *ty = iron_type_make_object(ctx->arena, od);
            Iron_Symbol *sym = iron_symbol_create(ctx->arena, od->name,
                                                   IRON_SYM_TYPE,
                                                   node, od->span);
            /* HARD-09 CR-03: skip this decl on arena OOM. */
            if (!sym) { ctx->in_error_recovery = true; break; }
            sym->type = ty;
            /* Phase 93 VIS-02/03: propagate the AST is_pub bit. */
            sym->is_pub = od->is_pub;
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
            /* HARD-09 CR-03: skip this decl on arena OOM. */
            if (!sym) { ctx->in_error_recovery = true; break; }
            sym->type = ty;
            /* Phase 93 RESEARCH Open Question 3: `pub interface` is deferred.
             * Interfaces are always treated as not-pub at the cross-module
             * gate; the gate predicate excludes IRON_SYM_INTERFACE anyway,
             * so this is documentation only. */
            sym->is_pub = false;
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
            /* HARD-09 CR-03: skip this decl on arena OOM. */
            if (!enum_sym) { ctx->in_error_recovery = true; break; }
            enum_sym->type = ty;
            /* Phase 93 VIS-02/03: propagate is_pub onto the enum symbol. */
            enum_sym->is_pub = ed->is_pub;
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
                /* HARD-09 CR-03: skip this variant on arena OOM. */
                if (!vsym) { ctx->in_error_recovery = true; continue; }
                vsym->type = ty;
                /* Phase 93 VIS-02/03: variants inherit the enclosing enum's
                 * pub bit. This matches the locked decision in CONTEXT.md
                 * (no per-variant visibility). */
                vsym->is_pub = ed->is_pub;
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
            /* HARD-09 CR-03: skip this decl on arena OOM. */
            if (!sym) { ctx->in_error_recovery = true; break; }
            sym->is_private    = fd->is_private;
            /* Phase 93 VIS-02/03: propagate the AST is_pub bit. */
            sym->is_pub        = fd->is_pub;
            sym->is_extern     = fd->is_extern;
            sym->extern_c_name = fd->extern_c_name;
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
                /* HARD-09 CR-03: skip this alias on arena OOM. */
                if (!sym) { ctx->in_error_recovery = true; break; }
                /* Tolerate duplicate alias silently — import ordering issues
                 * will be caught by the module resolver later. */
                iron_scope_define(ctx->global_scope, ctx->arena, sym);
            }
            break;
        }
        /* -Wswitch-enum opt-out: collect_decl only processes top-level
         * type/func/import declarations; method decls are handled in Pass 1b
         * and every other Iron_NodeKind (expressions, statements, literals)
         * is intentionally ignored at the top level. */
        default:
            /* Method decls handled in pass 1b; everything else ignored here */
            break;
    }
}

/* ── Pass 1b: Attach method declarations to their owning types ───────────── */

static void attach_method(ResolveCtx *ctx, Iron_Node *node) {
    if (node->kind != IRON_NODE_METHOD_DECL) return;

    Iron_MethodDecl *md = (Iron_MethodDecl *)node;

    /* Array extension methods (func [T].map(...)) have no owning type in scope */
    if (md->is_array_extension) return;

    Iron_Symbol *owner = iron_scope_lookup(ctx->global_scope, md->type_name);
    if (!owner) {
        /* Phase 4 Plan 04-01 (EDIT-07): seed .suggestion with typo candidate. */
        const char *sug = iron_best_typo_candidate(ctx->global_scope,
                                                    ctx->arena, md->type_name);
        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNDEFINED_VAR, md->span,
                       "method declared on undeclared type", sug);
        return;
    }
    if (owner->sym_kind != IRON_SYM_TYPE && owner->sym_kind != IRON_SYM_ENUM) {
        /* Phase 4 Plan 04-01 (EDIT-07): seed .suggestion with typo candidate. */
        const char *sug = iron_best_typo_candidate(ctx->global_scope,
                                                    ctx->arena, md->type_name);
        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNDEFINED_VAR, md->span,
                       "method declared on non-object type", sug);
        return;
    }
    md->owner_sym = owner;
}

/* ── Resolve statements and expressions ──────────────────────────────────── */

static void resolve_node_list(ResolveCtx *ctx, Iron_Node **nodes, int count) {
    for (int i = 0; i < count; i++) {
        /* HARD-05: cancel poll inside bulk list walker. */
        if (iron_cancel_requested(ctx->cancel_flag)) return;
        if (nodes[i]) resolve_node(ctx, nodes[i]);
    }
}

static void resolve_expr(ResolveCtx *ctx, Iron_Node *node) {
    if (!node) return;
    /* HARD-05: cancel poll at expression walker entry. */
    if (iron_cancel_requested(ctx->cancel_flag)) return;
    resolve_node(ctx, node);
}

static void resolve_node(ResolveCtx *ctx, Iron_Node *node) {
    if (!node) return;
    /* HARD-05: cancel poll at the top of the recursive switch dispatcher. */
    if (iron_cancel_requested(ctx->cancel_flag)) return;

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
                /* Receiver-form methods:
                 *   - Phase 82 in-block methods (`object T { func m() { self.x } }`)
                 *     synthesize a receiver param literally named `self`, so the
                 *     symbol IS in scope and resolves cleanly.
                 *   - Phase 79 style (`func (t: T) m()`) binds the receiver under
                 *     its declared name (e.g. `t`); `self` is NOT in scope, so
                 *     emit the "use the receiver's declared name" diagnostic
                 *     rather than leaking `Iron_self_x` into clang.
                 * Probe the scope: if `self` resolves, use it; otherwise, for
                 * receiver-form methods, emit the Phase 79 diagnostic. */
                Iron_Symbol *sym = iron_scope_lookup(ctx->current_scope, "self");
                if (sym) {
                    id->resolved_sym = sym;
                    return;
                }
                if (ctx->current_method->is_receiver_form) {
                    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                   IRON_ERR_SELF_OUTSIDE_METHOD, id->span,
                                   "'self' is not defined in a receiver-form "
                                   "method — use the receiver's declared name",
                                   NULL);
                    return;
                }
                /* Non-receiver-form method where implicit `self` was defined
                 * earlier in this branch at the md->owner_sym site — falling
                 * through here means the sym wasn't in scope, which would be
                 * a bug; remain silent and let downstream catch it. */
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
                /* Check if owning type has extends_name.
                 *
                 * PROT-04 rewrite (rank 10a, AUDIT-01): owner_sym can point
                 * to a non-object declaration (enum method, interface method)
                 * whose decl_node is Iron_EnumDecl or Iron_InterfaceDecl. The
                 * previous code cast unconditionally to Iron_ObjectDecl and
                 * silently misread extends_name at a foreign offset. Guard on
                 * decl_node->kind before the cast; emit a clean diagnostic
                 * for the non-object owner case. */
                if (ctx->current_method->owner_sym &&
                    ctx->current_method->owner_sym->decl_node) {
                    if (ctx->current_method->owner_sym->decl_node->kind !=
                        IRON_NODE_OBJECT_DECL) {
                        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                       IRON_ERR_SUPER_NO_PARENT, id->span,
                                       "'super' is only valid inside methods of object types",
                                       NULL);
                        return;
                    }
                    IRON_NODE_ASSERT_KIND(ctx->current_method->owner_sym->decl_node,
                                          IRON_NODE_OBJECT_DECL);
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
                /* Look up the parent type symbol.
                 *
                 * PROT-04 rewrite (rank 10b, AUDIT-01): the first guard above
                 * already returned for non-object owner, so this cast is
                 * structurally safe; still call IRON_NODE_ASSERT_KIND to
                 * document the invariant and catch any future guard drift. */
                IRON_NODE_ASSERT_KIND(ctx->current_method->owner_sym->decl_node,
                                      IRON_NODE_OBJECT_DECL);
                Iron_ObjectDecl *od =
                    (Iron_ObjectDecl *)ctx->current_method->owner_sym->decl_node;
                Iron_Symbol *parent_sym =
                    iron_scope_lookup(ctx->global_scope, od->extends_name);
                id->resolved_sym = parent_sym; /* may be NULL; caller checks */
                return;
            }

            /* Phase 87-02 SELF-01/02/03: "Self" is a contextual type keyword
             * that refers to the enclosing ObjectDecl type. Like lowercase
             * "self", it is not defined as a regular symbol in scope; the
             * typechecker handles its resolution. In method context, silently
             * skip the undefined-identifier check. Outside method context the
             * typechecker will emit E0259. */
            if (strcmp(id->name, "Self") == 0) {
                /* No resolver-level symbol; typechecker handles resolution. */
                break;
            }

            /* Normal identifier */
            Iron_Symbol *sym = iron_scope_lookup(ctx->current_scope, id->name);
            if (sym) {
                id->resolved_sym = sym;

                /* Phase 93 VIS-03: cross-module visibility check.
                 *
                 * Fires only on global-scope top-level symbols (FUNCTION /
                 * TYPE / ENUM / ENUM_VARIANT). Locals, params, fields, self,
                 * super, Self are never subject to this check (they are
                 * either earlier in this case body or have a different
                 * Iron_SymbolKind). IRON_SYM_INTERFACE is intentionally
                 * excluded — `pub interface` is deferred per RESEARCH Open
                 * Question 3. Stdlib carve-out (line-offset based) makes
                 * stdlib decls implicitly pub. */
                bool is_global =
                    sym->sym_kind == IRON_SYM_FUNCTION ||
                    sym->sym_kind == IRON_SYM_TYPE     ||
                    sym->sym_kind == IRON_SYM_ENUM     ||
                    sym->sym_kind == IRON_SYM_ENUM_VARIANT;
                if (is_global && !sym->is_pub &&
                    sym->span.filename && id->span.filename &&
                    !is_stdlib_decl(ctx, sym)) {
                    bool same_file =
                        sym->span.filename == id->span.filename ||
                        strcmp(sym->span.filename, id->span.filename) == 0;
                    if (!same_file) {
                        emit_cross_module_private(ctx, sym, id);
                    }
                }
            } else {
                emit_undefined(ctx, id->name, id->span);
            }
            break;
        }

        /* ── Declarations ─────────────────────────────────────────────────── */

        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)node;

            /* Extern funcs have no body — skip resolution entirely */
            if (fd->is_extern) break;

            push_scope(ctx, IRON_SCOPE_FUNCTION);
            if (ctx->current_scope) ctx->current_scope->owner_name = fd->name;

            /* Define params */
            for (int i = 0; i < fd->param_count; i++) {
                Iron_Param *p = (Iron_Param *)fd->params[i];
                Iron_Symbol *ps = iron_symbol_create(ctx->arena, p->name,
                                                      IRON_SYM_PARAM,
                                                      fd->params[i], p->span);
                /* HARD-09 CR-03: skip this param on arena OOM. */
                if (!ps) { ctx->in_error_recovery = true; continue; }
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
            if (ctx->current_scope) ctx->current_scope->owner_name = md->method_name;

            /* Phase 80 MUT-09: reject `mut` on non-struct receiver types.
             * Primitive types (Int, Float, Bool, String, etc.) ARE registered
             * in global scope as IRON_SYM_TYPE symbols (resolve.c:1037-1063),
             * but with decl_node=NULL (no user-declared AST node). User-declared
             * struct/object/enum types carry a non-NULL decl_node pointing at
             * the OBJECT_DECL or ENUM_DECL AST node. We use that distinction
             * to identify "struct-like" receiver owners:
             *   - user object/struct  -> sym_kind=IRON_SYM_TYPE, decl_node!=NULL
             *   - user enum           -> sym_kind=IRON_SYM_ENUM, decl_node!=NULL
             *   - primitive alias     -> sym_kind=IRON_SYM_TYPE, decl_node==NULL
             *   - undeclared type     -> owner_sym==NULL (attach_method emits E0200)
             * Any non-struct-like mut-receiver declaration gets the MUT-specific
             * diagnostic so the user sees actionable guidance instead of (or
             * in addition to) the generic "method declared on undeclared type".
             *
             * Fires once per offending declaration. Gated on is_receiver_form
             * + params[0]->is_mut_receiver so it never hits classic
             * `func T.m()` form or non-mut receiver-form methods. */
            if (md->is_receiver_form && md->param_count > 0) {
                Iron_Param *recv = (Iron_Param *)md->params[0];
                if (recv && recv->is_mut_receiver) {
                    bool owner_is_struct_like =
                        (md->owner_sym &&
                         md->owner_sym->decl_node &&
                         (md->owner_sym->sym_kind == IRON_SYM_TYPE ||
                          md->owner_sym->sym_kind == IRON_SYM_ENUM));
                    if (!owner_is_struct_like) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "'mut' on non-struct receiver type '%s' — "
                                 "only struct/composite types support mut",
                                 md->type_name ? md->type_name : "?");
                        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                       IRON_ERR_MUT_ON_PRIMITIVE, md->span,
                                       msg, NULL);
                    }
                }
            }

            /* Define implicit 'self' — classic form only. Receiver form
             * uses the declared receiver name (e.g. `t` in `func (t: Timer) ...`)
             * and has no implicit `self`; references to `self` in the body
             * should fall through to the normal undefined-identifier path. */
            if (md->owner_sym && !md->is_receiver_form) {
                Iron_Symbol *self_sym = iron_symbol_create(ctx->arena, "self",
                                                            IRON_SYM_VARIABLE,
                                                            node, md->span);
                /* HARD-09 CR-03: skip self binding on arena OOM. */
                if (self_sym) {
                    self_sym->is_mutable = true;
                    self_sym->type = md->owner_sym->type;
                    iron_scope_define(ctx->current_scope, ctx->arena, self_sym);
                } else {
                    ctx->in_error_recovery = true;
                }
            }

            /* Define params */
            for (int i = 0; i < md->param_count; i++) {
                Iron_Param *p = (Iron_Param *)md->params[i];
                Iron_Symbol *ps = iron_symbol_create(ctx->arena, p->name,
                                                      IRON_SYM_PARAM,
                                                      md->params[i], p->span);
                /* HARD-09 CR-03: skip this param on arena OOM. */
                if (!ps) { ctx->in_error_recovery = true; continue; }
                /* Phase 80 MUT-02: receiver-form methods use is_mut_receiver
                 * for the receiver binding (params[0] when md->is_receiver_form).
                 * Regular params — including the non-receiver params of a
                 * receiver-form method — continue to use the existing is_var
                 * rule. Phase 79's parser guarantees is_mut_receiver is false
                 * on non-receiver params. */
                if (md->is_receiver_form && i == 0) {
                    ps->is_mutable = p->is_mut_receiver;
                } else {
                    ps->is_mutable = p->is_var;
                }
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
            /* Phase 59 01d: destructure binding defines multiple symbols. */
            if (vd->binding_count > 0) {
                for (int i = 0; i < vd->binding_count; i++) {
                    if (vd->binding_names[i]) {  /* skip wildcards */
                        define_sym(ctx, vd->binding_names[i], IRON_SYM_VARIABLE,
                                   node, vd->span,
                                   /*is_mutable=*/false, /*is_private=*/false);
                    }
                }
            } else if (vd->name) {
                define_sym(ctx, vd->name, IRON_SYM_VARIABLE, node, vd->span,
                           /*is_mutable=*/false, /*is_private=*/false);
            }
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
            push_scope(ctx, IRON_SCOPE_BLOCK);
            if (mc->pattern) resolve_node(ctx, mc->pattern);
            if (mc->body) resolve_node(ctx, mc->body);
            pop_scope(ctx);
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
                /* HARD-09 CR-03: skip this param on arena OOM. */
                if (!ps) { ctx->in_error_recovery = true; continue; }
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

        case IRON_NODE_PATTERN: {
            Iron_Pattern *pat = (Iron_Pattern *)node;
            /* If fully-qualified (Shape.Circle), validate enum exists in scope */
            if (pat->enum_name) {
                Iron_Symbol *esym = iron_scope_lookup(ctx->current_scope, pat->enum_name);
                if (!esym || esym->sym_kind != IRON_SYM_ENUM) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "unknown enum '%s'", pat->enum_name);
                    const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
                    if (!msg_copy) { /* HARD-09 REPLACE (resolve.c:resolve_expr PATTERN unknown-enum msg) */ msg_copy = "analyzer error"; }
                    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                   IRON_ERR_UNKNOWN_VARIANT, pat->span,
                                   msg_copy, NULL);
                    break;
                }
                /* Validate variant exists in the enum */
                /* PROT-03 row 22 (AUDIT-01 M-severity): IRON_SYM_ENUM's
                 * decl_node should always be IRON_NODE_ENUM_DECL. The
                 * upstream sym_kind check above guarantees the symbol
                 * shape; assert kind on the decl_node before the cast so
                 * any future drift (e.g., a builtin enum with NULL
                 * decl_node, or a wrong-kind decl_node) aborts in Debug. */
                if (!esym->decl_node) break;
                /* HARD-10 REPLACE (audit row resolve.c:651):
                 * decl_node can be IRON_NODE_ERROR after parse recovery.
                 * Skip variant resolution gracefully instead of aborting. */
                if (esym->decl_node->kind != IRON_NODE_ENUM_DECL) {
                    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_NOTE,
                                   IRON_ERR_UNDEFINED_VAR, pat->span,
                                   "skipping pattern variant check on partially-parsed enum",
                                   NULL);
                    break;
                }
                Iron_EnumDecl *ed = (Iron_EnumDecl *)esym->decl_node;
                bool found = false;
                for (int i = 0; i < ed->variant_count; i++) {
                    Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[i];
                    if (strcmp(ev->name, pat->variant_name) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "enum '%s' has no variant '%s'",
                             pat->enum_name, pat->variant_name);
                    const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
                    if (!msg_copy) { /* HARD-09 REPLACE (resolve.c:resolve_expr PATTERN no-variant msg) */ msg_copy = "analyzer error"; }
                    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                   IRON_ERR_UNKNOWN_VARIANT, pat->span,
                                   msg_copy, NULL);
                    break;
                }
            }
            /* Introduce binding variables into current (arm) scope */
            for (int i = 0; i < pat->binding_count; i++) {
                const char *bname = pat->binding_names ? pat->binding_names[i] : NULL;
                if (!bname) {
                    /* Wildcard _ or nested pattern slot — recurse into nested pattern if present */
                    if (pat->nested_patterns && pat->nested_patterns[i]) {
                        resolve_node(ctx, pat->nested_patterns[i]);
                    }
                    continue;
                }
                /* Check for shadowing: look up in PARENT scope (arm scope's parent) */
                Iron_Symbol *outer = iron_scope_lookup(ctx->current_scope->parent, bname);
                if (outer) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "pattern binding '%s' shadows outer variable", bname);
                    const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
                    if (!msg_copy) { /* HARD-09 REPLACE (resolve.c:resolve_expr PATTERN shadow msg) */ msg_copy = "analyzer error"; }
                    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                   IRON_ERR_BINDING_SHADOWS, pat->span,
                                   msg_copy, NULL);
                }
                /* Define binding as immutable variable in arm scope */
                define_sym(ctx, bname, IRON_SYM_VARIABLE, node, pat->span,
                           /*is_mutable=*/false, /*is_private=*/false);
            }
            break;
        }

        case IRON_NODE_ENUM_CONSTRUCT: {
            Iron_EnumConstruct *ec = (Iron_EnumConstruct *)node;
            /* Validate enum exists; if not, reclassify as method call / field access
             * (the parser's uppercase heuristic may have misidentified a module
             *  static call like Time.Timer(1.0) or a module constant like Log.DEBUG). */
            Iron_Symbol *esym = iron_scope_lookup(ctx->current_scope, ec->enum_name);
            if (!esym || esym->sym_kind != IRON_SYM_ENUM) {
                Iron_Ident *ident_node = (Iron_Ident *)iron_arena_alloc(
                    ctx->arena, sizeof(Iron_Ident), _Alignof(Iron_Ident));
                if (!ident_node) { /* HARD-09 REPLACE (resolve.c:resolve_expr ENUM_CONSTRUCT ident_node) */ return; }
                ident_node->kind = IRON_NODE_IDENT;
                ident_node->span = ec->span;
                ident_node->name = ec->enum_name;
                ident_node->resolved_type = NULL;
                ident_node->resolved_sym = esym;
                const char *member = ec->variant_name;
                /* PROT-04 rewrite (ranks 7 and 8, AUDIT-01): allocate a fresh
                 * Iron_MethodCallExpr or Iron_FieldAccess node instead of
                 * reinterpreting Iron_EnumConstruct in place. The old pattern
                 * was layout-dependent UB: if any of the three struct types
                 * grew or reordered fields, the reinterpret would silently
                 * corrupt memory.
                 *
                 * Size fit: sizeof(Iron_MethodCallExpr) == sizeof(Iron_EnumConstruct)
                 * (both have identical 7-member layouts — verified by planner
                 * in src/parser/ast.h). sizeof(Iron_FieldAccess) <
                 * sizeof(Iron_EnumConstruct) (5 vs 7 members). The two
                 * _Static_asserts below fail the build immediately on any
                 * future struct-size regression, which is exactly what PROT-04
                 * is trying to prevent. */
                IRON_NODE_ASSERT_KIND(ec, IRON_NODE_ENUM_CONSTRUCT);
                /* WR-08: the _Static_asserts below prove the target layout
                 * fits in the source storage, so we populate the ec payload
                 * directly rather than round-tripping through a separate
                 * arena allocation. Local copies of the ec fields we need
                 * (span, args, arg_count) are captured before the cast-
                 * assignment to avoid aliasing issues when the target
                 * struct's layout overlaps the source fields. */
                if (ec->arg_count > 0) {
                    _Static_assert(sizeof(Iron_MethodCallExpr) <= sizeof(Iron_EnumConstruct),
                                   "enum-construct-to-method-call rewrite requires size fit");
                    Iron_Span    ec_span   = ec->span;
                    Iron_Node  **ec_args   = ec->args;
                    int          ec_argc   = ec->arg_count;
                    Iron_MethodCallExpr *mc_slot = (Iron_MethodCallExpr *)ec;
                    mc_slot->span          = ec_span;
                    mc_slot->kind          = IRON_NODE_METHOD_CALL;
                    mc_slot->resolved_type = NULL;
                    mc_slot->object        = (Iron_Node *)ident_node;
                    mc_slot->method        = member;
                    mc_slot->args          = ec_args;
                    mc_slot->arg_count     = ec_argc;
                    resolve_expr(ctx, (Iron_Node *)ec);
                } else {
                    _Static_assert(sizeof(Iron_FieldAccess) <= sizeof(Iron_EnumConstruct),
                                   "enum-construct-to-field-access rewrite requires size fit");
                    Iron_Span    ec_span   = ec->span;
                    Iron_FieldAccess *fa_slot = (Iron_FieldAccess *)ec;
                    fa_slot->span          = ec_span;
                    fa_slot->kind          = IRON_NODE_FIELD_ACCESS;
                    fa_slot->resolved_type = NULL;
                    fa_slot->object        = (Iron_Node *)ident_node;
                    fa_slot->field         = member;
                    /* Phase 83-02: defensive default so downstream passes
                     * do not read uninitialized state if this rewrite is
                     * ever exercised before typecheck. */
                    fa_slot->is_pub_access = false;
                    resolve_expr(ctx, (Iron_Node *)ec);
                }
                break;
            }
            /* Validate variant exists */
            /* PROT-03 row 23 (AUDIT-01 M-severity): same pattern as row 22 —
             * assert IRON_NODE_ENUM_DECL before casting esym->decl_node. */
            if (!esym->decl_node) break;
            /* HARD-10 REPLACE (audit row resolve.c:775):
             * decl_node can be IRON_NODE_ERROR after parse recovery.
             * Skip variant validation gracefully. */
            if (esym->decl_node->kind != IRON_NODE_ENUM_DECL) {
                iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_NOTE,
                               IRON_ERR_UNDEFINED_VAR, ec->span,
                               "skipping enum-construct check on partially-parsed enum",
                               NULL);
                break;
            }
            Iron_EnumDecl *ed = (Iron_EnumDecl *)esym->decl_node;
            bool found = false;
            for (int i = 0; i < ed->variant_count; i++) {
                Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[i];
                if (strcmp(ev->name, ec->variant_name) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                char msg[256];
                snprintf(msg, sizeof(msg), "enum '%s' has no variant '%s'",
                         ec->enum_name, ec->variant_name);
                const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
                if (!msg_copy) { /* HARD-09 REPLACE (resolve.c:resolve_expr ENUM_CONSTRUCT no-variant msg) */ msg_copy = "analyzer error"; }
                iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                               IRON_ERR_UNKNOWN_VARIANT, ec->span,
                               msg_copy, NULL);
                break;
            }
            /* Resolve arg expressions */
            for (int i = 0; i < ec->arg_count; i++) {
                if (ec->args[i]) resolve_expr(ctx, ec->args[i]);
            }
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

        case IRON_NODE_COUNT:
            /* sentinel — never a real node kind */
            break;
    }
}

/* ── Phase 86 PATCH-02/04: program-global patch registry ─────────────────── */

/* One entry per `patch object T { ... }` contribution. `target` is the
 * target type name (aliases od->target_type_name / od->name — same arena
 * strdup). `methods` is a stb_ds dynamic array of Iron_MethodDecl *
 * contributed by this patch body ONLY; the lookup path walks multiple
 * entries when several patches target the same type and flattens them
 * into a single array via a registry-owned concat buffer. */
typedef struct PatchEntry {
    const char       *target;
    Iron_MethodDecl **methods;
} PatchEntry;

/* One registry per Iron_Program. The struct layout is hidden from the
 * public API via the forward-declared Iron_TypePatchRegistry typedef in
 * resolve.h — callers only see the opaque handle. */
struct Iron_TypePatchRegistry {
    PatchEntry       *entries;           /* stb_ds */
    /* Scratch buffer for concat results from iron_type_patch_lookup.
     * Cached to keep the lookup result lifetime tied to the registry per
     * the API contract. Freed via arrfree at iron_type_patch_registry_free. */
    Iron_MethodDecl **lookup_scratch;    /* stb_ds */
};

/* Primitive target allowlist. Locked per Plan 86-02 <must_haves>: Int,
 * Int32, Float, String, Bool. Targets outside this list that do not
 * resolve to a user-declared object emit E0254. Extend only via an
 * explicit Plan change; the allowlist is spec surface. */
static const char *const k_primitive_patch_targets[] = {
    "Int", "Int32", "Float", "String", "Bool"
};
static const int k_primitive_patch_target_count =
    (int)(sizeof(k_primitive_patch_targets) / sizeof(k_primitive_patch_targets[0]));

static bool patch_target_is_primitive(const char *name) {
    if (!name) return false;
    for (int i = 0; i < k_primitive_patch_target_count; i++) {
        if (strcmp(k_primitive_patch_targets[i], name) == 0) return true;
    }
    return false;
}

/* Return the stb_ds index of the entry whose target matches `name`, or
 * -1 if no entry exists yet. Linear scan; realistic programs have a
 * handful of patched targets. Promote to a hash if a benchmark motivates. */
static int patch_registry_find_entry(Iron_TypePatchRegistry *reg,
                                      const char *name) {
    if (!reg || !name) return -1;
    for (ptrdiff_t i = 0; i < arrlen(reg->entries); i++) {
        if (reg->entries[i].target &&
            strcmp(reg->entries[i].target, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

Iron_TypePatchRegistry *iron_type_patch_registry_build(Iron_Program *program,
                                                       Iron_Scope    *global_scope,
                                                       Iron_Arena    *arena,
                                                       Iron_DiagList *diags) {
    (void)arena; /* registry itself is heap-owned via stb_ds; arena holds shared MethodDecl * */
    if (!program) return NULL;

    Iron_TypePatchRegistry *reg =
        (Iron_TypePatchRegistry *)malloc(sizeof(Iron_TypePatchRegistry));
    if (!reg) iron_oom_abort("resolve.c:iron_type_patch_registry_build reg");
    reg->entries        = NULL;
    reg->lookup_scratch = NULL;

    /* Pass A: register patch targets. Walk every top-level ObjectDecl with
     * is_patch=true; validate target (user object OR primitive allowlist);
     * emit E0254 on unknown targets and skip registration (the patch's
     * methods remain reachable via their MethodDecl nodes but are never
     * dispatched). */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
        if (!od->is_patch) continue;
        const char *target = od->target_type_name ? od->target_type_name : od->name;
        if (!target) continue;

        bool target_is_user_object = false;
        if (global_scope) {
            Iron_Symbol *sym = iron_scope_lookup(global_scope, target);
            /* Accept user objects (sym_kind=IRON_SYM_TYPE with decl_node pointing
             * at an OBJECT_DECL) — primitives also live under IRON_SYM_TYPE but
             * their decl_node is NULL; the primitive allowlist covers them
             * explicitly below so we do not confuse a patch on `Int64` (not in
             * the allowlist) with a valid user object. */
            if (sym && sym->sym_kind == IRON_SYM_TYPE && sym->decl_node &&
                sym->decl_node->kind == IRON_NODE_OBJECT_DECL) {
                target_is_user_object = true;
            }
        }
        bool target_is_primitive = patch_target_is_primitive(target);

        if (!target_is_user_object && !target_is_primitive) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "patch target '%s' not found (expected user type or "
                     "primitive Int/Int32/Float/String/Bool)",
                     target);
            const char *msg_copy = iron_arena_strdup(arena, msg, strlen(msg));
            if (!msg_copy) iron_oom_abort("resolve.c:patch target msg");
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_PATCH_TARGET_NOT_FOUND, od->span,
                           msg_copy, NULL);
            continue;
        }

        /* Reserve an entry if none exists for this target yet. Same-target
         * multi-patch is a single lookup-merge: each patch body contributes
         * its own PatchEntry; iron_type_patch_lookup flattens them. This
         * preserves declaration-order across patches for dispatch (Plan
         * 86-02 collision scan guarantees same-name uniqueness). */
        PatchEntry e;
        e.target  = target;
        e.methods = NULL;
        arrput(reg->entries, e);
    }

    /* Pass B: collect every patched MethodDecl * into the appropriate
     * entry. Plan 86-01's iron_parse_patch_decl flushed patched members
     * as top-level Iron_MethodDecl nodes via the extra_decls_out channel;
     * their type_name is set to the patch target. We attribute a method
     * to a patch entry when its type_name matches a registered patch
     * target AND a matching patch ObjectDecl precedes it in decl order.
     *
     * Attribution strategy: per top-level MethodDecl, we look for a patch
     * ObjectDecl on the same target whose span occurs before ours. If
     * there are N patch bodies on the same target, the nearest preceding
     * is_patch=true ObjectDecl on that target is the owning body; the
     * order-preserving walk naturally groups methods under their flushed
     * patch parent. Since the parser flushes the body's methods
     * immediately after the ObjectDecl, a simple "last patch target seen
     * before this method" state is sufficient.
     *
     * We still scan every entry, not every MethodDecl — keeps the
     * attribution deterministic even when parser output order changes. */
    const char *last_patch_target = NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        if (d->kind == IRON_NODE_OBJECT_DECL) {
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
            if (od->is_patch) {
                last_patch_target = od->target_type_name
                                    ? od->target_type_name : od->name;
            } else {
                last_patch_target = NULL;
            }
            continue;
        }
        if (d->kind != IRON_NODE_METHOD_DECL) {
            /* Classic func decls in between break the run; a classic
             * method on a non-patched type also clears the state. */
            if (d->kind != IRON_NODE_FUNC_DECL) last_patch_target = NULL;
            continue;
        }
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (!last_patch_target || !md->type_name) continue;
        /* Only gather methods whose type_name matches the current patch
         * body's target — a safety net against accidental runs if the
         * flush order ever changes. */
        if (strcmp(md->type_name, last_patch_target) != 0) continue;

        int idx = patch_registry_find_entry(reg, last_patch_target);
        if (idx < 0) continue; /* unknown target — E0254 already emitted */
        arrput(reg->entries[idx].methods, md);
    }

    return reg;
}

Iron_MethodDecl **iron_type_patch_lookup(Iron_TypePatchRegistry *reg,
                                          const char              *target_type_name) {
    if (!reg || !target_type_name) return NULL;
    /* Reset the scratch buffer; preserve the caller contract by returning
     * a registry-owned pointer whose lifetime equals the registry. */
    arrsetlen(reg->lookup_scratch, 0);
    for (ptrdiff_t i = 0; i < arrlen(reg->entries); i++) {
        if (!reg->entries[i].target) continue;
        if (strcmp(reg->entries[i].target, target_type_name) != 0) continue;
        for (ptrdiff_t j = 0; j < arrlen(reg->entries[i].methods); j++) {
            arrput(reg->lookup_scratch, reg->entries[i].methods[j]);
        }
    }
    return reg->lookup_scratch;
}

void iron_type_patch_registry_free(Iron_TypePatchRegistry *reg) {
    if (!reg) return;
    for (ptrdiff_t i = 0; i < arrlen(reg->entries); i++) {
        arrfree(reg->entries[i].methods);
    }
    arrfree(reg->entries);
    arrfree(reg->lookup_scratch);
    free(reg);
}


/* ── Phase 4 Plan 04-01 (EDIT-07): unused-import post-pass walker ────────── */
/* After Pass 2 completes, walk every Iron_Ident in the program AST looking
 * for uses of an import alias. Any IMPORT_DECL whose alias was never
 * referenced emits IRON_WARN_UNUSED_IMPORT with a non-NULL .suggestion
 * (empty string acts as a sentinel meaning "delete the line" in the
 * code-action dispatch layer of Plan 04-04; keeping it non-NULL satisfies
 * the "every P1 emit site populates .suggestion" invariant). */

typedef struct {
    const char **used_alias_set;   /* stb_ds dynamic array of alias names */
} UnusedImportScan;

static void scan_collect_ident(UnusedImportScan *s, Iron_Node *node);

static void scan_collect_list(UnusedImportScan *s, Iron_Node **nodes, int n) {
    if (!nodes) return;
    for (int i = 0; i < n; i++) if (nodes[i]) scan_collect_ident(s, nodes[i]);
}

/* Mark an import alias name as "used". O(n) dedup scan — alias sets are
 * small (<~20 per file in practice). */
static void mark_alias_used(UnusedImportScan *s, const char *alias) {
    if (!alias) return;
    for (ptrdiff_t i = 0; i < arrlen(s->used_alias_set); i++) {
        if (strcmp(s->used_alias_set[i], alias) == 0) return;
    }
    arrput(s->used_alias_set, alias);
}

/* Walk node subtree collecting any Iron_Ident whose resolved_sym's decl_node
 * kind is IRON_NODE_IMPORT_DECL. These are the "used import" names. */
static void scan_collect_ident(UnusedImportScan *s, Iron_Node *node) {
    if (!node) return;
    switch ((int)node->kind) {
        case IRON_NODE_IDENT: {
            Iron_Ident *id = (Iron_Ident *)node;
            if (id->resolved_sym && id->resolved_sym->decl_node &&
                id->resolved_sym->decl_node->kind == IRON_NODE_IMPORT_DECL) {
                /* The alias symbol's name *is* the alias. */
                mark_alias_used(s, id->resolved_sym->name);
            }
            break;
        }
        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)node;
            scan_collect_ident(s, fd->body);
            break;
        }
        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *md = (Iron_MethodDecl *)node;
            scan_collect_ident(s, md->body);
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)node;
            scan_collect_list(s, b->stmts, b->stmt_count);
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            scan_collect_ident(s, vd->init);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            scan_collect_ident(s, vd->init);
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            scan_collect_ident(s, rs->value);
            break;
        }
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *be = (Iron_BinaryExpr *)node;
            scan_collect_ident(s, be->left);
            scan_collect_ident(s, be->right);
            break;
        }
        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *ue = (Iron_UnaryExpr *)node;
            scan_collect_ident(s, ue->operand);
            break;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)node;
            scan_collect_ident(s, ce->callee);
            scan_collect_list(s, ce->args, ce->arg_count);
            break;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
            scan_collect_ident(s, fa->object);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            scan_collect_ident(s, is->condition);
            scan_collect_ident(s, is->body);
            scan_collect_ident(s, is->else_body);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;
            scan_collect_ident(s, as->target);
            scan_collect_ident(s, as->value);
            break;
        }
        default:
            /* Other kinds (literals, type decls, etc.) don't carry import refs. */
            break;
    }
}

/* Emit IRON_WARN_UNUSED_IMPORT for each import alias that was never
 * referenced. Non-aliased imports have no in-scope symbol to track, so
 * they are conservatively NOT flagged here (a future plan may extend
 * the tracker to cover path-based references). */
static void emit_unused_imports(ResolveCtx *ctx, Iron_Program *program) {
    UnusedImportScan s = { NULL };

    /* Build the "used alias" set by walking every declaration body. */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        scan_collect_ident(&s, d);
    }

    /* Emit for every aliased IMPORT_DECL whose alias isn't in the used set. */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_IMPORT_DECL) continue;
        Iron_ImportDecl *imp = (Iron_ImportDecl *)d;
        if (!imp->alias) continue; /* only aliased imports are tracked */

        bool used = false;
        for (ptrdiff_t k = 0; k < arrlen(s.used_alias_set); k++) {
            if (strcmp(s.used_alias_set[k], imp->alias) == 0) { used = true; break; }
        }
        if (used) continue;

        /* Empty-string suggestion is the "delete line" sentinel. arena-strdup
         * so .suggestion is non-NULL and owned by the compilation arena. */
        const char *sug = iron_arena_strdup(ctx->arena, "", 0);
        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_WARNING,
                       IRON_WARN_UNUSED_IMPORT, imp->span,
                       "unused import", sug);
    }

    if (s.used_alias_set) arrfree(s.used_alias_set);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

Iron_Scope *iron_resolve(Iron_Program *program, Iron_Arena *arena,
                          Iron_DiagList *diags,
                          const _Atomic bool *cancel_flag) {
    if (!program) return NULL;
    /* HARD-05: pre-entry cancel check. */
    if (iron_cancel_requested(cancel_flag)) return NULL;

    ResolveCtx ctx;
    ctx.arena              = arena;
    ctx.diags              = diags;
    ctx.global_scope       = iron_scope_create(arena, NULL, IRON_SCOPE_GLOBAL);
    /* HARD-09 CR-03: if the top-level arena allocation fails we cannot
     * proceed with resolution. Emit a meta-diagnostic and return NULL so
     * the caller can surface it without aborting the process. */
    if (!ctx.global_scope) {
        Iron_Span empty_span = {0};
        iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                       IRON_ERR_LEXER_OOM, empty_span,
                       "out of memory while creating global scope", NULL);
        return NULL;
    }
    ctx.current_scope      = ctx.global_scope;
    ctx.current_method     = NULL;
    ctx.current_type_name  = NULL;
    ctx.cancel_flag        = cancel_flag;
    ctx.skipped_scope_pushes = 0;
    ctx.in_error_recovery  = false;
    /* Initialize cross-module check state. The carve-out line is read
     * from Iron_Program (which copies it from Iron_Parser at parse
     * exit). build.c / check.c populate the parser value after stdlib
     * prepends; user code without prepends has user_source_start_line
     * == 0, which keeps the gate inert (no real line satisfies
     * line < 0). */
    ctx.emitted_first_e0320    = false;
    ctx.user_source_start_line = program->user_source_start_line;

    /* Initialize type system */
    iron_types_init(arena);

    /* FIX-03 / AUDIT-04 §4: SAFETY — the builtin symbol registration block
     * below (lines ~849-~1010, print/println/len/min/max/clamp/abs/assert/
     * range/...) allocates each Iron_Symbol via iron_symbol_create on the
     * compilation arena, then stores it in ctx.global_scope->symbols (the
     * stb_ds shmap covered by §3). The stb_ds shmap holds pointers whose
     * BACKING storage is the compilation arena; when the arena is freed,
     * the shmap's key bytes (duplicated by sh_new_strdup at scope.c:17)
     * remain heap-owned and leak to process exit. The VALUES (Iron_Symbol*)
     * are arena-owned and reclaimed at arena teardown. No dangling-pointer
     * risk: shmap readers always run before arena teardown (resolve and
     * typecheck share the same ctx lifetime). Same cross-arena coupling as
     * §3; same tradeoff justification. */
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
            /* HARD-09 CR-03: skip builtin on arena OOM. */
            if (!bsym) { ctx.in_error_recovery = true; continue; }
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
            /* HARD-09 CR-03: skip builtin on arena OOM. */
            if (sym) {
                sym->type = fn;
                iron_scope_define(ctx.global_scope, arena, sym);
            } else {
                ctx.in_error_recovery = true;
            }
        }
        /* min(Int, Int) -> Int */
        {
            Iron_Type *params[2] = { int_t, int_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 2, int_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "min",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            /* HARD-09 CR-03: skip builtin on arena OOM. */
            if (sym) { sym->type = fn; iron_scope_define(ctx.global_scope, arena, sym); }
            else     { ctx.in_error_recovery = true; }
        }
        /* max(Int, Int) -> Int */
        {
            Iron_Type *params[2] = { int_t, int_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 2, int_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "max",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            /* HARD-09 CR-03: skip builtin on arena OOM. */
            if (sym) { sym->type = fn; iron_scope_define(ctx.global_scope, arena, sym); }
            else     { ctx.in_error_recovery = true; }
        }
        /* clamp(Int, Int, Int) -> Int */
        {
            Iron_Type *params[3] = { int_t, int_t, int_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 3, int_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "clamp",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            /* HARD-09 CR-03: skip builtin on arena OOM. */
            if (sym) { sym->type = fn; iron_scope_define(ctx.global_scope, arena, sym); }
            else     { ctx.in_error_recovery = true; }
        }
        /* abs(Int) -> Int */
        {
            Iron_Type *params[1] = { int_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 1, int_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "abs",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            /* HARD-09 CR-03: skip builtin on arena OOM. */
            if (sym) { sym->type = fn; iron_scope_define(ctx.global_scope, arena, sym); }
            else     { ctx.in_error_recovery = true; }
        }
        /* range(Int) -> Int */
        {
            Iron_Type *params[1] = { int_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 1, int_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "range",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            /* HARD-09 CR-03: skip builtin on arena OOM. */
            if (sym) { sym->type = fn; iron_scope_define(ctx.global_scope, arena, sym); }
            else     { ctx.in_error_recovery = true; }
        }
        /* assert(Bool) -> Void */
        {
            Iron_Type *params[1] = { bool_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 1, void_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "assert",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            /* HARD-09 CR-03: skip builtin on arena OOM. */
            if (sym) { sym->type = fn; iron_scope_define(ctx.global_scope, arena, sym); }
            else     { ctx.in_error_recovery = true; }
        }
        /* read_file(String) -> String — comptime only */
        {
            Iron_Type *params[1] = { str_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 1, str_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "read_file",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            /* HARD-09 CR-03: skip builtin on arena OOM. */
            if (sym) { sym->type = fn; iron_scope_define(ctx.global_scope, arena, sym); }
            else     { ctx.in_error_recovery = true; }
        }
        /* fill(Int, Int) -> [Int]  (type checker special-cases to infer [T]) */
        {
            Iron_Type *arr_t = iron_type_make_array(arena, int_t, -1);
            Iron_Type *params[2] = { int_t, int_t };
            Iron_Type *fn = iron_type_make_func(arena, params, 2, arr_t);
            Iron_Symbol *sym = iron_symbol_create(arena, "fill",
                                                   IRON_SYM_FUNCTION, NULL, no_span);
            /* HARD-09 CR-03: skip builtin on arena OOM. */
            if (sym) { sym->type = fn; iron_scope_define(ctx.global_scope, arena, sym); }
            else     { ctx.in_error_recovery = true; }
        }
    }

    /* Register primitive type names (Int, Float, Bool, etc.) as IRON_SYM_TYPE
     * so that Float(x), Int(x), Bool(x) casts resolve during name resolution.
     * Each symbol's type is the corresponding primitive type singleton. */
    {
        Iron_Span no_span = {0};
        static const struct { const char *name; Iron_TypeKind kind; } prims[] = {
            { "Int",     IRON_TYPE_INT     },
            { "Int8",    IRON_TYPE_INT8    },
            { "Int16",   IRON_TYPE_INT16   },
            { "Int32",   IRON_TYPE_INT32   },
            { "Int64",   IRON_TYPE_INT64   },
            { "UInt",    IRON_TYPE_UINT    },
            { "UInt8",   IRON_TYPE_UINT8   },
            { "UInt16",  IRON_TYPE_UINT16  },
            { "UInt32",  IRON_TYPE_UINT32  },
            { "UInt64",  IRON_TYPE_UINT64  },
            { "Float",   IRON_TYPE_FLOAT   },
            { "Float32", IRON_TYPE_FLOAT32 },
            { "Float64", IRON_TYPE_FLOAT64 },
            { "Bool",    IRON_TYPE_BOOL    },
            { "String",  IRON_TYPE_STRING  },
        };
        for (int pi = 0; pi < (int)(sizeof(prims)/sizeof(prims[0])); pi++) {
            Iron_Type *pt = iron_type_make_primitive(prims[pi].kind);
            Iron_Symbol *sym = iron_symbol_create(arena, prims[pi].name,
                                                   IRON_SYM_TYPE, NULL, no_span);
            /* HARD-09 CR-03: skip primitive type registration on arena OOM. */
            if (!sym) { ctx.in_error_recovery = true; continue; }
            sym->type = pt;
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

        switch ((int)(decl->kind)) {
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
            /* -Wswitch-enum opt-out: Pass 2 runs over top-level declarations;
             * any stray expression/statement kinds reach the default and go
             * through the generic resolve_node path. */
            default:
                resolve_node(&ctx, decl);
                break;
        }
    }

    /* Phase 4 Plan 04-01 (EDIT-07): post-pass — flag any aliased imports
     * that never resolved a reference. Runs after Pass 2 so every
     * Iron_Ident has its resolved_sym set (or NULL for unresolved). */
    emit_unused_imports(&ctx, program);

    return ctx.global_scope;
}
