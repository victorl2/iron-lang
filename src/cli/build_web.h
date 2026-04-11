#ifndef IRON_CLI_BUILD_WEB_H
#define IRON_CLI_BUILD_WEB_H

#include "cli/build.h"  /* for IronBuildOpts */

/* Dispatch function for `iron build --target=web`.
 *
 * Phase 2: medium-depth stub. Probes PATH for emcc, reads the pinned
 * version from .emsdk-version, prints `using emcc <ver> from <path>`,
 * soft-warns on version drift, validates the parsed IronWebConfig on
 * IronProject, and returns 0 with a "Phase 2 complete" message.
 *
 * Phase 7 will replace the stub's return-point with a real emcc link.
 *
 * Parameters:
 *   source_path: path to the .iron source file (matches iron_build)
 *   output_path: requested output path or NULL to derive
 *   opts:        build options (target field MUST be IRON_TARGET_WEB)
 *
 * Returns 0 on success (stub happy path), non-zero on error
 * (emcc not found, config validation failure, read_file failure).
 */
int iron_build_web(const char *source_path, const char *output_path,
                   IronBuildOpts opts);

#endif /* IRON_CLI_BUILD_WEB_H */
