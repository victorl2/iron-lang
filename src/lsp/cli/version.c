/* Phase 7 Plan 07-07 Task 01 (HARD-22, D-10) -- runtime accessor for the
 * compile-time IRON_VERSION_STRING define.
 *
 * See lsp/cli/version.h for the single-source-of-truth rationale.
 *
 * The build system (CMakeLists.txt ironls target) is expected to ALWAYS
 * pass -DIRON_VERSION_STRING="..."; the #ifndef guard is a last-line
 * defense so a misconfigured downstream build surfaces a sentinel
 * instead of failing to link. The coherence CTest will fire immediately
 * in that regression path (sentinel != iron/ironc version), so the
 * fallback is observable, not silent.
 */

#include "lsp/cli/version.h"

#ifndef IRON_VERSION_STRING
#  define IRON_VERSION_STRING "unknown"
#endif

const char *ilsp_server_version(void) {
    return IRON_VERSION_STRING;
}
