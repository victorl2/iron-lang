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
#include "lsp/transport/json.h"
#include "lsp/transport/writer.h"
#include "lsp/transport/types.h"
#include "vendor/yyjson/yyjson.h"
#include "util/arena.h"

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
    { "exit",                              ilsp_handle_exit,                           false, NULL                    },
    { "initialize",                        ilsp_handle_initialize,                     true,  NULL                    },
    { "initialized",                       ilsp_handle_initialized,                    false, NULL                    },
    { "shutdown",                          ilsp_handle_shutdown,                       true,  NULL                    },
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
    /* Plan 03 Task 03 (NAV-07): documentSymbol. */
    { "textDocument/documentSymbol",       ilsp_handle_text_document_document_symbol,  true,  "documentSymbolProvider"},
    /* Plan 04 Task 02 (NAV-09): hover. */
    { "textDocument/hover",                ilsp_handle_text_document_hover,            true,  "hoverProvider"         },
    /* Plan 04 Task 01 (NAV-06): references. */
    { "textDocument/references",           ilsp_handle_text_document_references,       true,  "referencesProvider"    },
    /* Plan 04 Task 03 (NAV-10): signatureHelp. */
    { "textDocument/signatureHelp",        ilsp_handle_text_document_signature_help,   true,  "signatureHelpProvider" },
    /* Plan 03 Task 02 (NAV-04): typeDefinition. */
    { "textDocument/typeDefinition",       ilsp_handle_text_document_type_definition,  true,  "typeDefinitionProvider"},
    { "workspace/didChangeWatchedFiles",   ilsp_handle_didChangeWatchedFiles,          false, NULL                    },
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

    /* Dispatch. */
    entry->handler(server, doc, arena);

    /* Advance state machine. */
    server->lifecycle = ilsp_lifecycle_next(server->lifecycle, method);
}
