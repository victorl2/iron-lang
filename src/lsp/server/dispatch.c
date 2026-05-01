/* Phase 2 Plan 03 Task 01 -- Dispatcher.
 *
 * Compile-time const handler table + bsearch lookup (analog:
 * src/lexer/lexer.c kw_table, ll. 28-79). Every inbound message lands in
 * ilsp_dispatch_route, which:
 *   1. Parses the body via ilsp_json_parse. Parse failure => -32700
 *      ParseError response with id=null.
 *   2. Extracts the method and id. Missing method => notification-silent
 *      (if no id) or -32600 InvalidRequest.
 *   3. Looks up the handler. Unknown method => -32601 MethodNotFound
 *      response (requests only); notifications are silently dropped.
 *   4. Checks the lifecycle gate. Violation => -32002 ServerNotInitialized
 *      in UNINIT/INITIALIZING, -32600 InvalidRequest otherwise.
 *   5. Invokes the handler.
 *   6. Advances lifecycle state via ilsp_lifecycle_next.
 *
 * Errors produce JSON-RPC error responses only when the message was a
 * request (had an id). Notifications never receive error responses per
 * JSON-RPC 2.0 §5. */
#include "lsp/server/dispatch.h"
#include "lsp/server/server.h"
#include "lsp/server/lifecycle.h"
#include "lsp/obs/crash_dump.h"   /* Phase 7 Plan 07-01: in-flight ring buffer */
#include "lsp/transport/json.h"
#include "lsp/transport/writer.h"
#include "lsp/transport/types.h"
#include "vendor/yyjson/yyjson.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Handler forward declarations -- defined in handlers_lifecycle.c. */
void ilsp_handle_initialize (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_initialized(IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_shutdown   (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_exit       (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_cancel     (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);

/* Plan 04: document-sync handlers (defined in handlers_document.c). */
void ilsp_handle_didOpen                 (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_didChange               (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_didClose                (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_didSave                 (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_didChangeWatchedFiles   (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
/* Plan 05: pull-diagnostic request. */
void ilsp_handle_text_document_diagnostic(IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);

/* Phase 3 Plan 03: navigation handlers (handlers_nav.c). */
void ilsp_handle_text_document_declaration   (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_text_document_definition    (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_text_document_document_symbol(IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_text_document_type_definition(IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_workspace_symbol            (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);

/* Phase 3 Plan 04: hover + references + signatureHelp handlers (handlers_nav.c). */
void ilsp_handle_text_document_hover         (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_text_document_references    (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_text_document_signature_help(IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);

/* Phase 3 Plan 05: implementation handler (handlers_nav.c). */
void ilsp_handle_text_document_implementation(IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);

/* Phase 3 Plan 05 Task 02: typeHierarchy protocol (handlers_nav.c). */
void ilsp_handle_text_document_prepare_type_hierarchy(IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_type_hierarchy_supertypes          (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_type_hierarchy_subtypes            (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);

/* Phase 3 Plan 06 (NAV-12): workspace/diagnostic pull handler. */
void ilsp_handle_workspace_diagnostic(IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);

/* Phase 14 Plan 14-02 (CMD-01, CMD-03): workspace/executeCommand handler. */
void ilsp_handle_workspace_execute_command(IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);

/* Phase 4 Plan 04-02 (EDIT-01, EDIT-03): completion + resolve. */
void ilsp_handle_text_document_completion   (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_completion_item_resolve    (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);

/* Phase 4 Plan 04-04 (EDIT-07, EDIT-08): code-action + resolve. */
void ilsp_handle_text_document_code_action  (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_code_action_resolve        (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);

/* Phase 4 Plan 04-06 (EDIT-10, EDIT-11, EDIT-12): prepareRename + rename. */
void ilsp_handle_text_document_prepare_rename(IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_text_document_rename        (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);

/* Phase 4 Plan 04-07 (EDIT-13, EDIT-14, EDIT-15): parser-only
 * always-on editing endpoints (documentHighlight, foldingRange,
 * selectionRange). */
void ilsp_handle_text_document_document_highlight(IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_text_document_folding_range    (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_text_document_selection_range  (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);

/* Phase 5 Plan 05-02 (FMT-02, FMT-03, FMT-04): formatting endpoints.
 * formatting is substantive; rangeFormatting + onTypeFormatting are
 * empty-TextEdit[] stubs that Plans 05-03 and 05-04 replace with real
 * facade calls (handlers_fmt.c). */
void ilsp_handle_text_document_formatting         (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_text_document_range_formatting   (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_text_document_on_type_formatting (IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);

/* ── Handler table ───────────────────────────────────────────────────────
 * MUST remain sorted by method name for bsearch. Plans 04 + 05 will
 * insert document / diagnostics handlers between the lifecycle entries
 * below; keep the invariant.
 *
 * Sort order verification (ASCII): '$' (0x24) < 'e' (0x65) < 'i' (0x69)
 * < 's' (0x73) < 't' (0x74) < 'w' (0x77). Within "textDocument/..." the
 * sub-tokens are also lexical.
 *
 * Alphabetical order within textDocument (ASCII):
 *   declaration < definition < diagnostic < didChange < didClose <
 *   didOpen < didSave < documentSymbol < hover < references <
 *   signatureHelp < typeDefinition
 * because 'c'<'f'<'g'<'i'<'o'<'u'<'h'<'r'<'s'<'y' -- actually the
 * ordering is documentSymbol < hover because the next differing byte
 * after "document" is 'S' (0x53) vs nothing (" " in "hover"): strcmp
 * compares "Symbol" vs "hover" starting at the next byte -- 'S' 0x53 <
 * 'h' 0x68, so documentSymbol < hover. Similarly hover < references
 * ('h' 0x68 < 'r' 0x72), references < signatureHelp ('r' < 's'), and
 * signatureHelp < typeDefinition ('s' < 't'). */
const IronLsp_HandlerEntry ilsp_handler_table[] = {
    { "$/cancelRequest",                   ilsp_handle_cancel,                         false, NULL                    },
    /* Phase 4 Plan 04-04 (EDIT-08): codeAction/resolve.
     * Sort: '$' (0x24) < 'c' (0x63). Within c-prefix, 'codeAction/r...'
     * vs 'completionItem/r...': common 'co', then 'd' (0x64) < 'm'
     * (0x6D), so codeAction/resolve < completionItem/resolve. */
    { "codeAction/resolve",                ilsp_handle_code_action_resolve,            true,  "codeActionProvider"    },
    /* Phase 4 Plan 04-02 (EDIT-03): completionItem/resolve.
     * Sort: '$' (0x24) < 'c' (0x63) < 'e' (0x65), so this row belongs
     * directly after "$/cancelRequest" and before "exit". */
    { "completionItem/resolve",            ilsp_handle_completion_item_resolve,        true,  "completionProvider"    },
    { "exit",                              ilsp_handle_exit,                           false, NULL                    },
    { "initialize",                        ilsp_handle_initialize,                     true,  NULL                    },
    { "initialized",                       ilsp_handle_initialized,                    false, NULL                    },
    { "shutdown",                          ilsp_handle_shutdown,                       true,  NULL                    },
    /* Phase 4 Plan 04-04 (EDIT-07): textDocument/codeAction. Sort:
     * 'codeAction' vs 'completion' — common 'co', then 'd'(0x64) <
     * 'm'(0x6D), so codeAction < completion. */
    { "textDocument/codeAction",           ilsp_handle_text_document_code_action,      true,  "codeActionProvider"    },
    /* Phase 4 Plan 04-02 (EDIT-01): textDocument/completion. Sort:
     * "completion" < "declaration" because 'c'(0x63) < 'd'(0x64). */
    { "textDocument/completion",           ilsp_handle_text_document_completion,       true,  "completionProvider"    },
    /* Plan 03 Task 02 (NAV-03): declaration. */
    { "textDocument/declaration",          ilsp_handle_text_document_declaration,      true,  "declarationProvider"   },
    /* Plan 03 Task 02 (NAV-02): definition. */
    { "textDocument/definition",           ilsp_handle_text_document_definition,       true,  "definitionProvider"    },
    /* Plan 05 pull-diagnostic row: "textDocument/diagnostic" sorts BEFORE
     * "textDocument/did*" ('a' 0x61 < 'd' 0x64). Capability flows through
     * the handler-table -> ServerCapabilities pipeline as
     * "diagnosticProvider" (capabilities.c special-cases it). */
    { "textDocument/diagnostic",           ilsp_handle_text_document_diagnostic,       true,  "diagnosticProvider"    },
    { "textDocument/didChange",            ilsp_handle_didChange,                      false, "textDocumentSync"      },
    { "textDocument/didClose",             ilsp_handle_didClose,                       false, "textDocumentSync"      },
    { "textDocument/didOpen",              ilsp_handle_didOpen,                        false, "textDocumentSync"      },
    { "textDocument/didSave",              ilsp_handle_didSave,                        false, "textDocumentSync"      },
    /* Phase 4 Plan 04-07 (EDIT-13): documentHighlight. Sort:
     * "documentHighlight" vs "documentSymbol" — common "document",
     * then 'H'(0x48) < 'S'(0x53), so documentHighlight precedes
     * documentSymbol. */
    { "textDocument/documentHighlight",    ilsp_handle_text_document_document_highlight, true, "documentHighlightProvider"},
    /* Plan 03 Task 03 (NAV-07): documentSymbol. */
    { "textDocument/documentSymbol",       ilsp_handle_text_document_document_symbol,  true,  "documentSymbolProvider"},
    /* Phase 4 Plan 04-07 (EDIT-14): foldingRange. Sort: 'f' (0x66)
     * sits between 'documentSymbol' ('d'<'f') and 'hover' ('f'<'h'). */
    { "textDocument/foldingRange",         ilsp_handle_text_document_folding_range,    true,  "foldingRangeProvider"  },
    /* Phase 5 Plan 05-02 (FMT-02): textDocument/formatting. Sort:
     * "foldingRange" vs "formatting" -- common "fo", then 'l'(0x6C) <
     * 'r'(0x72), so foldingRange precedes formatting; formatting vs
     * hover -- 'f'(0x66) < 'h'(0x68). */
    { "textDocument/formatting",           ilsp_handle_text_document_formatting,       true,  "documentFormattingProvider" },
    /* Plan 04 Task 02 (NAV-09): hover. */
    { "textDocument/hover",                ilsp_handle_text_document_hover,            true,  "hoverProvider"         },
    /* Plan 05 Task 01 (NAV-05): implementation. 'h' < 'i' < 'r' sort. */
    { "textDocument/implementation",       ilsp_handle_text_document_implementation,   true,  "implementationProvider"},
    /* Phase 5 Plan 05-02 (FMT-04 STUB; Plan 05-04 fills body):
     * textDocument/onTypeFormatting. Sort: 'i'(0x69) < 'o'(0x6F) <
     * 'p'(0x70). */
    { "textDocument/onTypeFormatting",     ilsp_handle_text_document_on_type_formatting, true, "documentOnTypeFormattingProvider" },
    /* Phase 4 Plan 04-06 (EDIT-10): prepareRename. Sort: 'prepareR' vs
     * 'prepareT' — common 'prepare', then 'R'(0x52) < 'T'(0x54), so
     * prepareRename precedes prepareTypeHierarchy. */
    { "textDocument/prepareRename",        ilsp_handle_text_document_prepare_rename,   true,  "renameProvider"        },
    /* Plan 05 Task 02 (NAV-11): prepareTypeHierarchy. 'i' < 'p' < 'r'. */
    { "textDocument/prepareTypeHierarchy", ilsp_handle_text_document_prepare_type_hierarchy, true, "typeHierarchyProvider"},
    /* Phase 5 Plan 05-02 (FMT-03 STUB; Plan 05-03 fills body):
     * textDocument/rangeFormatting. Sort: 'p'(0x70) < 'r'(0x72);
     * within r-prefix, "rangeFormatting" vs "references" -- common 'r',
     * then 'a'(0x61) < 'e'(0x65), so rangeFormatting precedes
     * references. */
    { "textDocument/rangeFormatting",      ilsp_handle_text_document_range_formatting, true,  "documentRangeFormattingProvider" },
    /* Plan 04 Task 01 (NAV-06): references. */
    { "textDocument/references",           ilsp_handle_text_document_references,       true,  "referencesProvider"    },
    /* Phase 4 Plan 04-06 (EDIT-11): rename. Sort: 'references' vs
     * 'rename' — common 're', then 'f'(0x66) < 'n'(0x6E), so
     * references precedes rename; 'rename' vs 'signatureHelp' — 'r'
     * (0x72) < 's' (0x73). */
    { "textDocument/rename",               ilsp_handle_text_document_rename,           true,  "renameProvider"        },
    /* Phase 4 Plan 04-07 (EDIT-15): selectionRange. Sort:
     * "selectionRange" vs "signatureHelp" — common "s", then 'e'
     * (0x65) < 'i' (0x69), so selectionRange precedes signatureHelp.
     * "rename" vs "selectionRange" — 'r' (0x72) < 's' (0x73). */
    { "textDocument/selectionRange",       ilsp_handle_text_document_selection_range,  true,  "selectionRangeProvider"},
    /* Plan 04 Task 03 (NAV-10): signatureHelp. */
    { "textDocument/signatureHelp",        ilsp_handle_text_document_signature_help,   true,  "signatureHelpProvider" },
    /* Plan 03 Task 02 (NAV-04): typeDefinition. */
    { "textDocument/typeDefinition",       ilsp_handle_text_document_type_definition,  true,  "typeDefinitionProvider"},
    /* Plan 05 Task 02 (NAV-11): typeHierarchy sub/supertypes.
     * Sort: textDocument/... < typeHierarchy/... because 'e' (0x65)
     * < 'y' (0x79) in the 'x'/'y' position. subtypes < supertypes
     * because 'b' (0x62) < 'p' (0x70) at the 15th char. */
    { "typeHierarchy/subtypes",            ilsp_handle_type_hierarchy_subtypes,        true,  "typeHierarchyProvider" },
    { "typeHierarchy/supertypes",          ilsp_handle_type_hierarchy_supertypes,      true,  "typeHierarchyProvider" },
    /* Plan 06 (NAV-12): workspace/diagnostic pull. Sort: 'a' (0x61) < 'd'
     * (0x64) so "workspace/diagnostic" precedes "workspace/didChangeWatchedFiles".
     * Capability handled by capabilities.c's diagnosticProvider special-case
     * (workspaceDiagnostics=true in Plan 06). caps_has dedups. */
    { "workspace/diagnostic",              ilsp_handle_workspace_diagnostic,           true,  "diagnosticProvider"    },
    { "workspace/didChangeWatchedFiles",   ilsp_handle_didChangeWatchedFiles,          false, NULL                    },
    /* Phase 14 Plan 14-02 (CMD-01, CMD-03): workspace/executeCommand.
     * Sort: 'e' (0x65) < 's' (0x73) so executeCommand precedes symbol;
     * 'd' (0x64) < 'e' (0x65) so didChangeWatchedFiles precedes executeCommand.
     * Capability handled via caps_add override in capabilities.c (Phase 3 D-13
     * signatureHelpProvider precedent -- auto-derive boolean cannot produce
     * the nested object shape { commands: ["iron.migrate"] }). */
    { "workspace/executeCommand",          ilsp_handle_workspace_execute_command,      true,  "executeCommandProvider"},
    /* Plan 03 Task 03 (NAV-08): workspace/symbol. */
    { "workspace/symbol",                  ilsp_handle_workspace_symbol,               true,  "workspaceSymbolProvider"},
};
const size_t ilsp_handler_table_size =
    sizeof(ilsp_handler_table) / sizeof(ilsp_handler_table[0]);

static int handler_compare(const void *key, const void *entry) {
    const char *method = (const char *)key;
    const IronLsp_HandlerEntry *e = (const IronLsp_HandlerEntry *)entry;
    return strcmp(method, e->method);
}

const IronLsp_HandlerEntry *ilsp_handler_lookup(const char *method) {
    if (!method) return NULL;
    return (const IronLsp_HandlerEntry *)bsearch(
        method, ilsp_handler_table, ilsp_handler_table_size,
        sizeof(ilsp_handler_table[0]), handler_compare);
}

/* ── Error response helpers ─────────────────────────────────────────────── */

/* Clone the request id (int/uint/str/null) into the response doc. */
static yyjson_mut_val *clone_id(yyjson_mut_doc *rd, yyjson_val *id) {
    if (!id || yyjson_is_null(id)) return yyjson_mut_null(rd);
    if (yyjson_is_int(id) || yyjson_is_sint(id))
        return yyjson_mut_sint(rd, yyjson_get_sint(id));
    if (yyjson_is_uint(id))
        return yyjson_mut_uint(rd, yyjson_get_uint(id));
    if (yyjson_is_str(id))
        return yyjson_mut_strcpy(rd, yyjson_get_str(id));
    return yyjson_mut_null(rd);
}

/* Build and enqueue a JSON-RPC error response. `id` may be NULL. */
static void enqueue_error(IronLsp_Server *s,
                          Iron_Arena     *arena,
                          yyjson_val     *id,
                          int             code,
                          const char     *message) {
    yyjson_alc      alc = ilsp_json_alc(arena);
    yyjson_mut_doc *rd  = yyjson_mut_doc_new(&alc);
    if (!rd) return;
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_doc_set_root(rd, root);

    yyjson_mut_obj_add_strcpy(rd, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val   (rd, root, "id", clone_id(rd, id));

    yyjson_mut_val *err = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_int   (rd, err, "code",    code);
    yyjson_mut_obj_add_strcpy(rd, err, "message", message);
    yyjson_mut_obj_add_val   (rd, root, "error", err);

    size_t len = 0;
    char *body = ilsp_json_write_mut(rd, arena, &len);
    if (!body) return;
    char *heap_body = (char *)malloc(len);
    if (!heap_body) return;
    memcpy(heap_body, body, len);
    ilsp_writer_enqueue(s->writer, ILSP_PRIO_RESPONSE, heap_body, len);
}

/* ── Top-level dispatch ─────────────────────────────────────────────────── */

void ilsp_dispatch_route(IronLsp_Server *server,
                         const char     *body,
                         size_t          len,
                         Iron_Arena     *arena) {
    if (!server || !body || !arena) return;

    yyjson_read_err err;
    memset(&err, 0, sizeof(err));
    yyjson_doc *doc = ilsp_json_parse(body, len, arena, &err);
    if (!doc) {
        /* Parse error: per JSON-RPC 2.0 §5 the id is null in this case. */
        enqueue_error(server, arena, NULL, -32700, "ParseError");
        return;
    }

    yyjson_val *root   = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        enqueue_error(server, arena, NULL, -32600, "InvalidRequest");
        return;
    }

    yyjson_val *method_v = yyjson_obj_get(root, "method");
    yyjson_val *id       = yyjson_obj_get(root, "id");
    const char *method   = method_v ? yyjson_get_str(method_v) : NULL;
    bool is_request      = (id != NULL);

    if (!method) {
        if (is_request) enqueue_error(server, arena, id, -32600, "InvalidRequest");
        return;
    }

    /* Lifecycle gate FIRST (CORE-05). Per LSP 3.17 §lifecycle the server
     * must reject any non-initialize request before init with
     * ServerNotInitialized -- regardless of whether the method is
     * registered. An unknown method in an allowed-state yields
     * MethodNotFound (below). */
    if (!ilsp_lifecycle_allow_request(server->lifecycle, method)) {
        if (is_request) {
            /* UNINIT + any non-`initialize` => ServerNotInitialized.
             * INITIALIZING + any non-`initialized` => also pre-init, same code.
             * Anything else (duplicate initialize, post-shutdown request)
             * => InvalidRequest per spec. */
            int code = (server->lifecycle == ILSP_LIFECYCLE_UNINIT ||
                        server->lifecycle == ILSP_LIFECYCLE_INITIALIZING)
                       ? -32002 : -32600;
            const char *msg = (code == -32002) ? "ServerNotInitialized" : "InvalidRequest";
            enqueue_error(server, arena, id, code, msg);
        }
        return;
    }

    const IronLsp_HandlerEntry *entry = ilsp_handler_lookup(method);
    if (!entry) {
        if (is_request) enqueue_error(server, arena, id, -32601, "MethodNotFound");
        return;
    }

    /* Phase 7 Plan 07-01 (HARD-14, D-02): push the (id, method) pair into
     * the crash-dump in-flight ring buffer. For requests we use the JSON-RPC
     * id if integral; for notifications (id==NULL) we synthesise a monotonic
     * non-zero id via a static counter so the ring can still record the
     * method in the event of a crash inside a notification handler. */
    static _Atomic uint64_t s_notif_counter = 0;
    uint64_t ring_id = 0;
    if (id) {
        if (yyjson_is_int(id) || yyjson_is_sint(id)) {
            int64_t v = yyjson_get_sint(id);
            ring_id = (v > 0) ? (uint64_t)v : (uint64_t)(-v);
        } else if (yyjson_is_uint(id)) {
            ring_id = yyjson_get_uint(id);
        } else {
            /* String or unusual id -- synthesise from counter. */
            ring_id = atomic_fetch_add_explicit(&s_notif_counter, 1,
                                                memory_order_relaxed) + 1;
        }
    } else {
        ring_id = atomic_fetch_add_explicit(&s_notif_counter, 1,
                                            memory_order_relaxed) + 1;
    }
    if (ring_id == 0) ring_id = 1;
    ilsp_crash_ring_push(ring_id, method);

    /* Dispatch. */
    entry->handler(server, doc, arena);

    ilsp_crash_ring_pop(ring_id);

    /* Advance state machine. */
    server->lifecycle = ilsp_lifecycle_next(server->lifecycle, method);
}
