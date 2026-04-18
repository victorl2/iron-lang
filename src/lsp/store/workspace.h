#ifndef IRON_LSP_STORE_WORKSPACE_H
#define IRON_LSP_STORE_WORKSPACE_H

/* Phase 2 Plan 04 Task 02 (CORE-13) -- Workspace root discovery +
 * watched-files event dispatch.
 *
 * Plan 04 only locates the workspace root (walk up from the first
 * `workspaceFolders` entry until `iron.toml` is found). Full workspace
 * indexing is Phase 3 NAV-01; Plan 04 just logs noops on
 * `workspace/didChangeWatchedFiles` so that the handler exists and
 * records traffic. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Extract the filesystem path from a file:// URI. Returns a malloc'd
 * absolute path, or NULL on malformed input. The caller owns the result. */
char *ilsp_workspace_path_from_uri(const char *uri);

/* Walk up from `start_path` looking for `iron.toml` on each ancestor.
 * Returns malloc'd absolute path to the directory containing iron.toml,
 * or NULL if none is found. */
char *ilsp_workspace_find_root(const char *start_path);

/* Classification for a watched-files event path. Used by
 * handlers_document.c to decide which log event to emit. */
typedef enum IronLsp_WatchedKind {
    ILSP_WATCHED_UNKNOWN  = 0,
    ILSP_WATCHED_SOURCE   = 1,  /* *.iron */
    ILSP_WATCHED_MANIFEST = 2,  /* iron.toml */
    ILSP_WATCHED_LOCKFILE = 3,  /* iron.lock */
} IronLsp_WatchedKind;

/* Classify a URI or path by suffix. */
IronLsp_WatchedKind ilsp_workspace_classify(const char *uri_or_path);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_STORE_WORKSPACE_H */
