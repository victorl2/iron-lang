#ifndef IRON_LSP_FACADE_NAV_REFERENCES_INDEX_H
#define IRON_LSP_FACADE_NAV_REFERENCES_INDEX_H

/* Phase 3 Plan 04 Task 01 (NAV-06, D-09, Pitfall 6) -- workspace-level
 * reverse reference index.
 *
 * Structure:
 *   workspace -> shmap keyed on FNV-1a 64-bit hash of IronLsp_SymbolId,
 *   value = stb_ds array of IronLsp_RefSpan (use-site spans + owning
 *   canonical_path so cross-file queries survive the per-entry arena
 *   lifetime).
 *
 * Per-entry contribution tracking:
 *   Each IronLsp_IndexEntry carries a separate "contributed triples"
 *   sidemap (stb_ds dynamic array of triple-hash keys) so invalidation
 *   can remove ONLY this entry's contributions without walking every
 *   workspace entry. This is the Pitfall 6 defense: drop-before-populate
 *   is the invariant.
 *
 * Query:
 *   ilsp_refs_query(wi, triple, arena, out) -> array of RefSite with
 *   URI + LSP Range computed against each owning entry's line index.
 *   Stdlib ("stdlib://") and dep ("dep://") use-sites are filtered out
 *   UNCONDITIONALLY per D-09 LOCKED policy. */

#include "lsp/facade/nav/symbol_id.h"
#include "lsp/facade/types.h"
#include "diagnostics/diagnostics.h"      /* Iron_Span */
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_WorkspaceIndex;
struct IronLsp_IndexEntry;

/* A recorded use-site span: the span and the canonical path of the
 * file it lives in. canonical_path is a pointer into the owning
 * IndexEntry's canonical_path (which is heap-allocated for the
 * entry's lifetime). */
typedef struct IronLsp_RefSpan {
    Iron_Span     span;              /* 1-based, byte-column */
    const char   *canonical_path;    /* owning entry's path (borrowed) */
} IronLsp_RefSpan;

/* Public query result: one use-site rendered as {uri, LSP Range}. */
typedef struct IronLsp_RefSite {
    const char    *uri;              /* arena-allocated file:// URI */
    IronLsp_Range  range;            /* 0-based, encoding-aware */
} IronLsp_RefSite;

/* Populate (or re-populate) the reverse-ref contributions made by
 * `entry`. Walks entry->program AST, finds every Iron_Ident with a
 * resolved_sym, derives the triple, and appends the ident span to the
 * workspace map.
 *
 * Idempotent: ALWAYS calls ilsp_refs_drop_for_entry first so stale
 * contributions from a previous analyze are removed before new ones
 * are added. This is the Pitfall 6 defense. */
void ilsp_refs_populate_for_entry(struct IronLsp_WorkspaceIndex *wi,
                                    struct IronLsp_IndexEntry     *entry);

/* Remove all contributions made by `entry` from the workspace map.
 * Safe to call on an entry that has never contributed (no-op).
 * Called by workspace_index invalidation paths before freeing the
 * entry's arena. */
void ilsp_refs_drop_for_entry(struct IronLsp_WorkspaceIndex *wi,
                                struct IronLsp_IndexEntry     *entry);

/* Query the workspace map by identity triple. Returns arena-allocated
 * array of IronLsp_RefSite covering every recorded use-site across the
 * workspace, with stdlib:// / dep:// sites filtered out (D-09 LOCKED).
 * Returns count via *out_n; sets *out_sites=NULL and *out_n=0 on miss
 * or empty result. */
void ilsp_refs_query(struct IronLsp_WorkspaceIndex *wi,
                      IronLsp_SymbolId                triple,
                      Iron_Arena                     *arena,
                      IronLsp_PositionEncoding        enc,
                      IronLsp_RefSite               **out_sites,
                      size_t                         *out_n);

/* Destroy the workspace reverse index. Called by
 * ilsp_workspace_index_destroy before freeing the parent wi struct. */
void ilsp_refs_index_destroy(struct IronLsp_WorkspaceIndex *wi);

/* Introspection for tests: total recorded use-sites across every
 * triple (sum of all span-array lengths in the workspace map). */
size_t ilsp_refs_index_total_sites(struct IronLsp_WorkspaceIndex *wi);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_NAV_REFERENCES_INDEX_H */
