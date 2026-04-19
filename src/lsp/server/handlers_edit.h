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

/* Phase 4 Plan 04-06 Task 03 (EDIT-10, EDIT-11, EDIT-12, D-09, D-10,
 * D-11) — rename handlers. prepareRename classifies the cursor and
 * emits {range, placeholder} (accept) or null + optional window/
 * showMessage (reject). rename builds the full WorkspaceEdit via
 * the apply facade, emitting either documentChanges (preferred) or
 * the legacy changes map based on the sniffed client capability.
 * Collision / stdlib-implementor / dep-implementor failures emit
 * JSON-RPC -32803 RequestFailed with the offender location in the
 * message. */
void ilsp_handle_text_document_prepare_rename(struct IronLsp_Server *s,
                                                 struct yyjson_doc     *doc,
                                                 Iron_Arena            *arena);

void ilsp_handle_text_document_rename(struct IronLsp_Server *s,
                                         struct yyjson_doc     *doc,
                                         Iron_Arena            *arena);

/* Phase 4 Plan 04-07 Task 03 (EDIT-13, EDIT-14, EDIT-15, D-12, D-13,
 * D-14) -- parser-only "always-on" editing endpoints. Same 7-step
 * pattern as the nav handlers. documentHighlight is analyzer-backed
 * (needs resolved_sym via ilsp_facade_compile_for_nav -- CORE-22
 * invariant preserved). foldingRange + selectionRange are strict
 * parse-only: they keep working on syntactically broken files. */
void ilsp_handle_text_document_document_highlight(struct IronLsp_Server *s,
                                                     struct yyjson_doc     *doc,
                                                     Iron_Arena            *arena);

void ilsp_handle_text_document_folding_range    (struct IronLsp_Server *s,
                                                     struct yyjson_doc     *doc,
                                                     Iron_Arena            *arena);

void ilsp_handle_text_document_selection_range  (struct IronLsp_Server *s,
                                                     struct yyjson_doc     *doc,
                                                     Iron_Arena            *arena);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_SERVER_HANDLERS_EDIT_H */
