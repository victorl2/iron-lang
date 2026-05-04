#ifndef IRON_LSP_HANDLERS_WORKSPACE_COMMAND_H
#define IRON_LSP_HANDLERS_WORKSPACE_COMMAND_H

/* Phase 14 Plan 14-02 (CMD-01, CMD-03) -- workspace/executeCommand handler.
 *
 * Handles workspace/executeCommand with command "iron.migrate":
 *   1. CMD-03 version gate: probes ironc --version; fails with window/showMessage
 *      Error if ironc < 3.0.0. Reuses IRON_LSP_IRONC_PATH env var as a test hook.
 *   2. Emits 5-bucket $/progress (1/5: parsing manifest .. 5/5: verifying parity).
 *   3. Spawns `ironc migrate --from v2 --to v3 <workspace-root>` in a temp dir.
 *   4. Returns WorkspaceEdit containing file-by-file diff for editor-side preview.
 *
 * The server does NOT write to disk — the editor handles workspace/applyEdit
 * after the user approves the returned WorkspaceEdit (D-02 UX policy). */

#include "lsp/server/dispatch.h"  /* IronLsp_Server forward decl */
#include "vendor/yyjson/yyjson.h"  /* yyjson_doc */
#include "util/arena.h"            /* Iron_Arena */

#ifdef __cplusplus
extern "C" {
#endif

/* workspace/executeCommand handler — iron.migrate dispatch, subprocess spawn,
 * $/progress emission, WorkspaceEdit construction, CMD-03 version gate. */
void ilsp_handle_workspace_execute_command(IronLsp_Server    *s,
                                           struct yyjson_doc *doc,
                                           Iron_Arena        *arena);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_HANDLERS_WORKSPACE_COMMAND_H */
