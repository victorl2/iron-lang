#ifndef IRON_LSP_SERVER_DISPATCH_H
#define IRON_LSP_SERVER_DISPATCH_H

/* Phase 2 Plan 03 Task 01 -- Dispatcher: compile-time const handler table
 * with bsearch lookup. Lifecycle-aware: every inbound message runs through
 * ilsp_dispatch_route which parses JSON, checks the lifecycle FSM, and
 * routes to a handler (or emits a JSON-RPC error response).
 *
 * The handler table is a sorted const array (analog: src/lexer/lexer.c's
 * kw_table + bsearch pattern, ll. 28-79). Every advertised server
 * capability is derived from this table (CORE-06; PITFALLS.md #15 mitigation):
 * an entry with `capability != NULL` both registers a handler AND advertises
 * the corresponding capability in the `initialize` response. */

#include <stdbool.h>
#include <stddef.h>

#include "util/arena.h"   /* Iron_Arena (typedef'd without struct tag) */

/* Forward decls -- heavy structs live in their own headers. */
struct yyjson_doc;
typedef struct IronLsp_Server IronLsp_Server;

/* Handler signature. Handlers receive the parsed JSON doc (whose
 * lifetime is the per-request arena) and the arena itself so they can
 * allocate their response via ilsp_json_write_mut. */
typedef void (*IronLsp_HandlerFn)(IronLsp_Server    *server,
                                  struct yyjson_doc *doc,
                                  Iron_Arena        *arena);

/* One row of the compile-time handler table. */
typedef struct IronLsp_HandlerEntry {
    const char        *method;      /* literal; rows kept alphabetical for bsearch */
    IronLsp_HandlerFn  handler;
    bool               is_request;  /* true => response expected; false => notification */
    const char        *capability;  /* server capability name advertised by this handler;
                                     * NULL for lifecycle / cancel handlers */
} IronLsp_HandlerEntry;

/* Compile-time const table + its length. Defined in dispatch.c; referenced
 * by capabilities.c to build ServerCapabilities. */
extern const IronLsp_HandlerEntry ilsp_handler_table[];
extern const size_t               ilsp_handler_table_size;

/* bsearch-based lookup. Returns NULL if no handler is registered. */
const IronLsp_HandlerEntry *ilsp_handler_lookup(const char *method);

/* Top-level dispatch entry point.
 *   1. Parse JSON; on failure enqueue -32700 ParseError response.
 *   2. Read method + id.
 *   3. Look up handler. Unknown => -32601 MethodNotFound (if request).
 *   4. Check lifecycle gate. Violation => -32002 or -32600.
 *   5. Invoke handler.
 *   6. Advance lifecycle state via ilsp_lifecycle_next.
 * All error responses are enqueued via server->writer. */
void ilsp_dispatch_route(IronLsp_Server *server,
                         const char     *body,
                         size_t          len,
                         Iron_Arena     *arena);

/* -- Legacy umbrella-header forward-decl stubs (Plan 01 shim) ---------- */
struct IronLsp_Dispatcher;

#endif /* IRON_LSP_SERVER_DISPATCH_H */
