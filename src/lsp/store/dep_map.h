#ifndef IRON_LSP_STORE_DEP_MAP_H
#define IRON_LSP_STORE_DEP_MAP_H

/* Phase 3 Plan 02 Task 03 (NAV-01) -- iron.toml dep symbol map.
 *
 * Resolves iron.toml dependencies by library-linking src/pkg/resolver.c
 * (NO subprocess).  For each resolved dep, the map holds:
 *   - canonical_path   : dep cache root (realpath-normalised)
 *   - exported_symbols : stb_ds array of top-level decl names
 *   - arena            : per-dep Iron_Arena (freed on invalidation)
 *
 * The map is keyed on the dep's declared name in iron.toml.
 *
 * Path-escape defense (T-03-05):
 *   After resolve_dependencies returns, each dep's canonical path is
 *   realpath'd and rejected if it doesn't live under workspace_root or
 *   ~/.iron/cache/.  This mitigates a crafted iron.toml pointing at /etc
 *   or ../../../../tmp via symlinks.
 *
 * Invalidation:
 *   - ilsp_dep_map_invalidate(dm, NULL)  drops every entry.
 *   - ilsp_dep_map_invalidate(dm, name)  drops just the named dep.
 *
 * Inject (test seam):
 *   ilsp_dep_map_inject_for_test lets unit tests populate an entry from
 *   a path without invoking the network-touching resolver.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "parser/ast.h"    /* Iron_Program typedef */
#include "util/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IronLsp_DepMap IronLsp_DepMap;

typedef struct IronLsp_DepEntry {
    char        *dep_name;          /* malloc'd */
    char        *canonical_path;    /* realpath of dep cache root */
    Iron_Arena  *arena;             /* per-dep; freed on invalidation */
    const char **exported_symbols;  /* stb_ds array; arena-interned names */
} IronLsp_DepEntry;

/* Lifecycle. workspace_root must be non-NULL -- used for path-escape
 * check (all deps must live under workspace_root or ~/.iron/cache/). */
IronLsp_DepMap *ilsp_dep_map_create (const char *workspace_root);
void            ilsp_dep_map_destroy(IronLsp_DepMap *dm);

/* Parse iron.toml at {workspace_root}/iron.toml, call resolve_dependencies
 * (library-linked), populate entries.  Returns 0 on success or when
 * iron.toml is absent/empty (the LSP stays usable in bare workspaces).
 * Negative on error.  Polls `cancel` between each dep. */
int             ilsp_dep_map_resolve(IronLsp_DepMap *dm, _Atomic bool *cancel);

/* O(1) lookup. Returns NULL if dep_name is not resolved. */
IronLsp_DepEntry *ilsp_dep_map_lookup(IronLsp_DepMap *dm,
                                       const char *dep_name);

/* Invalidate one dep (dep_name != NULL) or all (dep_name == NULL). */
void            ilsp_dep_map_invalidate(IronLsp_DepMap *dm,
                                         const char *dep_name);

/* Count of live entries (mostly for tests). */
size_t          ilsp_dep_map_size(const IronLsp_DepMap *dm);

/* Test seam: inject a dep entry bypassing the resolver. Takes ownership
 * of `exported_symbols_arr` (stb_ds array; may be NULL). `dep_root` is
 * the filesystem path to the dep cache; it is realpath'd internally and
 * rejected if it escapes the workspace_root or recognised cache path. */
bool            ilsp_dep_map_inject_for_test(IronLsp_DepMap *dm,
                                              const char *dep_name,
                                              const char *dep_root,
                                              const char **exported_symbols_arr);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_STORE_DEP_MAP_H */
