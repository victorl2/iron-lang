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
} IronLsp_IndexEntry;

typedef struct IronLsp_WorkspaceIndex {
    char                       *workspace_root;   /* absolute realpath */
    /* stb_ds shmap -- key = canonical_path (sh_new_strdup); value = heap-alloc entry. */
    struct { char *key; IronLsp_IndexEntry *value; } *entries;
    int                         analyzed_count;   /* for LRU cap */
    iron_mutex_t                lock;             /* coarse lock */
    _Atomic int_least64_t       tick;             /* monotonic LRU counter */
    struct IronLsp_StdlibCache *stdlib;           /* process-lifetime singleton ptr */
    struct IronLsp_DepMap      *deps;             /* per-workspace */
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

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_STORE_WORKSPACE_INDEX_H */
