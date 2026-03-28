#ifndef IRON_PKG_RESOLVER_H
#define IRON_PKG_RESOLVER_H

#include <stdbool.h>
#include "cli/toml.h"

/* Result of dependency resolution: a flat list of resolved deps
 * in topological order (leaf deps first, direct deps last).
 * Each entry has name, git, version, sha, and cache_path filled in. */
typedef struct {
    IronDep *deps;       /* malloc'd array of resolved deps (topo-sorted) */
    int      count;      /* number of entries */
    int      capacity;   /* allocated capacity */
} ResolvedDeps;

/* Resolve all dependencies for a project.
 *
 * proj: the parsed iron.toml of the root project
 * proj_dir: the project directory (where iron.toml lives)
 * colors: whether to print colored output
 *
 * Reads iron.lock from {proj_dir}/iron.lock if it exists.
 * For each dep not in iron.lock: resolves tag to SHA via GitHub API.
 * Downloads and extracts dep source into ~/.iron/cache/.
 * Recursively resolves transitive deps from each dep's iron.toml.
 * Detects circular dependencies (error + exit).
 * Detects version conflicts (same git at different versions, error + exit).
 * Handles diamond deps (same git at same version, include once).
 * Writes updated iron.lock to {proj_dir}/iron.lock.
 *
 * Returns 0 on success (result populated), non-zero on error.
 * On success, caller must call resolved_deps_free(result). */
int resolve_dependencies(IronProject *proj, const char *proj_dir,
                         bool colors, ResolvedDeps *result);

/* Free a ResolvedDeps result. */
void resolved_deps_free(ResolvedDeps *result);

#endif /* IRON_PKG_RESOLVER_H */
