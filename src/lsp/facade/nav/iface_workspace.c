/* Phase 3 Plan 05 Task 01 (NAV-05, D-06, D-07, Pitfall 6) --
 * workspace-level interface implementor registry.
 *
 * Data structures:
 *
 *   struct IronLsp_IfaceWorkspace {
 *     Iron_Arena                 *arena;     // process-lifetime strings
 *     IfaceMapEntry              *ws_map;    // hmput keyed uint64_t ->
 *                                              // stb_ds array of ImplEntry
 *     ContribMapEntry            *contrib;   // shmap keyed canonical_path ->
 *                                              // stb_ds array of uint64_t
 *                                              // (iface-triple hashes)
 *   };
 *
 * Per-entry contribution tracking lives in `contrib` as a parallel
 * shmap -- IronLsp_IndexEntry stays untouched (minimal cross-module
 * coupling, per the plan).
 *
 * Drop-before-populate invariant (Pitfall 6): populate_for_entry
 * always calls drop_for_entry first so stale contributions from any
 * prior analyze vanish before we re-harvest.  Drop tolerates entries
 * with no prior contributions.
 *
 * iface_collect.c stays unchanged.  We call iron_iface_collect via
 * its existing public API and merge the returned per-program registry
 * into the workspace-level map keyed on the interface decl's identity
 * triple hash. */

#include "lsp/facade/nav/iface_workspace.h"

#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/nav/node_at.h"
#include "lsp/facade/nav/symbol_id.h"
#include "lsp/facade/compile.h"
#include "lsp/facade/span.h"
#include "lsp/server/server.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h"
#include "analyzer/iface_collect.h"
#include "analyzer/scope.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal map types ──────────────────────────────────────────── */

typedef struct IfaceMapEntry {
    uint64_t           key;          /* interface triple hash */
    IronLsp_ImplEntry *impls;        /* stb_ds array */
} IfaceMapEntry;

typedef struct ContribMapEntry {
    char     *key;                   /* sh_new_strdup'd canonical_path */
    uint64_t *hashes;                /* stb_ds array of iface-triple hashes */
} ContribMapEntry;

struct IronLsp_IfaceWorkspace {
    Iron_Arena      *arena;          /* heap-owned long-lived arena for
                                       * interned name strings that must
                                       * outlive entry arenas */
    IfaceMapEntry   *ws_map;         /* hmput keyed on uint64_t hash */
    ContribMapEntry *contrib;        /* shmap keyed on canonical_path */
};

/* ── Lifecycle ───────────────────────────────────────────────────── */

IronLsp_IfaceWorkspace *ilsp_iface_ws_create(void) {
    IronLsp_IfaceWorkspace *iws =
        (IronLsp_IfaceWorkspace *)calloc(1, sizeof(*iws));
    if (!iws) return NULL;
    iws->arena = (Iron_Arena *)malloc(sizeof(Iron_Arena));
    if (!iws->arena) {
        free(iws);
        return NULL;
    }
    *iws->arena = iron_arena_create(64 * 1024);
    iws->ws_map = NULL;
    iws->contrib = NULL;
    sh_new_strdup(iws->contrib);
    return iws;
}

void ilsp_iface_ws_destroy(IronLsp_IfaceWorkspace *iws) {
    if (!iws) return;
    if (iws->ws_map) {
        ptrdiff_t n = hmlen(iws->ws_map);
        for (ptrdiff_t i = 0; i < n; i++) {
            ptrdiff_t m = arrlen(iws->ws_map[i].impls);
            for (ptrdiff_t j = 0; j < m; j++) {
                arrfree(iws->ws_map[i].impls[j].methods);
            }
            arrfree(iws->ws_map[i].impls);
        }
        hmfree(iws->ws_map);
    }
    if (iws->contrib) {
        ptrdiff_t n = shlen(iws->contrib);
        for (ptrdiff_t i = 0; i < n; i++) {
            arrfree(iws->contrib[i].hashes);
        }
        shfree(iws->contrib);
    }
    if (iws->arena) {
        iron_arena_free(iws->arena);
        free(iws->arena);
    }
    free(iws);
}

/* ── Helpers ─────────────────────────────────────────────────────── */

static IfaceMapEntry *ws_get(IronLsp_IfaceWorkspace *iws, uint64_t key) {
    if (!iws || !iws->ws_map) return NULL;
    ptrdiff_t idx = hmgeti(iws->ws_map, key);
    if (idx < 0) return NULL;
    return &iws->ws_map[idx];
}

static IfaceMapEntry *ws_get_or_create(IronLsp_IfaceWorkspace *iws,
                                         uint64_t key) {
    IfaceMapEntry *e = ws_get(iws, key);
    if (e) return e;
    IfaceMapEntry nent = { .key = key, .impls = NULL };
    hmputs(iws->ws_map, nent);
    return ws_get(iws, key);
}

static ContribMapEntry *contrib_get(IronLsp_IfaceWorkspace *iws,
                                     const char *path) {
    if (!iws || !iws->contrib || !path) return NULL;
    ptrdiff_t idx = shgeti(iws->contrib, path);
    if (idx < 0) return NULL;
    return &iws->contrib[idx];
}

static ContribMapEntry *contrib_get_or_create(IronLsp_IfaceWorkspace *iws,
                                                const char *path) {
    ContribMapEntry *e = contrib_get(iws, path);
    if (e) return e;
    /* sh_new_strdup duplicates the key on insert; the struct-layout
     * variant (shputs) expects a valid key in the struct so stb_ds
     * can strdup it.  Provide the path string here. */
    ContribMapEntry nent = { .key = (char *)path, .hashes = NULL };
    shputs(iws->contrib, nent);
    return contrib_get(iws, path);
}

/* Build an IronLsp_SymbolId for an interface decl or object decl by
 * fabricating a symbol-like triple.  We don't need an actual
 * Iron_Symbol here -- the canonical_path + decl name + kind are
 * sufficient to key the workspace map.  This is the same shape
 * ilsp_symbol_id_derive produces for a "module.Name" decl when
 * there's no owner. */
static IronLsp_SymbolId triple_for_decl(const char      *canonical_path,
                                          const char      *decl_name,
                                          Iron_SymbolKind  kind,
                                          Iron_Arena      *arena) {
    /* Fabricate a temporary Iron_Symbol -- we only read .name, .decl_node,
     * .sym_kind (the last two are trivial here). */
    Iron_Symbol fake = {0};
    fake.name = decl_name;
    fake.sym_kind = kind;
    fake.decl_node = NULL;  /* find_owner lookups will no-op -> module.Name */
    return ilsp_symbol_id_derive(&fake, canonical_path, NULL, arena);
}

/* ── Drop + populate ─────────────────────────────────────────────── */

void ilsp_iface_ws_drop_for_entry(IronLsp_IfaceWorkspace *iws,
                                    IronLsp_IndexEntry     *entry) {
    if (!iws || !entry || !entry->canonical_path) return;
    ContribMapEntry *ce = contrib_get(iws, entry->canonical_path);
    if (!ce) return;

    /* For each iface-triple hash this entry contributed to, filter
     * the workspace map's impl array in place, dropping entries whose
     * canonical_path matches this entry's. */
    ptrdiff_t nh = arrlen(ce->hashes);
    for (ptrdiff_t i = 0; i < nh; i++) {
        uint64_t h = ce->hashes[i];
        IfaceMapEntry *we = ws_get(iws, h);
        if (!we || !we->impls) continue;
        ptrdiff_t n = arrlen(we->impls);
        ptrdiff_t w = 0;
        for (ptrdiff_t r = 0; r < n; r++) {
            const char *ep = we->impls[r].canonical_path;
            if (ep && entry->canonical_path &&
                strcmp(ep, entry->canonical_path) == 0) {
                /* drop: free the impl's method stb_ds array first */
                arrfree(we->impls[r].methods);
                continue;
            }
            if (w != r) we->impls[w] = we->impls[r];
            w++;
        }
        while (arrlen(we->impls) > w) (void)arrpop(we->impls);
    }
    arrfree(ce->hashes);
    ce->hashes = NULL;
}

/* Derive the Iron_Span for the `implements X` clause within an
 * object decl.  The AST doesn't expose a distinct impl-clause span
 * today, so we fall back to the full decl span per D-06 (documented
 * fallback in the plan).  If the AST ever gains the span, swap the
 * fallback here. */
static Iron_Span impl_clause_span_for(const Iron_ObjectDecl *od) {
    if (!od) { Iron_Span z = {0}; return z; }
    return od->span;
}

/* Locate the object's method decl whose method_name matches `name`.
 * Iron methods are NOT stored inside the object decl's fields[] --
 * they live as top-level Iron_MethodDecl nodes keyed on type_name.
 * Walk the enclosing program to find them. */
static void collect_impl_methods(IronLsp_IfaceWorkspace *iws,
                                   IronLsp_ImplEntry      *impl,
                                   const Iron_Program     *program,
                                   const char             *type_name) {
    if (!program || !type_name) return;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *m = (Iron_MethodDecl *)d;
        if (!m->type_name || strcmp(m->type_name, type_name) != 0) continue;

        IronLsp_ImplMethod im = {0};
        if (m->method_name) {
            im.name = iron_arena_strdup(iws->arena, m->method_name,
                                         strlen(m->method_name));
        } else {
            im.name = "";
        }
        im.sig_span   = m->span;
        im.ident_span = m->span;  /* AST doesn't expose ident-only span today */
        arrput(impl->methods, im);
    }
}

void ilsp_iface_ws_populate_for_entry(IronLsp_IfaceWorkspace *iws,
                                        IronLsp_IndexEntry     *entry) {
    if (!iws || !entry || !entry->program || !entry->canonical_path) return;

    /* Pitfall 6: drop BEFORE populate.  UNCONDITIONAL. */
    ilsp_iface_ws_drop_for_entry(iws, entry);

    Iron_Program *p = entry->program;

    /* Run the per-compile registry through the analyzer helper.
     * Arena is the ENTRY's arena so map allocations are scoped to
     * this entry's lifetime (the registry keys are freed on arena
     * free; we copy the relevant data into iws->arena for long-
     * lived storage). */
    Iron_IfaceRegistry reg = iron_iface_collect(p, entry->arena);

    /* Need contrib map slot for this entry regardless of registry
     * size (an empty file still gets a zero-length contribution
     * tracker so future drops are well-defined). */
    ContribMapEntry *ce = contrib_get_or_create(iws, entry->canonical_path);
    if (!ce) return;

    ptrdiff_t nifc = shlen(reg.map);
    for (ptrdiff_t i = 0; i < nifc; i++) {
        Iron_IfaceEntry *ie = &reg.map[i].value;
        if (!ie->iface_name || !ie->iface_decl) continue;

        /* Derive the iface triple.  The interface's canonical_path
         * is the ENTRY's canonical_path -- Iron interfaces are
         * defined in exactly one file. */
        IronLsp_SymbolId ifc_triple = triple_for_decl(
            entry->canonical_path, ie->iface_name,
            IRON_SYM_TYPE, iws->arena);

        IfaceMapEntry *we = ws_get_or_create(iws, ifc_triple.hash);
        if (!we) continue;

        /* Track that this entry contributes to this iface triple. */
        bool already_tracked = false;
        ptrdiff_t cn = arrlen(ce->hashes);
        for (ptrdiff_t k = 0; k < cn; k++) {
            if (ce->hashes[k] == ifc_triple.hash) {
                already_tracked = true; break;
            }
        }
        if (!already_tracked) arrput(ce->hashes, ifc_triple.hash);

        /* Append each implementor. */
        for (int j = 0; j < ie->impl_count; j++) {
            Iron_IfaceImpl *impl = &ie->impls[j];
            Iron_ObjectDecl *od = impl->decl;
            if (!od) continue;

            IronLsp_ImplEntry ne = {0};
            ne.canonical_path = entry->canonical_path;
            if (od->name) {
                ne.object_name = iron_arena_strdup(
                    iws->arena, od->name, strlen(od->name));
            } else {
                ne.object_name = "";
            }
            ne.object_decl_span   = od->span;
            ne.object_ident_span  = od->span;
            ne.impl_clause_span   = impl_clause_span_for(od);
            ne.object_triple = triple_for_decl(
                entry->canonical_path,
                od->name ? od->name : "",
                IRON_SYM_TYPE, iws->arena);
            ne.methods = NULL;

            /* Collect per-method spans for D-06 Case B. */
            collect_impl_methods(iws, &ne, p,
                                  od->name ? od->name : "");

            arrput(we->impls, ne);
        }
    }
}

/* ── Query ───────────────────────────────────────────────────────── */

void ilsp_iface_ws_query_implementors(IronLsp_IfaceWorkspace *iws,
                                        IronLsp_SymbolId         triple,
                                        Iron_Arena              *arena,
                                        IronLsp_ImplEntry      **out,
                                        size_t                  *out_n) {
    if (out) *out = NULL;
    if (out_n) *out_n = 0;
    if (!iws || !arena || !out || !out_n) return;
    IfaceMapEntry *we = ws_get(iws, triple.hash);
    if (!we || !we->impls) return;
    ptrdiff_t n = arrlen(we->impls);
    if (n <= 0) return;
    IronLsp_ImplEntry *arr = (IronLsp_ImplEntry *)iron_arena_alloc(
        arena, (size_t)n * sizeof(*arr), _Alignof(IronLsp_ImplEntry));
    if (!arr) return;
    for (ptrdiff_t i = 0; i < n; i++) {
        arr[i] = we->impls[i];
        /* methods stb_ds array is borrowed -- lives in iws's storage */
    }
    *out = arr;
    *out_n = (size_t)n;
}

size_t ilsp_iface_ws_total_impls(IronLsp_IfaceWorkspace *iws) {
    if (!iws || !iws->ws_map) return 0;
    size_t total = 0;
    ptrdiff_t n = hmlen(iws->ws_map);
    for (ptrdiff_t i = 0; i < n; i++) {
        total += (size_t)arrlen(iws->ws_map[i].impls);
    }
    return total;
}
