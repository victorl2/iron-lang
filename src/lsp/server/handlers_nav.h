#ifndef IRON_LSP_SERVER_HANDLERS_NAV_H
#define IRON_LSP_SERVER_HANDLERS_NAV_H

/* Phase 3 Plan 03 Task 02/03 -- NAV request dispatchers.
 *
 * Each handler parses its JSON params, looks up the target document,
 * delegates to the corresponding facade/nav endpoint TU, and
 * enqueues a JSON-RPC response on server->writer at
 * ILSP_PRIO_RESPONSE. The dispatcher (src/lsp/server/dispatch.c)
 * invokes these functions via the alphabetically-sorted handler
 * table. */

#include "util/arena.h"

struct IronLsp_Server;
struct yyjson_doc;

#ifdef __cplusplus
extern "C" {
#endif

void ilsp_handle_text_document_definition(struct IronLsp_Server *s,
                                            struct yyjson_doc     *doc,
                                            Iron_Arena            *arena);

void ilsp_handle_text_document_declaration(struct IronLsp_Server *s,
                                             struct yyjson_doc     *doc,
                                             Iron_Arena            *arena);

void ilsp_handle_text_document_type_definition(struct IronLsp_Server *s,
                                                 struct yyjson_doc     *doc,
                                                 Iron_Arena            *arena);

void ilsp_handle_text_document_document_symbol(struct IronLsp_Server *s,
                                                 struct yyjson_doc     *doc,
                                                 Iron_Arena            *arena);

void ilsp_handle_workspace_symbol(struct IronLsp_Server *s,
                                    struct yyjson_doc     *doc,
                                    Iron_Arena            *arena);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_SERVER_HANDLERS_NAV_H */
