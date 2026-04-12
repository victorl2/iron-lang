#ifndef IRON_CLI_BUILD_WEB_H
#define IRON_CLI_BUILD_WEB_H

#include "cli/build.h"  /* for IronBuildOpts */

/* Preflight for `iron build --target=web`.
 *
 * Phase 6: renamed from stub to preflight. Probes PATH for emcc, reads the
 * pinned version from .emsdk-version, prints `using emcc <ver> from <path>`,
 * soft-warns on version drift, validates the parsed IronWebConfig on
 * IronProject, and returns 0 on success.
 *
 * Called by iron_build() at entry when target == IRON_TARGET_WEB. On 0
 * return the caller falls through to the main pipeline (lexer → LIR →
 * emit_web_module). Phase 7 will replace invoke_clang (step 13) with emcc.
 *
 * Parameters:
 *   source_path: path to the .iron source file (matches iron_build)
 *   output_path: requested output path or NULL to derive (unused in Phase 6)
 *   opts:        build options (target field MUST be IRON_TARGET_WEB)
 *
 * Returns 0 on success, non-zero on error
 * (emcc not found, config validation failure).
 */
int iron_build_web(const char *source_path, const char *output_path,
                   IronBuildOpts opts);

#endif /* IRON_CLI_BUILD_WEB_H */
