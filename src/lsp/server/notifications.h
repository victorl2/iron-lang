#ifndef IRON_LSP_SERVER_NOTIFICATIONS_H
#define IRON_LSP_SERVER_NOTIFICATIONS_H

/* Phase 2 Plan 05 Task 01 (CORE-18, CORE-20) -- Outbound LSP notification
 * builders.
 *
 * The SIGABRT boundary in src/lsp/workers/ast_worker.c calls
 * ilsp_send_window_showmessage to notify the editor that analysis
 * crashed (first strike -> Warning) or that the document is quarantined
 * (second strike -> Error). The helper builds the LSP 3.17
 * `window/showMessage` JSON body via yyjson and enqueues it on the
 * server's writer queue at ILSP_PRIO_NOTIFICATION priority.
 *
 * Safe to call from any thread: the writer queue is internally locked.
 * May drop under backpressure (Plan 02 CORE-04 policy); the dropped
 * notification is logged on the writer thread.
 *
 * window/showMessage params shape per LSP 3.17:
 *   { "type": <MessageType>, "message": "<string>" }
 *
 * MessageType enum: 1=Error, 2=Warning, 3=Info, 4=Log. Note there is no
 * `uri` field on window/showMessage params -- the URI is interpolated
 * into `message` by the caller for user context. */

struct IronLsp_Server;

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IronLsp_MessageType {
    ILSP_MESSAGE_TYPE_ERROR   = 1,
    ILSP_MESSAGE_TYPE_WARNING = 2,
    ILSP_MESSAGE_TYPE_INFO    = 3,
    ILSP_MESSAGE_TYPE_LOG     = 4,
} IronLsp_MessageType;

/* Build and enqueue a `window/showMessage` notification onto
 * server->writer at ILSP_PRIO_NOTIFICATION priority. `uri` is accepted
 * for caller context (typically interpolated into `message`) but is NOT
 * serialized separately -- the LSP spec does not define a uri field on
 * window/showMessage params. `message_type` is an LSP MessageType int
 * (1=Error, 2=Warning, 3=Info, 4=Log). Dropped silently if server or
 * message is NULL. */
void ilsp_send_window_showmessage(struct IronLsp_Server *server,
                                   const char            *uri,
                                   int                    message_type,
                                   const char            *message);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_SERVER_NOTIFICATIONS_H */
