#ifndef IRON_LSP_SERVER_SERVER_H
#define IRON_LSP_SERVER_SERVER_H

/* Phase 2 Plan 03 -- Shared IronLsp_Server struct body.
 *
 * This header is the single source of truth for the server-singleton's
 * shape. Every TU that dereferences `server->writer` / `server->cancels` /
 * etc. includes this header. The individual subsystem structs (writer,
 * reader, cancel registry, dyn register) remain opaque -- only their
 * pointer types appear here, forward-declared from their own heads.
 *
 * Mutation discipline (between transport-thread start and join):
 *   - lifecycle:         main dispatcher thread only
 *   - writer:            thread-safe (internal mutex)
 *   - reader:            thread-safe (owns its own thread + shutdown flag)
 *   - cancels:           thread-safe (internal mutex)
 *   - dyn_reg:           main dispatcher thread only (writes once post-initialized)
 *   - position_encoding: const-after-initialize
 *   - next_request_id:   atomic (server-originated request counter)
 */

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#include "lsp/server/lifecycle.h"
#include "lsp/facade/types.h"    /* IronLsp_PositionEncoding */

/* Forward declarations -- each subsystem owns its own struct body. */
typedef struct IronLsp_Writer         IronLsp_Writer;
typedef struct IronLsp_Reader         IronLsp_Reader;
typedef struct IronLsp_CancelRegistry IronLsp_CancelRegistry;
typedef struct IronLsp_DynRegister    IronLsp_DynRegister;
typedef struct IronLsp_Document       IronLsp_Document;   /* Plan 04 */
typedef struct IronLsp_WorkerPool     IronLsp_WorkerPool; /* Plan 05 */
typedef struct IronLsp_WorkspaceIndex IronLsp_WorkspaceIndex; /* Phase 3 Plan 02 */
typedef struct IronLsp_WsDiagCache    IronLsp_WsDiagCache;    /* Phase 3 Plan 06 */

/* Compile-time shared server state. Singleton; owned by main.c (Plan 06);
 * mutated ONLY before transport threads start and AFTER they join. */
typedef struct IronLsp_Server {
    IronLsp_LifecycleState    lifecycle;
    IronLsp_Writer           *writer;
    IronLsp_Reader           *reader;
    IronLsp_CancelRegistry   *cancels;
    IronLsp_DynRegister      *dyn_reg;
    IronLsp_PositionEncoding  position_encoding;

    /* Plan 04: URI -> IronLsp_Document* registry (stb_ds sh_new_strdup
     * map). Mutated ONLY on the main dispatcher thread in Plan 04; Plan 05
     * will add per-doc mailbox access from worker threads with a coarser
     * lock discipline. */
    struct { char *key; IronLsp_Document *value; } *documents;

    /* Plan 04: discovered workspace root (malloc'd path to the directory
     * containing iron.toml). NULL if not yet discovered (no
     * workspaceFolders in the initialize params, or no iron.toml found
     * up the tree). */
    char                     *workspace_root;

    /* Plan 05: optional per-document worker pool abstraction. main.c may
     * manage per-doc workers directly via the `documents` map's
     * `IronLsp_Document.worker_thread` field; either discipline is
     * acceptable. For Plan 05 this pointer is only initialized when a
     * top-level pool is introduced (not required in the document-owned
     * worker model actually used by handlers_document.c). */
    IronLsp_WorkerPool       *workers;

    /* Phase 3 Plan 02: workspace index (per-file parsed/analyzed cache,
     * stdlib cache pointer, dep map). Created in handlers_lifecycle.c on
     * `initialize` once workspace_root is known; warm-seeded on a
     * detached thread after `initialized`; destroyed in main.c teardown.
     * NULL until `initialize` resolves a workspace root. */
    IronLsp_WorkspaceIndex   *workspace_index;

    /* Phase 3 Plan 06 (NAV-12, D-12): workspace/diagnostic per-file cache.
     * Created alongside workspace_index in handlers_lifecycle.c;
     * destroyed in main.c teardown. NULL until workspace_index is
     * created. Keyed on canonical_path; LRU=200; resultId embeds the
     * monotonic workspace_version counter for client correlation. */
    IronLsp_WsDiagCache      *ws_diag_cache;

    /* Atomic request-id counter for server-originated requests
     * (e.g., client/registerCapability from the dyn-register subsystem). */
    _Atomic uint64_t          next_request_id;

    /* Phase 4 Plan 04-03 Task 03 (EDIT-04 D-15, RESEARCH §3.1): client
     * capability sniff results cached at `initialize` time. Both
     * default to false; populated only when the corresponding
     * capability path is present + true in the client's
     * ClientCapabilities JSON.
     *   client_supports_snippet -- textDocument.completion
     *       .completionItem.snippetSupport. Gates snippet-format
     *       insertText; when false the orchestrator falls back to
     *       PlainText with the plain label.
     *   client_supports_document_changes -- workspace.workspaceEdit
     *       .documentChanges. Observed here for Plan 04-06 rename +
     *       future WorkspaceEdit.documentChanges emissions.
     * const-after-initialize (mutated only in ilsp_handle_initialize). */
    bool                      client_supports_snippet;
    bool                      client_supports_document_changes;
} IronLsp_Server;

#endif /* IRON_LSP_SERVER_SERVER_H */
