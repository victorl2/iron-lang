#ifndef IRON_LSP_STORE_WORKSPACE_INDEX_H
#define IRON_LSP_STORE_WORKSPACE_INDEX_H

/* Phase 3 Plan 02 Task 01 (NAV-01, D-01) -- per-file workspace index.
 *
 * Extends the HARD-06 per-request arena discipline to a per-file cache
 * keyed on the canonical (realpath-normalised) absolute POSIX path. Each
 * IronLsp_IndexEntry owns its own Iron_Arena -- invalidation is
 * iron_arena_free + slot drop, no cross-entry pointer leaks possible.
 *
 * Warm-seed (post-`initialized`):
 *   Walk workspace_root recursively via opendir/readdir, excluding
 *   `.git`, `build`, `.iron-build`, `node_modules`, and any
 *   dot-prefixed directory. For each `*.iron` file:
 *     1. realpath-normalise             -> canonical_path (heap-owned)
 *     2. read bytes, compute 64-bit SHA-256 prefix  -> content_hash
 *     3. fresh Iron_Arena (64 KB heap-allocated)
 *     4. iron_lexer_create + iron_lex_all + iron_parser_create + iron_parse
 *        (parse-only; the SINGLE iron_analyze_buffer call site invariant
 *         lives elsewhere, so parse-only never breaches it)
 *     5. ilsp_line_index_rebuild for span -> LSP Range conversion
 *     6. shput into the entries shmap under the coarse mutex.
 *   Cancel flag polled between each file (D-16 iteration-boundary).
 *   Cap at 5000 files (T-03-04 DoS mitigation).
 *
 * Lazy analyze (on-demand):
 *   When a nav endpoint needs resolved symbols for a non-open file,
 *   ilsp_workspace_index_analyze_lazy routes through
 *   ilsp_facade_compile_pure (preserves single iron_analyze_buffer site).
 *   Results are cached per-entry; bumps analyzed_count; if >200, evicts
 *   the LRU entry on the dispatcher thread only (Pitfall 9 defense).
 *
 * Invalidation:
 *   - invalidate_path:  remove one entry (didChangeWatchedFiles SOURCE).
 *   - invalidate_dep:   NULL => cascade to every entry that imports a
 *                       dep; otherwise drop only entries importing the
 *                       named module (Plan 02-03 dep_map wire-up).
 *
 * Thread discipline:
 *   The coarse mutex wraps every read/write of `entries`. Readers hold
 *   it for the duration of a lookup and any arena access. Eviction runs
 *   on the dispatcher thread between requests (no concurrent free).
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "analyzer/analyzer.h"        /* Iron_AnalyzeResult */
#include "diagnostics/diagnostics.h"  /* Iron_DiagList */
#include "parser/ast.h"               /* Iron_Program */
#include "runtime/iron_runtime.h"     /* iron_mutex_t */
#include "lsp/store/line_index.h"     /* IronLsp_LineIndex */
#include "util/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct IronLsp_StdlibCache;
struct IronLsp_DepMap;

typedef struct IronLsp_IndexEntry {
    char              *canonical_path;   /* malloc'd realpath-normalized absolute path */
    uint64_t           content_hash;     /* 64-bit prefix of SHA-256(source bytes) */

    char              *source_bytes;     /* malloc'd; kept for re-analyze + span text */
    size_t             source_len;

    Iron_Arena        *arena;            /* heap-pointed; owns parsed AST + lex tokens */
    Iron_Program      *program;          /* parse-only at warm-seed; set after parse */

    Iron_AnalyzeResult last_analyze;     /* valid when .analyzed == true */
    bool               analyzed;

    int64_t            last_used_tick;   /* LRU stamp */

    IronLsp_LineIndex  line_idx;         /* built at warm-seed */

    /* Phase 3 Plan 04 Task 01 (NAV-06, Pitfall 6): the set of triple
     * hashes this entry has contributed to the workspace reverse-ref
     * map. stb_ds dynamic array of uint64_t. Used by
     * ilsp_refs_drop_for_entry to remove this entry's contributions
     * in O(contributions_by_this_entry) rather than O(workspace).
     * NULL until the first populate call. */
    uint64_t          *ref_contributed_hashes;
} IronLsp_IndexEntry;

/* Phase 3 Plan 04 Task 01 -- forward decl for the reverse-ref map.
 * Defined in src/lsp/facade/nav/references_index.c as a stb_ds shmap
 * keyed on the 64-bit triple hash (uint64_t -> span array). */
struct IronLsp_RefsMapEntry;

typedef struct IronLsp_WorkspaceIndex {
    char                       *workspace_root;   /* absolute realpath */
    /* stb_ds shmap -- key = canonical_path (sh_new_strdup); value = heap-alloc entry. */
    struct { char *key; IronLsp_IndexEntry *value; } *entries;
    int                         analyzed_count;   /* for LRU cap */
    iron_mutex_t                lock;             /* coarse lock */
    _Atomic int_least64_t       tick;             /* monotonic LRU counter */
    struct IronLsp_StdlibCache *stdlib;           /* process-lifetime singleton ptr */
    struct IronLsp_DepMap      *deps;             /* per-workspace */

    /* Phase 3 Plan 04 Task 01 (NAV-06, D-09): workspace-wide reverse
     * reference map. stb_ds struct hashmap (hmput/hmget) keyed by the
     * 64-bit FNV-1a hash of IronLsp_SymbolId; value is a stb_ds array
     * of IronLsp_RefSpan (use-site spans + owning canonical_path).
     * Populated by ilsp_refs_populate_for_entry; drained by
     * ilsp_refs_drop_for_entry. Pointer-typed (opaque) so callers
     * don't need references_index.h to include this header. */
    struct IronLsp_RefsMapEntry *refs;

    /* Phase 3 Plan 04 Task 01: one-shot "bulk analyze done" flag.
     * First references request iterates every workspace entry and
     * calls analyze_lazy to produce resolved_sym annotations, then
     * sets this to true so subsequent references queries are cheap. */
    bool                        bulk_analyze_done;
} IronLsp_WorkspaceIndex;

/* Tuning knobs: exposed so tests can override via a separate helper. */
#define ILSP_WORKSPACE_INDEX_ANALYZE_CAP  200
#define ILSP_WORKSPACE_INDEX_WARMSEED_CAP 5000

/* Lifecycle. `workspace_root` may be NULL; in that case warm_seed is a no-op. */
IronLsp_WorkspaceIndex *ilsp_workspace_index_create(const char *workspace_root);
void                    ilsp_workspace_index_destroy(IronLsp_WorkspaceIndex *wi);

/* Warm-seed: walk workspace_root, parse-only every .iron file. Polls
 * cancel between files (may be NULL). Idempotent -- re-invoking is safe
 * (duplicates are replaced). */
void                    ilsp_workspace_index_warm_seed(IronLsp_WorkspaceIndex *wi,
                                                        _Atomic bool          *cancel);

/* Per-path lookup. Returns pointer into the entries map (lifetime tied
 * to the index); caller MUST NOT free. Bumps last_used_tick. NULL on miss.
 * The mutex is held only for the shget + tick bump; subsequent reads of
 * the returned pointer are unlocked (entry itself is never mutated after
 * warm-seed except via invalidate). */
IronLsp_IndexEntry     *ilsp_workspace_index_lookup(IronLsp_WorkspaceIndex *wi,
                                                     const char *canonical_path);

/* Invalidate a single path: frees arena, drops the map slot. No-op on
 * missing. Safe from the dispatcher thread. */
void                    ilsp_workspace_index_invalidate_path(IronLsp_WorkspaceIndex *wi,
                                                              const char *canonical_path);

/* Invalidate dependency-related entries:
 *   dep_name == NULL : drop every entry in the workspace (MANIFEST event)
 *   dep_name != NULL : drop entries whose program contains
 *                      import <dep_name> (Plan 02-03 wires real dep map).
 */
void                    ilsp_workspace_index_invalidate_dep(IronLsp_WorkspaceIndex *wi,
                                                             const char *dep_name);

/* Lazy analyze: populate entry->last_analyze via ilsp_facade_compile_pure
 * (the single iron_analyze_buffer call site).  Idempotent per
 * (canonical_path, content_hash).  May evict an older entry if
 * analyzed_count would exceed ILSP_WORKSPACE_INDEX_ANALYZE_CAP.
 * Returns the entry's program pointer (never NULL when entry is valid). */
Iron_Program           *ilsp_workspace_index_analyze_lazy(IronLsp_WorkspaceIndex *wi,
                                                           IronLsp_IndexEntry *entry,
                                                           _Atomic bool *cancel);

/* Introspection for tests. */
int                     ilsp_workspace_index_analyzed_count(const IronLsp_WorkspaceIndex *wi);
size_t                  ilsp_workspace_index_entry_count  (const IronLsp_WorkspaceIndex *wi);

/* Phase 3 Plan 04 Task 01 -- snapshot every entry's canonical_path
 * under the mutex, returning a freshly malloc'd array of malloc'd
 * path strings. Caller must free each string AND the outer array.
 * Used by references bulk-analyze to iterate safely without holding
 * wi->lock while analyzing (avoids re-entrant locking since analyze
 * eventually calls lookup which also takes the lock). */
char                  **ilsp_workspace_index_snapshot_paths(IronLsp_WorkspaceIndex *wi,
                                                              size_t                 *out_n);

/* Phase 3 Plan 04 Task 01 -- public hook called by the references
 * facade when the first `textDocument/references` request arrives.
 * Iterates every entry, calls analyze_lazy, and populates reverse
 * refs contributions. Idempotent: subsequent calls short-circuit if
 * wi->bulk_analyze_done is already true. Polls cancel between files
 * (D-16). */
void                    ilsp_workspace_index_bulk_analyze_for_refs(
                            IronLsp_WorkspaceIndex *wi,
                            _Atomic bool           *cancel);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_STORE_WORKSPACE_INDEX_H */
