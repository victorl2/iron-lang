#ifndef IRON_LSP_SERVER_HANDLERS_WORKSPACE_DIAG_H
#define IRON_LSP_SERVER_HANDLERS_WORKSPACE_DIAG_H

/* Phase 3 Plan 06 (NAV-12, NAV-13, D-12) -- workspace/diagnostic handler
 * + refresh-notification emitter.
 *
 * `workspace/diagnostic` is a client-initiated pull request: the client
 * asks the server for diagnostics across every file in the workspace
 * index. The handler delegates to ilsp_facade_workspace_diagnostic
 * (per the Handler->Facade->iron_compiler hard rule) and wraps the
 * returned per-file reports into a WorkspaceDiagnosticReport response.
 *
 * `workspace/diagnostic/refresh` is a server-initiated push: when a
 * non-open file in the index is invalidated (didChangeWatchedFiles),
 * the server emits this notification so the client re-pulls. Called
 * from src/lsp/server/handlers_document.c. */

#include "util/arena.h"

struct IronLsp_Server;
struct yyjson_doc;

#ifdef __cplusplus
extern "C" {
#endif

/* Pull handler -- registered in dispatch.c's handler table. */
void ilsp_handle_workspace_diagnostic(struct IronLsp_Server *s,
                                        struct yyjson_doc     *doc,
                                        Iron_Arena            *arena);

/* Push-notification emitter. No params per LSP 3.17
 * workspace/diagnostic/refresh. Enqueues at ILSP_PRIO_NOTIFICATION. */
void ilsp_send_workspace_diagnostic_refresh(struct IronLsp_Server *s);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_SERVER_HANDLERS_WORKSPACE_DIAG_H */
