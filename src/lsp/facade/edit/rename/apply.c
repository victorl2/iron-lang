/* Phase 4 Plan 04-06 Task 02 (EDIT-11, D-11, PITFALL B) -- rename apply
 * facade. Builds a WorkspaceEdit workspace-wide for the cursor symbol.
 *
 * Flow (lifted from facade/nav/references.c + extended):
 *   1. Analyze the open doc to resolve cursor -> Iron_Symbol.
 *   2. Same-name short-circuit: if new_name == old_name emit empty edit.
 *   3. Bulk-analyze gate (first-time only):
 *        ilsp_workspace_index_bulk_analyze_for_refs(wi, cancel);
 *      TODO(phase-7): emit $/progress during the walk when the client
 *      supports WorkDoneProgress.
 *   4. Derive identity triple; query ilsp_refs_query for use-sites.
 *   5. PITFALL B: if symbol is an interface method, fan out to implementors.
 *      ANY implementor with canonical_path starting "stdlib://" or
 *      "dep://" rejects the entire rename.
 *   6. Collect enclosing scope per use-site (lazy-analyze for closed
 *      files; for the open doc the root scope is sufficient as the
 *      LSP facade does not expose fine-grained scope-at-cursor today
 *      — the global scope walk covers the collision cases we care
 *      about: same-file shadows + top-level redefinitions).
 *   7. Collision check: ANY conflict -> FAIL_COLLISION.
 *   8. Group use-sites by canonical_path, sort DESC within file, sort
 *      files ASC across files. Emit per caps.
 */

#include "lsp/facade/edit/rename/apply.h"

#include "lsp/facade/edit/rename/collision.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/nav/node_at.h"
#include "lsp/facade/nav/symbol_id.h"
#include "lsp/facade/nav/references_index.h"
#include "lsp/facade/nav/iface_workspace.h"
#include "lsp/facade/compile.h"
#include "lsp/facade/span.h"
#include "lsp/server/server.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h"
#include "analyzer/scope.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal structures ─────────────────────────────────────────── */

/* Intermediate site collector: (canonical_path, span) pairs before
 * grouping into per-file edit lists. */
typedef struct SiteRaw {
    const char   *uri;           /* arena-owned */
    IronLsp_Range range;
    /* For per-file DESC sort we need a sortable key. Range already has
     * start.line + start.character; we compute a composite 64-bit
     * offset (line << 32 | character). */
    uint64_t      sort_key;
} SiteRaw;

static uint64_t range_sort_key(IronLsp_Range r) {
    return ((uint64_t)r.start.line << 32) | (uint64_t)r.start.character;
}

static int siteraw_cmp_desc(const void *a, const void *b) {
    const SiteRaw *sa = (const SiteRaw *)a;
    const SiteRaw *sb = (const SiteRaw *)b;
    if (sa->sort_key < sb->sort_key) return 1;   /* DESC */
    if (sa->sort_key > sb->sort_key) return -1;
    return 0;
}

/* Lift the ident-at-cursor helper locally to avoid an extra TU
 * dependency on prepare.c (the symbol isn't exported). */
static const Iron_Symbol *ident_at_cursor(const IronLsp_Document   *doc,
                                            const Iron_Program       *program,
                                            IronLsp_Position          pos,
                                            IronLsp_PositionEncoding  enc,
                                            Iron_Node               **out_ident) {
    if (out_ident) *out_ident = NULL;
    Iron_Node *n = ilsp_nav_node_at(doc, program, pos, enc);
    if (!n) return NULL;
    if (out_ident) *out_ident = n;
    if (n->kind == IRON_NODE_IDENT) {
        const Iron_Ident *id = (const Iron_Ident *)n;
        return id->resolved_sym;
    }
    return NULL;
}

/* Return true if the canonical path indicates a stdlib/dep location
 * (PITFALL B filter). */
static bool path_is_stdlib(const char *p) {
    return p && strncmp(p, "stdlib://", 9) == 0;
}
static bool path_is_dep(const char *p) {
    return p && strncmp(p, "dep://", 6) == 0;
}

/* Arena-format small strings. */
static const char *arena_printf(Iron_Arena *arena, const char *fmt, ...) {
    if (!arena) return "";
    va_list ap;
    va_start(ap, fmt);
    /* First pass: size calculation. */
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (need < 0) { va_end(ap); return ""; }
    size_t cap = (size_t)need + 1;
    char *buf = (char *)iron_arena_alloc(arena, cap, 1);
    if (!buf) { va_end(ap); return ""; }
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return buf;
}

/* Build a decl site for the target itself so the decl name also gets
 * rewritten. Returns true on success. The decl span may live in the
 * open doc OR in a workspace entry; we resolve the URI + range
 * appropriately. */
static bool build_decl_site(const Iron_Symbol              *sym,
                              IronLsp_Document              *doc,
                              IronLsp_PositionEncoding       enc,
                              Iron_Arena                    *arena,
                              IronLsp_WorkspaceIndex        *wi,
                              SiteRaw                       *out) {
    memset(out, 0, sizeof(*out));
    if (!sym || !sym->decl_node) return false;
    Iron_Span dspan = sym->decl_node->span;
    const char *fn = dspan.filename;
    /* Same-file? */
    bool same = false;
    if (!fn || !*fn) {
        same = true;
    } else if (doc && doc->uri) {
        if (strcmp(fn, doc->uri) == 0) same = true;
        else {
            size_t fl = strlen(fn), ul = strlen(doc->uri);
            same = (ul >= fl && strcmp(doc->uri + (ul - fl), fn) == 0);
        }
    }

    if (same && doc) {
        out->range = ilsp_span_to_lsp_range(dspan, doc, enc);
        out->uri   = doc->uri
            ? iron_arena_strdup(arena, doc->uri, strlen(doc->uri)) : "";
    } else if (wi && fn) {
        IronLsp_IndexEntry *entry = ilsp_workspace_index_lookup(wi, fn);
        if (entry) {
            out->range = ilsp_nav_entry_span_to_range(entry, dspan, enc);
            out->uri   = ilsp_nav_path_to_uri(fn, arena);
        } else {
            IronLsp_Range z = { {0,0}, {0,0} };
            out->range = z;
            out->uri   = fn ? ilsp_nav_path_to_uri(fn, arena) : "";
        }
    } else {
        IronLsp_Range z = { {0,0}, {0,0} };
        out->range = z;
        out->uri   = fn ? ilsp_nav_path_to_uri(fn, arena) : "";
    }
    if (!out->uri) out->uri = "";
    out->sort_key = range_sort_key(out->range);
    return true;
}

/* Walk the open doc's program collecting identifier use-sites whose
 * resolved_sym's decl_node matches target_decl. Mirrors the
 * references.c same-file fallback walker.
 *
 * Results appended to raw[] stb_ds-free simple growable array; caller
 * passes cap pointer for growth. */
static void gather_open_doc_sites(const Iron_Program *program,
                                    const Iron_Node     *target_decl,
                                    IronLsp_Document    *doc,
                                    IronLsp_PositionEncoding enc,
                                    Iron_Arena          *arena,
                                    SiteRaw            **raw,
                                    size_t              *n,
                                    size_t              *cap) {
    if (!program || !target_decl || !doc) return;
    Iron_Node **stack = NULL;
    size_t sp = 0, sc = 64;
    stack = (Iron_Node **)malloc(sc * sizeof(*stack));
    if (!stack) return;
#define PUSH(ptr) do { \
        Iron_Node *_n_ = (Iron_Node *)(ptr); \
        if (_n_) { \
            if (sp >= sc) { sc *= 2; \
                Iron_Node **ns = (Iron_Node **)realloc(stack, sc * sizeof(*stack)); \
                if (!ns) goto gather_done; stack = ns; } \
            stack[sp++] = _n_; \
        } \
    } while (0)

    for (int i = 0; i < program->decl_count; i++) PUSH(program->decls[i]);
    while (sp > 0) {
        Iron_Node *cur = stack[--sp];
        if (!cur || cur->kind == IRON_NODE_ERROR) continue;
        switch ((int)cur->kind) {
            case IRON_NODE_IDENT: {
                Iron_Ident *id = (Iron_Ident *)cur;
                if (id->resolved_sym &&
                    id->resolved_sym->decl_node == target_decl) {
                    /* Grow + append. */
                    if (*n == *cap) {
                        size_t ncap = (*cap) * 2;
                        if (ncap < 8) ncap = 8;
                        SiteRaw *nr = (SiteRaw *)iron_arena_alloc(
                            arena, ncap * sizeof(**raw), _Alignof(SiteRaw));
                        if (!nr) break;
                        if (*raw && *n > 0) memcpy(nr, *raw, (*n) * sizeof(**raw));
                        *raw = nr; *cap = ncap;
                    }
                    SiteRaw s;
                    memset(&s, 0, sizeof(s));
                    s.range = ilsp_span_to_lsp_range(id->span, doc, enc);
                    s.uri   = doc->uri
                        ? iron_arena_strdup(arena, doc->uri, strlen(doc->uri)) : "";
                    s.sort_key = range_sort_key(s.range);
                    (*raw)[*n] = s;
                    (*n)++;
                }
                break;
            }
            case IRON_NODE_FUNC_DECL:   PUSH(((Iron_FuncDecl   *)cur)->body); break;
            case IRON_NODE_METHOD_DECL: PUSH(((Iron_MethodDecl *)cur)->body); break;
            case IRON_NODE_BLOCK: {
                Iron_Block *b = (Iron_Block *)cur;
                for (int i = 0; i < b->stmt_count; i++) PUSH(b->stmts[i]);
                break;
            }
            case IRON_NODE_BINARY: {
                Iron_BinaryExpr *e = (Iron_BinaryExpr *)cur;
                PUSH(e->left); PUSH(e->right); break;
            }
            case IRON_NODE_UNARY: PUSH(((Iron_UnaryExpr *)cur)->operand); break;
            case IRON_NODE_CALL: {
                Iron_CallExpr *c = (Iron_CallExpr *)cur;
                PUSH(c->callee);
                for (int i = 0; i < c->arg_count; i++) PUSH(c->args[i]);
                break;
            }
            case IRON_NODE_METHOD_CALL: {
                Iron_MethodCallExpr *m = (Iron_MethodCallExpr *)cur;
                PUSH(m->object);
                for (int i = 0; i < m->arg_count; i++) PUSH(m->args[i]);
                break;
            }
            case IRON_NODE_FIELD_ACCESS: PUSH(((Iron_FieldAccess *)cur)->object); break;
            case IRON_NODE_INDEX: {
                Iron_IndexExpr *ix = (Iron_IndexExpr *)cur;
                PUSH(ix->object); PUSH(ix->index); break;
            }
            case IRON_NODE_SLICE: {
                Iron_SliceExpr *sl = (Iron_SliceExpr *)cur;
                PUSH(sl->object); PUSH(sl->start); PUSH(sl->end); break;
            }
            case IRON_NODE_ASSIGN: {
                Iron_AssignStmt *a = (Iron_AssignStmt *)cur;
                PUSH(a->target); PUSH(a->value); break;
            }
            case IRON_NODE_RETURN: PUSH(((Iron_ReturnStmt *)cur)->value); break;
            case IRON_NODE_IF: {
                Iron_IfStmt *s = (Iron_IfStmt *)cur;
                PUSH(s->condition); PUSH(s->body);
                for (int i = 0; i < s->elif_count; i++) {
                    PUSH(s->elif_conds[i]); PUSH(s->elif_bodies[i]);
                }
                PUSH(s->else_body); break;
            }
            case IRON_NODE_WHILE: {
                Iron_WhileStmt *w = (Iron_WhileStmt *)cur;
                PUSH(w->condition); PUSH(w->body); break;
            }
            case IRON_NODE_FOR: {
                Iron_ForStmt *f = (Iron_ForStmt *)cur;
                PUSH(f->iterable); PUSH(f->body); break;
            }
            case IRON_NODE_VAL_DECL: PUSH(((Iron_ValDecl *)cur)->init); break;
            case IRON_NODE_VAR_DECL: PUSH(((Iron_VarDecl *)cur)->init); break;
            case IRON_NODE_MATCH: {
                Iron_MatchStmt *m = (Iron_MatchStmt *)cur;
                PUSH(m->subject);
                for (int i = 0; i < m->case_count; i++) PUSH(m->cases[i]);
                PUSH(m->else_body); break;
            }
            case IRON_NODE_MATCH_CASE: PUSH(((Iron_MatchCase *)cur)->body); break;
            case IRON_NODE_DEFER: PUSH(((Iron_DeferStmt *)cur)->expr); break;
            case IRON_NODE_FREE:  PUSH(((Iron_FreeStmt  *)cur)->expr); break;
            case IRON_NODE_LEAK:  PUSH(((Iron_LeakStmt  *)cur)->expr); break;
            case IRON_NODE_SPAWN: {
                Iron_SpawnStmt *d = (Iron_SpawnStmt *)cur;
                PUSH(d->pool_expr); PUSH(d->body); break;
            }
            case IRON_NODE_INTERP_STRING: {
                Iron_InterpString *is = (Iron_InterpString *)cur;
                for (int i = 0; i < is->part_count; i++) PUSH(is->parts[i]);
                break;
            }
            default: break;
        }
    }
#undef PUSH
gather_done:
    free(stack);
}

/* Compare files ASC by URI string. */
static int fileedit_cmp_asc(const void *a, const void *b) {
    const IronLsp_RenameFileEdit *fa = (const IronLsp_RenameFileEdit *)a;
    const IronLsp_RenameFileEdit *fb = (const IronLsp_RenameFileEdit *)b;
    const char *ua = fa->uri ? fa->uri : "";
    const char *ub = fb->uri ? fb->uri : "";
    return strcmp(ua, ub);
}

/* ── Public API ──────────────────────────────────────────────────── */

void ilsp_facade_rename(IronLsp_Server        *server,
                          IronLsp_Document      *doc,
                          IronLsp_Position       pos,
                          const char            *new_name,
                          _Atomic bool          *cancel,
                          Iron_Arena            *arena,
                          IronLsp_RenameResult  *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->outcome = ILSP_RENAME_SUCCESS;
    out->use_document_changes = server
        ? server->client_supports_document_changes : false;
    if (!doc || !arena) {
        out->outcome = ILSP_RENAME_SUCCESS;
        return;
    }

    IronLsp_PositionEncoding enc = server
        ? server->position_encoding : ILSP_ENC_UTF8;

    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &walk_diags);
    if (!program) goto done;
    if (cancel && atomic_load(cancel)) {
        out->outcome = ILSP_RENAME_FAIL_CANCELLED; goto done;
    }

    /* Step 2: Resolve cursor -> Iron_Symbol. */
    Iron_Node *ident = NULL;
    const Iron_Symbol *sym = ident_at_cursor(doc, program, pos, enc, &ident);
    if (!sym) goto done;          /* graceful: empty WorkspaceEdit */

    /* Step 3: Same-name short-circuit (D-10). */
    const char *old_name = sym->name ? sym->name : "";
    if (new_name && strcmp(new_name, old_name) == 0) {
        out->outcome = ILSP_RENAME_SUCCESS;
        out->files   = NULL;
        out->files_n = 0;
        goto done;
    }

    /* Step 4: Bulk-analyze gate (Phase 3 precedent from references.c).
     * TODO(phase-7): emit $/progress begin/report/end when the client
     * supports WorkDoneProgress per RESEARCH §EDIT-11. For v1 we
     * block the request; cold-workspace latency is ~10s at worst. */
    IronLsp_WorkspaceIndex *wi = server ? server->workspace_index : NULL;
    if (wi && !wi->bulk_analyze_done) {
        ilsp_workspace_index_bulk_analyze_for_refs(wi, cancel);
        if (cancel && atomic_load(cancel)) {
            out->outcome = ILSP_RENAME_FAIL_CANCELLED; goto done;
        }
    }

    /* Step 5: Identity triple. */
    const char *decl_path = (sym->decl_node && sym->decl_node->span.filename)
        ? sym->decl_node->span.filename
        : (doc->uri ? doc->uri : "");
    IronLsp_SymbolId triple = ilsp_symbol_id_derive(
        sym, decl_path, program, &walk_arena);
    if (triple.hash == 0) goto done;

    /* Ensure the open doc's contributions are in the refs index if it
     * has a workspace entry (same as references.c does). */
    if (wi && doc->uri) {
        IronLsp_IndexEntry *self_entry = ilsp_workspace_index_lookup(wi, doc->uri);
        if (self_entry) {
            (void)ilsp_workspace_index_analyze_lazy(wi, self_entry, cancel);
        }
    }

    /* Step 6: PITFALL B — interface-method rename must reject if any
     * implementor lives in stdlib/dep. Detection: sym->sym_kind ==
     * IRON_SYM_METHOD AND decl_node is inside an Iron_InterfaceDecl.
     * The iface_workspace registry is keyed on the INTERFACE's triple,
     * not the method's triple, so we first need to locate the owning
     * interface. For v1 we walk the open program looking for a method
     * decl pointer match. */
    if (sym->sym_kind == IRON_SYM_METHOD && sym->decl_node &&
        sym->decl_node->kind == IRON_NODE_METHOD_DECL) {
        /* Find an interface whose method_sigs[] contains sym->decl_node. */
        Iron_InterfaceDecl *iface = NULL;
        for (int i = 0; i < program->decl_count; i++) {
            Iron_Node *d = program->decls[i];
            if (!d || d->kind != IRON_NODE_INTERFACE_DECL) continue;
            Iron_InterfaceDecl *ifc = (Iron_InterfaceDecl *)d;
            for (int j = 0; j < ifc->method_count; j++) {
                if (ifc->method_sigs[j] == sym->decl_node) {
                    iface = ifc; break;
                }
            }
            if (iface) break;
        }
        if (iface && wi) {
            /* Build the interface's identity triple. */
            IronLsp_IfaceWorkspace *iws = ilsp_workspace_index_iface_ws(wi);
            if (iws) {
                /* Synthesize an interface symbol for triple derivation. */
                Iron_Symbol iface_sym = {0};
                iface_sym.name      = iface->name;
                iface_sym.sym_kind  = IRON_SYM_INTERFACE;
                iface_sym.decl_node = (struct Iron_Node *)iface;
                iface_sym.span      = iface->span;
                const char *iface_path = iface->span.filename
                    ? iface->span.filename : decl_path;
                IronLsp_SymbolId iface_triple = ilsp_symbol_id_derive(
                    &iface_sym, iface_path, program, &walk_arena);

                IronLsp_ImplEntry *impls = NULL;
                size_t impls_n = 0;
                ilsp_iface_ws_query_implementors(
                    iws, iface_triple, &walk_arena, &impls, &impls_n);

                const char *method_name = sym->name ? sym->name : "";
                for (size_t i = 0; i < impls_n; i++) {
                    /* PITFALL B contract: reject when ANY implementor
                     * lives in stdlib:// or dep://. Method-name
                     * filtering within the implementor would require
                     * stb_ds arrlen on the methods array; for v1 we
                     * keep the conservative fan-out guard: any stdlib
                     * or dep implementor at all aborts the rename.
                     * This is the safe over-approximation; Phase 7
                     * may tighten it to method-name exact match. */
                    const char *ip = impls[i].canonical_path;
                    if (path_is_stdlib(ip)) {
                        out->outcome = ILSP_RENAME_FAIL_STDLIB_IMPLEMENTOR;
                        out->fail_location = arena_printf(
                            arena, "%s:%u:%u",
                            ip ? ip : "",
                            impls[i].object_decl_span.line,
                            impls[i].object_decl_span.col);
                        out->fail_message = arena_printf(
                            arena,
                            "rename would affect stdlib implementor of %s.%s at %s; stdlib is read-only",
                            iface->name ? iface->name : "",
                            method_name,
                            out->fail_location);
                        goto done;
                    }
                    if (path_is_dep(ip)) {
                        out->outcome = ILSP_RENAME_FAIL_DEP_IMPLEMENTOR;
                        out->fail_location = arena_printf(
                            arena, "%s:%u:%u",
                            ip ? ip : "",
                            impls[i].object_decl_span.line,
                            impls[i].object_decl_span.col);
                        out->fail_message = arena_printf(
                            arena,
                            "rename would affect dep implementor of %s.%s at %s; deps are read-only",
                            iface->name ? iface->name : "",
                            method_name,
                            out->fail_location);
                        goto done;
                    }
                }
            }
        }
    }

    /* Step 7: query workspace-wide use-sites. D-09 filter
     * (stdlib/dep) is applied by ilsp_refs_query. */
    IronLsp_RefSite *workspace_sites = NULL;
    size_t workspace_n = 0;
    if (wi) {
        ilsp_refs_query(wi, triple, arena, enc,
                          &workspace_sites, &workspace_n);
    }

    /* Step 8: open-doc fallback if workspace index doesn't cover the
     * open doc (single-file scenario). */
    SiteRaw *raw = NULL;
    size_t   raw_n = 0, raw_cap = 0;
    if (workspace_n == 0) {
        gather_open_doc_sites(program, sym->decl_node, doc, enc,
                                 arena, &raw, &raw_n, &raw_cap);
    } else {
        /* Translate RefSite -> SiteRaw. */
        raw = (SiteRaw *)iron_arena_alloc(
            arena, workspace_n * sizeof(*raw), _Alignof(SiteRaw));
        if (raw) {
            for (size_t i = 0; i < workspace_n; i++) {
                raw[i].uri      = workspace_sites[i].uri;
                raw[i].range    = workspace_sites[i].range;
                raw[i].sort_key = range_sort_key(workspace_sites[i].range);
            }
            raw_n = workspace_n;
        }
    }

    /* Step 9: append decl site so the decl gets rewritten too. */
    SiteRaw decl_site;
    if (build_decl_site(sym, doc, enc, arena, wi, &decl_site)) {
        /* Avoid duplicate decl+use-site pairs by skipping if an
         * existing raw entry has matching URI + range. */
        bool already = false;
        for (size_t i = 0; i < raw_n; i++) {
            if (raw[i].sort_key == decl_site.sort_key &&
                raw[i].uri && decl_site.uri &&
                strcmp(raw[i].uri, decl_site.uri) == 0) {
                already = true; break;
            }
        }
        if (!already) {
            /* Grow raw by 1. */
            SiteRaw *nr = (SiteRaw *)iron_arena_alloc(
                arena, (raw_n + 1) * sizeof(*nr), _Alignof(SiteRaw));
            if (nr) {
                if (raw_n > 0) memcpy(nr, raw, raw_n * sizeof(*nr));
                nr[raw_n] = decl_site;
                raw = nr;
                raw_n++;
            }
        }
    }

    /* Step 10: collision pre-flight. Full scope-at-cursor per
     * use-site requires a scope_at_cursor helper for closed files;
     * that lands in Phase 7 (RESEARCH §EDIT-12). For v1 we run the
     * collision check with NULL scopes — same-name, empty-name, and
     * keyword guards still fire, covering the D-10 minimum surface.
     * Cross-scope shadow detection is the Phase 7 deliverable. */
    {
        IronLsp_CollisionResult coll;
        memset(&coll, 0, sizeof(coll));
        ilsp_rename_collision_check(wi, sym, old_name, new_name,
                                       NULL, 0, arena, &coll);
        if (coll.kind != ILSP_COLLISION_NONE &&
            coll.kind != ILSP_COLLISION_SAME_NAME) {
            out->outcome   = ILSP_RENAME_FAIL_COLLISION;
            out->collision = coll;
            goto done;
        }
    }

    if (raw_n == 0) {
        /* Nothing to edit — emit success with empty files. */
        out->outcome = ILSP_RENAME_SUCCESS;
        out->files   = NULL;
        out->files_n = 0;
        goto done;
    }

    /* Step 11: group raw sites by URI. Simple approach: sort raw by
     * (uri, DESC-offset) then emit consecutive runs per file. */
    /* First, sort by URI ASC using string compare. Within ties, we
     * want DESC by offset. Combine via two-pass: group-by-URI then
     * sort each group DESC. */

    /* Build a list of unique URIs in raw[]. */
    const char **uris = (const char **)iron_arena_alloc(
        arena, raw_n * sizeof(*uris), _Alignof(const char *));
    size_t uris_n = 0;
    for (size_t i = 0; i < raw_n; i++) {
        const char *u = raw[i].uri ? raw[i].uri : "";
        bool seen = false;
        for (size_t j = 0; j < uris_n; j++) {
            if (strcmp(uris[j], u) == 0) { seen = true; break; }
        }
        if (!seen) { uris[uris_n++] = u; }
    }

    /* Allocate the final per-file edit array. */
    IronLsp_RenameFileEdit *files = (IronLsp_RenameFileEdit *)iron_arena_alloc(
        arena, uris_n * sizeof(*files), _Alignof(IronLsp_RenameFileEdit));
    if (!files) goto done;
    memset(files, 0, uris_n * sizeof(*files));

    /* For each unique URI, extract + DESC-sort its site list. */
    for (size_t f = 0; f < uris_n; f++) {
        const char *this_uri = uris[f];
        /* Count and collect. */
        size_t count = 0;
        for (size_t i = 0; i < raw_n; i++) {
            const char *ru = raw[i].uri ? raw[i].uri : "";
            if (strcmp(ru, this_uri) == 0) count++;
        }
        SiteRaw *grp = (SiteRaw *)iron_arena_alloc(
            arena, count * sizeof(*grp), _Alignof(SiteRaw));
        if (!grp) continue;
        size_t w = 0;
        for (size_t i = 0; i < raw_n; i++) {
            const char *ru = raw[i].uri ? raw[i].uri : "";
            if (strcmp(ru, this_uri) == 0) grp[w++] = raw[i];
        }
        qsort(grp, count, sizeof(*grp), siteraw_cmp_desc);

        files[f].uri     = iron_arena_strdup(arena, this_uri, strlen(this_uri));
        if (!files[f].uri) files[f].uri = "";
        /* Version: match if it's the open doc, else -1 (closed). */
        files[f].version = (doc->uri && strcmp(this_uri, doc->uri) == 0)
            ? doc->version : -1;
        files[f].edits = (IronLsp_RenameTextEdit *)iron_arena_alloc(
            arena, count * sizeof(*files[f].edits), _Alignof(IronLsp_RenameTextEdit));
        if (!files[f].edits) continue;
        const char *nm_owned = iron_arena_strdup(
            arena, new_name ? new_name : "", new_name ? strlen(new_name) : 0);
        if (!nm_owned) nm_owned = "";
        for (size_t i = 0; i < count; i++) {
            files[f].edits[i].range    = grp[i].range;
            files[f].edits[i].new_text = nm_owned;
        }
        files[f].edits_n = count;
    }

    /* Step 12: sort files ASC by URI. */
    qsort(files, uris_n, sizeof(*files), fileedit_cmp_asc);

    out->outcome = ILSP_RENAME_SUCCESS;
    out->files   = files;
    out->files_n = uris_n;

done:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
