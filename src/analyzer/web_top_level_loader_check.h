/* web_top_level_loader_check.h — WEB-ASSET-03 analyzer pass.
 *
 * Emits a compile-time error if any of the raylib resource-loader functions
 * (LoadTexture, LoadSound, LoadFont, LoadModel) is called at module level —
 * i.e., outside any function body — when the build target is IRON_TARGET_WEB.
 *
 * Rationale: web builds preload assets asynchronously via --preload-file,
 * which mounts the asset directory into MEMFS.  A top-level call to these
 * functions executes before MEMFS mounting completes, causing a silent failure
 * or a hard abort.  Moving the call inside main() (after InitWindow) is the
 * correct fix.
 *
 * On IRON_TARGET_NATIVE this pass is a zero-cost early return.
 *
 * Pipeline slot: runs inside iron_analyze() immediately after iron_web_await_check.
 * Registered in src/analyzer/analyzer.c.
 */
#ifndef IRON_WEB_TOP_LEVEL_LOADER_CHECK_H
#define IRON_WEB_TOP_LEVEL_LOADER_CHECK_H

#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "cli/build.h"

void iron_web_top_level_loader_check(Iron_Program *program, Iron_Arena *arena,
                                     Iron_DiagList *diags,
                                     IronBuildTarget target);

#endif /* IRON_WEB_TOP_LEVEL_LOADER_CHECK_H */
