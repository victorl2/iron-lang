#ifndef IRON_LSP_STORE_STDLIB_CACHE_H
#define IRON_LSP_STORE_STDLIB_CACHE_H

/* Phase 3 Plan 02 Task 02 (NAV-01, D-14) -- stdlib pre-parse cache.
 *
 * A process-lifetime immutable `Iron_Program` graph keyed by module
 * name (filename stem: "math.iron" -> "math"). Populated once via
 * pthread_once at first ilsp_stdlib_cache_init call; subsequent calls
 * return the already-initialised singleton.
 *
 * The cache loads every `.iron` surface file in:
 *   {iron_source_dir}/stdlib/  (all *.iron files)
 *   {iron_source_dir}/runtime/ (all *.iron files)
 *
 * Files that fail to parse are logged (when the log sink is open) and
 * omitted; init never aborts on a single bad file. Stdlib surface
 * files rarely error in practice.
 *
 * The cache is INTENDED to leak at process exit -- this is the
 * documented "static-mutable-state exception" (same discipline as
 * obs/log.c per Phase 2 Plan 06 precedent). Tests may call
 * ilsp_stdlib_cache_destroy explicitly.
 */

#include <stdbool.h>
#include <stddef.h>

#include "parser/ast.h"   /* Iron_Program typedef (anonymous struct) */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IronLsp_StdlibCache IronLsp_StdlibCache;

/* Idempotent init.  `iron_source_dir` may be NULL, in which case the
 * cache resolves the path via:
 *   1. $IRON_SOURCE_DIR env var (if set)
 *   2. Compile-time IRON_SOURCE_DIR define (when present)
 *   3. "src" (cwd-relative last-ditch fallback)
 *
 * Safe to call from any thread.  Returns the singleton pointer (never
 * NULL on success of the first call; NULL if pthread_once itself fails). */
IronLsp_StdlibCache *ilsp_stdlib_cache_init(const char *iron_source_dir);

/* Retrieve a parsed stdlib module by stem ("math", "io", etc.).
 * Returns NULL when module is not in the cache.  The returned
 * Iron_Program is IMMUTABLE; the caller MUST NOT mutate it, and the
 * pointer is valid for the full process lifetime. */
const Iron_Program *ilsp_stdlib_cache_get(IronLsp_StdlibCache *cache,
                                          const char *module_name);

/* Count of modules currently in the cache (mostly for tests). */
size_t ilsp_stdlib_cache_size(IronLsp_StdlibCache *cache);

/* Destroy (tests only).  Clears the pthread_once state so subsequent
 * init calls run fresh.  Do NOT call from production code. */
void ilsp_stdlib_cache_destroy(IronLsp_StdlibCache *cache);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_STORE_STDLIB_CACHE_H */
