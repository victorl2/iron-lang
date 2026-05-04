/* tests/fuzz/lsp/dispatch/handler_stubs.c — Phase 7 Plan 07-05 (HARD-19).
 *
 * Stub definitions for every ilsp_handle_* symbol referenced by
 * src/lsp/server/dispatch.c's handler table. The dispatch fuzz harness
 * links dispatch.c to exercise ilsp_handler_lookup (a pure bsearch) but
 * deliberately does NOT invoke ilsp_dispatch_route, which would require
 * the full server singleton + writer + document store + workspace index
 * + compiler pipeline. These stubs satisfy the handler-table's address
 * taken references so dispatch.c links, but trap loudly if ever invoked
 * -- any invocation would indicate the harness accidentally started
 * routing, which is a bug in the harness itself (not the target).
 *
 * Stub surface mirrors the `extern` forward-decl block in dispatch.c
 * lines 34-98. If a plan adds a new handler to the dispatch table, the
 * linker will fail here and the maintainer must add a matching stub.
 */

#include <stddef.h>

#include "util/arena.h"

struct yyjson_doc;
struct IronLsp_Server;

/* Common signature for every handler in the dispatch table:
 *   void handler(IronLsp_Server *, yyjson_doc *, Iron_Arena *) */
#define ILSP_FUZZ_HANDLER_STUB(name)                          \
    void name(struct IronLsp_Server *s,                       \
              struct yyjson_doc     *d,                       \
              Iron_Arena            *a) {                     \
        (void)s; (void)d; (void)a;                            \
        __builtin_trap();                                     \
    }

/* Lifecycle (dispatch.c:35-39) */
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_initialize)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_initialized)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_shutdown)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_exit)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_cancel)

/* Document sync + diagnostics (dispatch.c:42-48) */
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_didOpen)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_didChange)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_didClose)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_didSave)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_didChangeWatchedFiles)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_diagnostic)

/* Navigation (dispatch.c:51-68) */
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_declaration)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_definition)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_document_symbol)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_type_definition)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_workspace_symbol)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_hover)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_references)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_signature_help)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_implementation)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_prepare_type_hierarchy)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_type_hierarchy_supertypes)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_type_hierarchy_subtypes)

/* Workspace diagnostic (dispatch.c:71) */
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_workspace_diagnostic)

/* Edit endpoints (dispatch.c:74-90) */
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_completion)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_completion_item_resolve)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_code_action)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_code_action_resolve)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_prepare_rename)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_rename)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_document_highlight)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_folding_range)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_selection_range)

/* Formatting (dispatch.c:96-98) */
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_formatting)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_range_formatting)
ILSP_FUZZ_HANDLER_STUB(ilsp_handle_text_document_on_type_formatting)

/* -- Transport + obs surface stubs -------------------------------------
 *
 * These satisfy the linker when dispatch.c is pulled into the fuzz
 * target. The fuzz harness exercises only ilsp_handler_lookup (pure
 * bsearch), so none of these runtime helpers are ever invoked. If the
 * fuzzer regresses and starts calling ilsp_dispatch_route, the __builtin_trap
 * below will surface the bug on the first iteration.
 */

#include <stdint.h>

/* From src/lsp/obs/crash_dump.h. The dispatcher calls these on every
 * request entry/exit but ilsp_handler_lookup alone never touches them. */
void ilsp_crash_ring_push(uint64_t request_id, const char *method);
void ilsp_crash_ring_pop (uint64_t request_id);
void ilsp_crash_ring_push(uint64_t request_id, const char *method) {
    (void)request_id; (void)method;
    __builtin_trap();
}
void ilsp_crash_ring_pop(uint64_t request_id) {
    (void)request_id;
    __builtin_trap();
}

/* From src/lsp/transport/writer.h. Dispatcher uses enqueue_error at
 * dispatch.c:277; harness never enters that path. */
struct IronLsp_Writer;
typedef enum IronLsp_EnqueueResult {
    ILSP_ENQUEUE_OK,
    ILSP_ENQUEUE_OK_DROPPED_LOG,
    ILSP_ENQUEUE_OK_DROPPED_NOTIFICATION,
    ILSP_ENQUEUE_FULL_DROPPED
} IronLsp_EnqueueResult;
typedef enum IronLsp_Priority {
    ILSP_PRIO_RESPONSE,
    ILSP_PRIO_NOTIFICATION,
    ILSP_PRIO_LOG
} IronLsp_Priority;

IronLsp_EnqueueResult ilsp_writer_enqueue(struct IronLsp_Writer *w,
                                          IronLsp_Priority prio,
                                          char *body, size_t len) {
    (void)w; (void)prio; (void)body; (void)len;
    __builtin_trap();
}
