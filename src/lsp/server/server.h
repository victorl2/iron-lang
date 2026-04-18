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

    /* Plan 05 adds: */
    /* IronLsp_WorkerPool *workers; */

    /* Atomic request-id counter for server-originated requests
     * (e.g., client/registerCapability from the dyn-register subsystem). */
    _Atomic uint64_t          next_request_id;
} IronLsp_Server;

#endif /* IRON_LSP_SERVER_SERVER_H */
