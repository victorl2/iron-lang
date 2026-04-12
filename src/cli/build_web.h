#ifndef IRON_CLI_BUILD_WEB_H
#define IRON_CLI_BUILD_WEB_H

#include "cli/build.h"      /* for IronBuildOpts */
#include "cli/web_config.h" /* for IronWebConfig */

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

/* Phase 7: Link the emitted C file with emcc into dist/web/index.{html,js,wasm}.
 *
 * Called by iron_build() step 13 when target == IRON_TARGET_WEB, AFTER the
 * Phase 6 emit_web_module() has produced a C file on disk. This function
 * constructs and executes a single emcc invocation with the canonical flag
 * set (WEB-BUILD-03), linking the emitted C against the Iron runtime + 5
 * web-compatible stdlib shims (including iron_time_web.c instead of
 * iron_time.c) + 3 util source files.
 *
 * Every translation unit in the link is compiled with -pthread
 * (WEB-BUILD-04). The output directory dist/web/ is created via mkdir_p
 * if it does not already exist (WEB-BUILD-06).
 *
 * Raylib linkage is deferred to Phase 8. Forbidden-flag validation is
 * performed by Plan 02's is_forbidden_flag self-audit at argv end.
 *
 * Parameters:
 *   c_file_path: path to the emitted C file (output of emit_web_module via
 *                write_temp_c in build.c)
 *   opts:        build options; opts.release selects release vs debug flag set,
 *                opts.verbose gates the command-dump in -v mode
 *   cfg:         parsed [web] config from iron.toml (may be NULL — defaults
 *                from web_config.h are applied).
 *   toml_dir:    directory containing iron.toml for resolving relative asset
 *                paths (WEB-ASSET-04). NULL is treated as "." (cwd). Required
 *                for [web].assets path resolution; may be NULL when there is
 *                no iron.toml on disk (e.g. bare hello.iron builds).
 *
 * Returns 0 on success (emcc exited 0 and dist/web/index.html exists),
 * non-zero on failure.
 */
int iron_build_web_link(const char *c_file_path, IronBuildOpts opts,
                        IronWebConfig *cfg, const char *toml_dir,
                        const char *lib_dir);

#endif /* IRON_CLI_BUILD_WEB_H */
