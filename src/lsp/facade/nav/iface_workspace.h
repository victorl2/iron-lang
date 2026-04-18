#ifndef IRON_LSP_FACADE_NAV_IFACE_WORKSPACE_H
#define IRON_LSP_FACADE_NAV_IFACE_WORKSPACE_H

/* Phase 3 Plan 05 Task 01 (NAV-05, D-06, D-07, Pitfall 6) --
 * workspace-level interface implementor registry.
 *
 * Purpose: Iron is interface-based.  The existing per-compile
 * `Iron_IfaceRegistry` (src/analyzer/iface_collect.h) is scoped to a
 * single `Iron_Program`.  An LSP needs workspace-wide visibility so
 * queries like "all implementors of Shape" span every file.
 *
 * Strategy (per D-07): after each per-file analyze, harvest the
 * per-program `Iron_IfaceRegistry` into a workspace-level shmap
 * keyed on the identity triple hash of the interface decl.  On
 * invalidation, drop ONLY that entry's contributions before re-
 * populating.  This mirrors the Plan 04 `references_index` drop-
 * before-populate discipline (Pitfall 6 defense).
 *
 * The per-entry contribution tracking lives in a parallel map inside
 * this TU so IronLsp_IndexEntry stays unchanged (minimises cross-
 * module coupling).  Keyed on the entry's canonical_path string, the
 * value is a stb_ds array of iface-triple hashes the entry has
 * contributed to.
 *
 * Zero changes to src/analyzer/iface_collect.c: we consume its
 * existing per-program output unchanged. */

#include "lsp/facade/nav/symbol_id.h"
#include "lsp/facade/types.h"
#include "diagnostics/diagnostics.h"   /* Iron_Span */
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_WorkspaceIndex;
struct IronLsp_IndexEntry;
struct IronLsp_Server;
struct IronLsp_Document;
struct IronLsp_LocationLink;

/* One implementor of an interface recorded at workspace scope. */
typedef struct IronLsp_ImplEntry {
    IronLsp_SymbolId  object_triple;        /* implementing object's triple */
    const char       *canonical_path;       /* owning entry's path (borrowed) */
    const char       *object_name;          /* strdup'd into iws arena; for UX */
    Iron_Span         object_decl_span;     /* full object decl span */
    Iron_Span         object_ident_span;    /* object name ident span (fallback==decl) */
    Iron_Span         impl_clause_span;     /* `implements X` clause span (fallback==decl) */
    /* Per-method impl sites: the object's method decls keyed by method
     * name.  Interface method-sig implementation lookup (D-06 Case B)
     * walks these at query time.  stb_ds dynamic array; arena-owned. */
    struct IronLsp_ImplMethod *methods;     /* stb_ds */
} IronLsp_ImplEntry;

typedef struct IronLsp_ImplMethod {
    const char  *name;              /* arena-owned method name */
    Iron_Span    sig_span;          /* method decl span in the object */
    Iron_Span    ident_span;        /* method name ident span (fallback==sig) */
} IronLsp_ImplMethod;

/* Opaque workspace-wide iface registry. */
typedef struct IronLsp_IfaceWorkspace IronLsp_IfaceWorkspace;

/* Lifecycle. */
IronLsp_IfaceWorkspace *ilsp_iface_ws_create(void);
void                    ilsp_iface_ws_destroy(IronLsp_IfaceWorkspace *iws);

/* Harvest the per-compile registry for `entry` into `iws`.  ALWAYS
 * calls ilsp_iface_ws_drop_for_entry first, so stale contributions
 * from any prior analyze are removed before new ones are added
 * (Pitfall 6).  Tolerates entries with no program (no-op). */
void ilsp_iface_ws_populate_for_entry(IronLsp_IfaceWorkspace        *iws,
                                        struct IronLsp_IndexEntry     *entry);

/* Drop every impl entry contributed by this entry, keyed on
 * entry->canonical_path.  Safe on an entry that has never
 * contributed. */
void ilsp_iface_ws_drop_for_entry(IronLsp_IfaceWorkspace        *iws,
                                    struct IronLsp_IndexEntry     *entry);

/* Query: all implementors of the iface identified by `triple`.
 * Returns array allocated in `arena` via a defensive copy; each
 * IronLsp_ImplEntry is a shallow copy of the stored record (strings
 * and method arrays are borrowed from iws's arena, which outlives
 * the query).  Returns empty (*out=NULL, *out_n=0) on miss. */
void ilsp_iface_ws_query_implementors(IronLsp_IfaceWorkspace        *iws,
                                        IronLsp_SymbolId                triple,
                                        Iron_Arena                    *arena,
                                        IronLsp_ImplEntry            **out,
                                        size_t                        *out_n);

/* Introspection for tests: total number of impl entries across every
 * iface (sum of all arrays in the ws map). */
size_t ilsp_iface_ws_total_impls(IronLsp_IfaceWorkspace *iws);

/* ── textDocument/implementation facade (NAV-05, D-06) ───────────── */

/* Resolve cursor under D-06 cases:
 *   A. Cursor on interface name IDENT that resolves to Iron_InterfaceDecl
 *      -> return LocationLink[] to each implementor's object decl (impl
 *         clause span preferred, falls back to full decl span).
 *   B. Cursor on a method sig identifier inside an interface decl
 *      -> return LocationLink[] to each implementor's matching method
 *         decl span.
 *   C. Cursor on an object name -> empty array (object IS its own
 *      implementation per D-06; fold into definition).
 *   D. Cursor on body expression / other -> empty array.
 * Stdlib + dep impls are included in the result set. */
void ilsp_facade_nav_implementation(struct IronLsp_Server         *server,
                                     struct IronLsp_Document       *doc,
                                     IronLsp_Position               pos,
                                     _Atomic bool                  *cancel,
                                     Iron_Arena                    *arena,
                                     struct IronLsp_LocationLink  **out,
                                     size_t                        *out_n);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_NAV_IFACE_WORKSPACE_H */
