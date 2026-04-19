#ifndef IRON_LSP_SERVER_HANDLERS_EDIT_H
#define IRON_LSP_SERVER_HANDLERS_EDIT_H

/* Phase 4 Plan 04-02 Task 03 -- EDIT request dispatchers.
 *
 * textDocument/completion + completionItem/resolve handlers. Each
 * handler follows the 7-step pattern from handlers_nav.c:
 *   1. Parse id_v, params, uri, position.
 *   2. Register cancel flag.
 *   3. Look up the open document.
 *   4. Delegate to the facade.
 *   5. If cancelled, drop the response.
 *   6. Build yyjson response.
 *   7. Enqueue at ILSP_PRIO_RESPONSE.
 */

#include "util/arena.h"

struct IronLsp_Server;
struct yyjson_doc;

#ifdef __cplusplus
extern "C" {
#endif

void ilsp_handle_text_document_completion(struct IronLsp_Server *s,
                                            struct yyjson_doc     *doc,
                                            Iron_Arena            *arena);

void ilsp_handle_completion_item_resolve(struct IronLsp_Server *s,
                                           struct yyjson_doc     *doc,
                                           Iron_Arena            *arena);

/* Phase 4 Plan 04-04 Task 02 (EDIT-07, EDIT-08) — code-action handlers.
 * Same 7-step pattern. codeAction builds lightweight actions without
 * the edit field; codeAction/resolve re-runs the quickfix handler to
 * materialize the edit lazily, guarded on the file version. */
void ilsp_handle_text_document_code_action(struct IronLsp_Server *s,
                                             struct yyjson_doc     *doc,
                                             Iron_Arena            *arena);

void ilsp_handle_code_action_resolve(struct IronLsp_Server *s,
                                       struct yyjson_doc     *doc,
                                       Iron_Arena            *arena);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_SERVER_HANDLERS_EDIT_H */
