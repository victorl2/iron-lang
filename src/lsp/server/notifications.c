/* Phase 2 Plan 05 Task 01 -- LSP notification builders.
 *
 * ilsp_send_window_showmessage is the sole builder of LSP
 * `window/showMessage` notifications for the server. It is invoked by
 * the ASTWorker's SIGABRT recovery branch (first/second strike) and
 * could be reused by later plans for any user-facing warning that does
 * not warrant a structured log event. */

#include "lsp/server/notifications.h"
#include "lsp/server/server.h"          /* IronLsp_Server */
#include "lsp/transport/writer.h"       /* ilsp_writer_enqueue */
#include "lsp/transport/types.h"        /* ILSP_PRIO_NOTIFICATION */
#include "lsp/transport/json.h"         /* ilsp_json_write_mut, ilsp_json_alc */
#include "vendor/yyjson/yyjson.h"
#include "util/arena.h"

#include <stdlib.h>
#include <string.h>

/* Shared builder for window/showMessage + window/logMessage. Both have
 * identical params shape per LSP 3.17; only the method string differs. */
static void send_window_message(IronLsp_Server *server,
                                 const char     *method,
                                 const char     *uri,
                                 int             message_type,
                                 const char     *message) {
    (void)uri;  /* LSP window message params have no uri field. */
    if (!server || !server->writer || !method || !message) return;

    /* Build the notification body on a short-lived per-call arena. The
     * yyjson alc routes all internal allocations through the arena, so
     * on failure we leak nothing beyond the arena's single block. */
    Iron_Arena arena = iron_arena_create(4 * 1024);
    yyjson_alc alc   = ilsp_json_alc(&arena);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&alc);
    if (!doc) {
        iron_arena_free(&arena);
        return;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_strcpy(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_strcpy(doc, root, "method",  method);

    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int   (doc, params, "type",    message_type);
    yyjson_mut_obj_add_strcpy(doc, params, "message", message);
    yyjson_mut_obj_add_val   (doc, root,   "params",  params);

    size_t body_len = 0;
    char  *body_arena = ilsp_json_write_mut(doc, &arena, &body_len);
    if (!body_arena || body_len == 0) {
        iron_arena_free(&arena);
        return;
    }

    /* The writer takes ownership of a malloc-backed buffer (it calls
     * free after sending or dropping). Copy the arena-owned bytes into
     * heap memory before enqueueing. */
    char *heap = (char *)malloc(body_len);
    if (!heap) {
        iron_arena_free(&arena);
        return;
    }
    memcpy(heap, body_arena, body_len);

    ilsp_writer_enqueue(server->writer, ILSP_PRIO_NOTIFICATION, heap, body_len);
    /* Ownership of `heap` is now the writer's; we must not touch it. */

    iron_arena_free(&arena);
}

void ilsp_send_window_showmessage(IronLsp_Server *server,
                                   const char     *uri,
                                   int             message_type,
                                   const char     *message) {
    send_window_message(server, "window/showMessage",
                         uri, message_type, message);
}

void ilsp_send_window_logmessage(IronLsp_Server *server,
                                  const char     *uri,
                                  int             message_type,
                                  const char     *message) {
    send_window_message(server, "window/logMessage",
                         uri, message_type, message);
}
