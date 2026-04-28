/* Phase 3 Plan 05 Task 01 (NAV-05, D-06) -- textDocument/implementation
 * facade.
 *
 * Three D-06 cases supported:
 *
 *   A. Cursor on interface name IDENT (or the Iron_InterfaceDecl
 *      itself when node_at lands on the decl for whole-name hit):
 *      -> LocationLink[] to every implementor's object decl.
 *
 *   B. Cursor on a method-sig identifier inside an interface decl:
 *      -> LocationLink[] to each implementor's matching method decl.
 *
 *   C. Cursor on an object name: return empty array (object IS its
 *      own implementation per D-06; fold into definition instead).
 *
 *   D. Cursor on method body / expression / other: empty.
 *
 * Stdlib and dep implementors are included in results (D-06 LOCKED). */

#include "lsp/facade/nav/iface_workspace.h"
#include "lsp/facade/nav/nav_core.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/nav/node_at.h"
#include "lsp/facade/nav/symbol_id.h"
#include "lsp/facade/nav/visibility.h"
#include "lsp/facade/compile.h"
#include "lsp/facade/span.h"
#include "lsp/server/server.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h"
#include "analyzer/scope.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Server-level accessor for the iface workspace aggregator.  Declared
 * here to avoid a circular include between workspace_index.h and
 * iface_workspace.h; the actual definition lives in workspace_index.c. */
IronLsp_IfaceWorkspace *ilsp_workspace_index_iface_ws(
    IronLsp_WorkspaceIndex *wi);

/* ── Cursor classification ───────────────────────────────────────── */

/* If the cursor landed on the Iron_InterfaceDecl itself, this is Case A.
 * If it landed on a method-sig node inside an interface's method_sigs[],
 * find the enclosing interface by pointer search. */
static const Iron_InterfaceDecl *classify(const Iron_Program *program,
                                            Iron_Node           *node,
                                            Iron_Node          **out_sig,
                                            const char         **out_method_name) {
    if (out_sig) *out_sig = NULL;
    if (out_method_name) *out_method_name = NULL;
    if (!node) return NULL;

    if (node->kind == IRON_NODE_INTERFACE_DECL) {
        return (const Iron_InterfaceDecl *)node;
    }

    /* Check whether node is a method sig inside an interface decl. */
    if (node->kind == IRON_NODE_METHOD_DECL ||
        node->kind == IRON_NODE_FUNC_DECL) {
        if (!program) return NULL;
        for (int i = 0; i < program->decl_count; i++) {
            Iron_Node *d = program->decls[i];
            if (!d || d->kind != IRON_NODE_INTERFACE_DECL) continue;
            Iron_InterfaceDecl *ifc = (Iron_InterfaceDecl *)d;
            for (int j = 0; j < ifc->method_count; j++) {
                if (ifc->method_sigs[j] == node) {
                    if (out_sig) *out_sig = node;
                    if (out_method_name) {
                        if (node->kind == IRON_NODE_METHOD_DECL) {
                            *out_method_name =
                                ((const Iron_MethodDecl *)node)->method_name;
                        } else {
                            *out_method_name =
                                ((const Iron_FuncDecl *)node)->name;
                        }
                    }
                    return ifc;
                }
            }
        }
    }
    return NULL;
}

/* ── LocationLink builders ───────────────────────────────────────── */

static IronLsp_LocationLink build_impl_link(
    const IronLsp_ImplEntry        *impl,
    IronLsp_Document               *origin_doc,
    Iron_Span                       origin_span,
    bool                            use_impl_clause,
    IronLsp_WorkspaceIndex         *wi,
    IronLsp_PositionEncoding        enc,
    Iron_Arena                     *arena) {
    IronLsp_LocationLink L;
    memset(&L, 0, sizeof(L));
    L.origin_selection_range = ilsp_span_to_lsp_range(origin_span, origin_doc, enc);

    Iron_Span range_span = use_impl_clause
        ? impl->impl_clause_span : impl->object_decl_span;
    Iron_Span sel_span = impl->object_ident_span;

    IronLsp_IndexEntry *entry = NULL;
    if (wi && impl->canonical_path) {
        entry = ilsp_workspace_index_lookup(wi, impl->canonical_path);
    }
    if (entry) {
        L.target_range           = ilsp_nav_entry_span_to_range(entry, range_span, enc);
        L.target_selection_range = ilsp_nav_entry_span_to_range(entry, sel_span, enc);
        L.target_uri = ilsp_nav_path_to_uri(impl->canonical_path, arena);
    } else if (origin_doc && origin_doc->uri && impl->canonical_path &&
                strcmp(impl->canonical_path, origin_doc->uri) == 0) {
        L.target_range           = ilsp_span_to_lsp_range(range_span, origin_doc, enc);
        L.target_selection_range = ilsp_span_to_lsp_range(sel_span, origin_doc, enc);
        L.target_uri = iron_arena_strdup(arena, origin_doc->uri,
                                          strlen(origin_doc->uri));
    } else {
        IronLsp_Range z = { {0,0}, {0,0} };
        L.target_range = z;
        L.target_selection_range = z;
        L.target_uri = impl->canonical_path
            ? ilsp_nav_path_to_uri(impl->canonical_path, arena)
            : "";
    }
    if (!L.target_uri) L.target_uri = "";
    return L;
}

static IronLsp_LocationLink build_method_link(
    const IronLsp_ImplEntry        *impl,
    const IronLsp_ImplMethod       *method,
    IronLsp_Document               *origin_doc,
    Iron_Span                       origin_span,
    IronLsp_WorkspaceIndex         *wi,
    IronLsp_PositionEncoding        enc,
    Iron_Arena                     *arena) {
    IronLsp_LocationLink L;
    memset(&L, 0, sizeof(L));
    L.origin_selection_range = ilsp_span_to_lsp_range(origin_span, origin_doc, enc);

    IronLsp_IndexEntry *entry = NULL;
    if (wi && impl->canonical_path) {
        entry = ilsp_workspace_index_lookup(wi, impl->canonical_path);
    }
    if (entry) {
        L.target_range           = ilsp_nav_entry_span_to_range(entry, method->sig_span, enc);
        L.target_selection_range = ilsp_nav_entry_span_to_range(entry, method->ident_span, enc);
        L.target_uri = ilsp_nav_path_to_uri(impl->canonical_path, arena);
    } else if (origin_doc && origin_doc->uri && impl->canonical_path &&
                strcmp(impl->canonical_path, origin_doc->uri) == 0) {
        L.target_range           = ilsp_span_to_lsp_range(method->sig_span, origin_doc, enc);
        L.target_selection_range = ilsp_span_to_lsp_range(method->ident_span, origin_doc, enc);
        L.target_uri = iron_arena_strdup(arena, origin_doc->uri,
                                          strlen(origin_doc->uri));
    } else {
        IronLsp_Range z = { {0,0}, {0,0} };
        L.target_range = z;
        L.target_selection_range = z;
        L.target_uri = impl->canonical_path
            ? ilsp_nav_path_to_uri(impl->canonical_path, arena)
            : "";
    }
    if (!L.target_uri) L.target_uri = "";
    return L;
}

/* Derive the iface triple keyed on the open doc's URI (matches what
 * populate_for_entry keyed on -- the entry's canonical_path). */
static IronLsp_SymbolId triple_for_iface_in_doc(const char *canonical_path,
                                                  const char *iface_name,
                                                  Iron_Arena *arena) {
    Iron_Symbol fake = {0};
    fake.name = iface_name;
    fake.sym_kind = IRON_SYM_TYPE;
    fake.decl_node = NULL;
    return ilsp_symbol_id_derive(&fake, canonical_path, NULL, arena);
}

/* ── Public facade entry ─────────────────────────────────────────── */

void ilsp_facade_nav_implementation(IronLsp_Server             *server,
                                     IronLsp_Document           *doc,
                                     IronLsp_Position            pos,
                                     _Atomic bool               *cancel,
                                     Iron_Arena                 *arena,
                                     IronLsp_LocationLink      **out,
                                     size_t                     *out_n) {
    if (out) *out = NULL;
    if (out_n) *out_n = 0;
    if (!server || !doc || !arena || !out || !out_n) return;

    IronLsp_PositionEncoding enc = server->position_encoding;

    /* Analyze the doc via the single nav seam. */
    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &walk_diags);
    if (!program) goto done;
    if (cancel && atomic_load(cancel)) goto done;

    Iron_Node *node = ilsp_nav_node_at(doc, program, pos, enc);
    if (!node) goto done;

    /* Case C: cursor on object name -> empty result per D-06. */
    if (node->kind == IRON_NODE_OBJECT_DECL) goto done;

    Iron_Node *method_sig = NULL;
    const char *method_name = NULL;
    const Iron_InterfaceDecl *ifc = classify(program, node, &method_sig,
                                               &method_name);
    if (!ifc) goto done;  /* Case D: unrelated cursor */

    /* If doc isn't tied to a workspace index, we can still harvest the
     * registry on-the-fly for the open file alone.  Otherwise query
     * the workspace-wide aggregator. */
    IronLsp_IfaceWorkspace *iws = server->workspace_index
        ? ilsp_workspace_index_iface_ws(server->workspace_index)
        : NULL;

    /* Derive iface triple keyed on the open doc's URI. */
    const char *doc_key = doc->uri ? doc->uri : "";
    IronLsp_SymbolId triple = triple_for_iface_in_doc(
        doc_key, ifc->name ? ifc->name : "", arena);

    IronLsp_ImplEntry *impls = NULL;
    size_t impls_n = 0;
    if (iws) {
        ilsp_iface_ws_query_implementors(iws, triple, &walk_arena,
                                           &impls, &impls_n);
    }

    /* Same-file fallback when workspace_index is not available: harvest
     * the per-program registry directly so same-file implementors are
     * still surfaced in unit tests and single-file sessions. */
    if (impls_n == 0) {
        /* Inline per-program harvest: find all objects in `program`
         * that list `ifc->name` in their implements_names[]. */
        int local_count = 0;
        for (int i = 0; i < program->decl_count; i++) {
            Iron_Node *d = program->decls[i];
            if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
            /* XXX_PHASE_11 - patches are not interface implementors in
             * the v2 result class; Phase 11 PATCH-01 surfaces them as a
             * separate result class. Skip is_patch ObjectDecls from the
             * implementor count + emit pass so textDocument/implementation
             * never lists a patch decl as a native implementor of the
             * interface under the cursor. */
            if (od->is_patch) continue;
            /* NEW Phase 10 VIS-03 (RESEARCH Conflict 2): filter each
             * implementor by cross-module visibility. Per-impl gate (NOT
             * per-request) so partial visibility is honored. Stdlib
             * implementors stay (D-08 carve-out flows through
             * ilsp_vis_can_see). NOTE: ObjectDecl has no is_pub axis in
             * v3 (parser drops `private` keyword per parser.c:4047), so
             * the predicate default-trues for ObjectDecl kind today --
             * the gate is functionally a no-op for objects but is wired
             * here so Phase 11 PATCH / Phase 14 MIG don't have to revisit
             * the call site. Same-module short-circuit handles same-file
             * implementors deterministically. */
            const char *od_decl_path =
                (od->span.filename) ? od->span.filename : "";
            const char *od_requester = (doc && doc->uri) ? doc->uri : "";
            if (!ilsp_vis_can_see(od_decl_path, od_requester,
                                   (const Iron_Node *)od)) {
                continue; /* skip this implementor */
            }
            for (int j = 0; j < od->implements_count; j++) {
                if (od->implements_names[j] &&
                    strcmp(od->implements_names[j], ifc->name) == 0) {
                    local_count++;
                    break;
                }
            }
        }
        if (local_count == 0) goto done;

        IronLsp_ImplEntry *local = (IronLsp_ImplEntry *)iron_arena_alloc(
            &walk_arena, (size_t)local_count * sizeof(*local),
            _Alignof(IronLsp_ImplEntry));
        if (!local) goto done;
        size_t li = 0;
        for (int i = 0; i < program->decl_count; i++) {
            Iron_Node *d = program->decls[i];
            if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
            /* XXX_PHASE_11 - patches are not interface implementors in
             * the v2 result class; mirror the count-pass guard above so
             * the emit pass and the count pass agree. Phase 11 PATCH-01
             * replaces both with proper virtual rendering. */
            if (od->is_patch) continue;
            /* NEW Phase 10 VIS-03 (RESEARCH Conflict 2): mirror the
             * count-pass visibility gate so the emit pass and count
             * pass agree on which implementors are kept. */
            const char *od_decl_path =
                (od->span.filename) ? od->span.filename : "";
            const char *od_requester = (doc && doc->uri) ? doc->uri : "";
            if (!ilsp_vis_can_see(od_decl_path, od_requester,
                                   (const Iron_Node *)od)) {
                continue;
            }
            bool matches = false;
            for (int j = 0; j < od->implements_count; j++) {
                if (od->implements_names[j] &&
                    strcmp(od->implements_names[j], ifc->name) == 0) {
                    matches = true; break;
                }
            }
            if (!matches) continue;

            IronLsp_ImplEntry ne = {0};
            ne.canonical_path = doc_key;
            ne.object_name    = od->name ? od->name : "";
            ne.object_decl_span  = od->span;
            ne.object_ident_span = od->span;
            ne.impl_clause_span  = od->span;
            ne.methods = NULL;
            /* Gather this object's methods for Case B. */
            for (int k = 0; k < program->decl_count; k++) {
                Iron_Node *mnode = program->decls[k];
                if (!mnode || mnode->kind != IRON_NODE_METHOD_DECL) continue;
                Iron_MethodDecl *mm = (Iron_MethodDecl *)mnode;
                if (!mm->type_name || !od->name) continue;
                if (strcmp(mm->type_name, od->name) != 0) continue;
                IronLsp_ImplMethod im = {0};
                im.name = mm->method_name ? mm->method_name : "";
                im.sig_span   = mm->span;
                im.ident_span = mm->span;
                arrput(ne.methods, im);
            }
            local[li++] = ne;
        }
        impls   = local;
        impls_n = li;
    }

    if (impls_n == 0) goto done;

    /* Case A: cursor on iface name -> one link per implementor. */
    if (!method_sig) {
        IronLsp_LocationLink *arr = (IronLsp_LocationLink *)iron_arena_alloc(
            arena, impls_n * sizeof(*arr), _Alignof(IronLsp_LocationLink));
        if (!arr) goto done;
        for (size_t i = 0; i < impls_n; i++) {
            arr[i] = build_impl_link(&impls[i], doc, ifc->span,
                                      /*use_impl_clause=*/true,
                                      server->workspace_index, enc, arena);
        }
        *out = arr;
        *out_n = impls_n;
        goto done;
    }

    /* Case B: cursor on method sig -> matching methods across impls. */
    if (!method_name || !*method_name) goto done;

    IronLsp_LocationLink *arr = (IronLsp_LocationLink *)iron_arena_alloc(
        arena, impls_n * sizeof(*arr), _Alignof(IronLsp_LocationLink));
    if (!arr) goto done;
    size_t w = 0;
    for (size_t i = 0; i < impls_n; i++) {
        const IronLsp_ImplEntry *impl = &impls[i];
        if (!impl->methods) continue;
        ptrdiff_t mlen = arrlen(impl->methods);
        for (ptrdiff_t k = 0; k < mlen; k++) {
            if (!impl->methods[k].name) continue;
            if (strcmp(impl->methods[k].name, method_name) != 0) continue;
            arr[w++] = build_method_link(impl, &impl->methods[k],
                                          doc, method_sig->span,
                                          server->workspace_index, enc, arena);
            break;  /* one match per implementor */
        }
    }
    if (w > 0) {
        *out = arr;
        *out_n = w;
    }

done:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
