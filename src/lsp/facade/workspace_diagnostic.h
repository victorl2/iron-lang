#ifndef IRON_LSP_FACADE_WORKSPACE_DIAGNOSTIC_H
#define IRON_LSP_FACADE_WORKSPACE_DIAGNOSTIC_H

/* Phase 3 Plan 06 Task 01 (NAV-12, D-12) -- workspace/diagnostic facade.
 *
 * Walks every IronLsp_WorkspaceIndex entry; for each file:
 *   - if (canonical_path, content_hash) is already in this cache, return
 *     kind="unchanged" with the cached resultId,
 *   - else run facade analyze via ilsp_facade_compile_for_nav (single
 *     iron_analyze_buffer call site preserved, CORE-22), cache the
 *     Iron_DiagList, build a DocumentDiagnosticReport-shaped JSON items[],
 *     and return kind="full".
 *
 * Budget: 2000 ms wall-clock. Files not yet analyzed at the deadline get
 * kind="unchanged" with previousResultId so the client re-pulls.
 *
 * Cancel: D-16 iteration-boundary polling. The cancel flag is checked
 * at every per-file loop entry; cancelled walks return the accumulated
 * reports up to that point.
 *
 * Cache: LRU = 200 entries; keyed on (canonical_path, content_hash).
 * resultId = "w<workspace_version>-<hash_prefix_8hex>". workspace_version
 * is a monotonic counter maintained alongside the cache (one cache per
 * server). */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diagnostics/diagnostics.h"  /* Iron_DiagList */
#include "util/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_Server;
struct yyjson_mut_doc;
struct yyjson_mut_val;

/* Cache budget knobs. Tunable, but match D-12 locked defaults. */
#define ILSP_WS_DIAG_LRU_CAP       200
#define ILSP_WS_DIAG_BUDGET_MS    2000
#define ILSP_WS_DIAG_SERIALIZE_MS  200  /* reserved for JSON serialization */

/* Opaque cache type. Created once per server, lives for server lifetime. */
typedef struct IronLsp_WsDiagCache IronLsp_WsDiagCache;

IronLsp_WsDiagCache *ilsp_ws_diag_cache_create(void);
void                 ilsp_ws_diag_cache_destroy(IronLsp_WsDiagCache *c);

/* Drop the cache slot for a given canonical path (e.g., on invalidate).
 * No-op when missing. Safe under the workspace_index lock. */
void ilsp_ws_diag_cache_evict_path(IronLsp_WsDiagCache *c,
                                    const char *canonical_path);

/* Test introspection: current entry count. */
size_t ilsp_ws_diag_cache_size(const IronLsp_WsDiagCache *c);

/* Bump the workspace_version monotonic counter (resultId uses it so
 * clients know the whole workspace has rolled forward). */
uint64_t ilsp_ws_diag_cache_bump_version(IronLsp_WsDiagCache *c);

/* One per-file entry in the result. uri + kind + resultId + items_json
 * where items_json is present only for kind="full". All fields are
 * arena-allocated (the arena supplied to
 * ilsp_facade_workspace_diagnostic). */
typedef struct IronLsp_WsDiagFileReport {
    const char             *uri;           /* file:// URI (arena) */
    const char             *kind;          /* "full" or "unchanged" */
    int32_t                 version;       /* -1 when not an open doc */
    const char             *result_id;     /* arena-owned string */
    /* JSON items[] array of LSP Diagnostic objects. NULL for "unchanged"
     * reports. Produced with the same yyjson_mut_doc the caller uses to
     * serialize the final response, so no cross-doc splicing is needed. */
    struct yyjson_mut_val  *items_json;
} IronLsp_WsDiagFileReport;

/* Facade entry: walk workspace_index entries under a 2 s budget, produce
 * an array of per-file reports. Cancel flag polled between files.
 *
 * previous_result_id is optional and currently ignored (clients pass it
 * back so we could implement partial-progress refresh, but the v1
 * implementation uses per-file caching which already handles this).
 *
 * Returned array + items_json are arena-owned; caller MUST serialize the
 * response before freeing the arena. */
void ilsp_facade_workspace_diagnostic(struct IronLsp_Server    *server,
                                        const char               *previous_result_id,
                                        _Atomic bool             *cancel,
                                        struct yyjson_mut_doc    *json_doc,
                                        Iron_Arena               *arena,
                                        IronLsp_WsDiagFileReport **out_reports,
                                        size_t                   *out_n);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_WORKSPACE_DIAGNOSTIC_H */
