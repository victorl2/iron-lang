/* web_await_check.h — WEB-RUNTIME-04 analyzer pass.
 *
 * Emits a compile-time error if `await` is reachable from the program's
 * entry point (`main`) when the build target is IRON_TARGET_WEB.
 *
 * On IRON_TARGET_NATIVE this pass is a zero-cost early return.
 *
 * Pipeline slot: runs inside iron_analyze() after iron_concurrency_check
 * and before iron_iface_collect. Registered in src/analyzer/analyzer.c.
 */
#ifndef IRON_WEB_AWAIT_CHECK_H
#define IRON_WEB_AWAIT_CHECK_H

#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "cli/build.h"

void iron_web_await_check(Iron_Program *program, Iron_Arena *arena,
                          Iron_DiagList *diags, IronBuildTarget target);

#endif /* IRON_WEB_AWAIT_CHECK_H */
