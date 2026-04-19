#ifndef IRON_LSP_SERVER_HANDLERS_FMT_H
#define IRON_LSP_SERVER_HANDLERS_FMT_H

/* Phase 5 Plan 05-02 (FMT-02, D-14) -- textDocument/formatting dispatch.
 *
 * Three formatting handlers share the same request-handler ABI used by
 * handlers_edit.c and handlers_nav.c (Phase 3/4):
 *
 *   1. Parse id_v, params, uri from the inbound yyjson doc.
 *   2. Register a cancel flag against the stringified id.
 *   3. Look up the open document for the uri.
 *   4. Delegate to the facade (ilsp_facade_format_full or, for the
 *      stubs, no-op).
 *   5. If cancel was observed, drop the response.
 *   6. Build a yyjson TextEdit[] response body.
 *   7. Enqueue at ILSP_PRIO_RESPONSE.
 *
 * Only the full-document handler is substantive in Plan 05-02 -- the
 * range and on-type stubs emit empty TextEdit[] so the capability
 * advertisement is honest (MethodNotFound is avoided) while Plans 05-03
 * and 05-04 fill in the real bodies. */

#include "util/arena.h"

struct IronLsp_Server;
struct yyjson_doc;

#ifdef __cplusplus
extern "C" {
#endif

void ilsp_handle_text_document_formatting         (struct IronLsp_Server *s,
                                                    struct yyjson_doc     *doc,
                                                    Iron_Arena            *arena);

/* Plan 05-02 STUB -- Plan 05-03 replaces with real range formatting. */
void ilsp_handle_text_document_range_formatting   (struct IronLsp_Server *s,
                                                    struct yyjson_doc     *doc,
                                                    Iron_Arena            *arena);

/* Plan 05-02 STUB -- Plan 05-04 replaces with real on-type formatting. */
void ilsp_handle_text_document_on_type_formatting (struct IronLsp_Server *s,
                                                    struct yyjson_doc     *doc,
                                                    Iron_Arena            *arena);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_SERVER_HANDLERS_FMT_H */
