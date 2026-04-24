#ifndef IRON_LSP_CLI_VERSION_H
#define IRON_LSP_CLI_VERSION_H

/* Phase 7 Plan 07-07 Task 01 (HARD-22, D-10) -- runtime accessor for the
 * compile-time IRON_VERSION_STRING define.
 *
 * Single source of truth:
 *   CMakeLists.txt IRON_VERSION_FULL
 *     -> target_compile_definitions(ironls PRIVATE IRON_VERSION_STRING=...)
 *       -> this function's return value
 *         -> initialize response serverInfo.version
 *         -> ironls --version stdout
 *         -> iron-lsp.diagnose / :IronLspDiagnose payload (via initialize).
 *
 * All three consumers read the same bytes. The coherence CTest
 * invariant (tests/lsp/invariant/test_version_stamp_coherence.sh)
 * pins the byte-for-byte match across `iron`, `ironc`, `ironls`.
 *
 * Thread safety: the returned pointer is a read-only string literal in
 * the ironls binary's .rodata section; safe to call from any thread at
 * any time without synchronization.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the IRON_VERSION_STRING compile-time define as a read-only
 * string literal. Never returns NULL. */
const char *ilsp_server_version(void);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_CLI_VERSION_H */
