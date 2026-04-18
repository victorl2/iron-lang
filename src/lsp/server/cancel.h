#ifndef IRON_LSP_SERVER_CANCEL_H
#define IRON_LSP_SERVER_CANCEL_H

/* Phase 2 Plan 03 Task 03 -- per-request cancellation registry.
 *
 * Every inbound request gets a heap-allocated _Atomic bool* flag keyed
 * by the stringified LSP id ("42" for int ids, the raw string for
 * string-typed ids). The dispatcher calls `register` on entry and
 * `unregister` on response; the `$/cancelRequest` handler calls
 * `signal` to flip the flag. ASTWorker threads (Plan 05) poll the flag
 * via the Phase 1 HARD-05 cancel primitive at every pipeline-stage
 * boundary in the analyzer.
 *
 * All three entry points are thread-safe (internal mutex). The flag
 * pointer returned by `register` is heap-owned by the registry until
 * `unregister` is called.
 *
 * Analogs:
 *   - src/lexer/lexer.c:12-18   cancel-poll helper (reused verbatim)
 *   - src/analyzer/scope.c:65-71 stb_ds string-keyed map shape */

#include <stdbool.h>
#include <stdatomic.h>

typedef struct IronLsp_CancelRegistry IronLsp_CancelRegistry;

IronLsp_CancelRegistry *ilsp_cancel_registry_create(void);
void                    ilsp_cancel_registry_destroy(IronLsp_CancelRegistry *r);

/* Register a request id. Returns a _Atomic bool* pointer (heap-owned by
 * the registry). Caller passes this pointer down to iron_analyze_buffer
 * as the cancel_flag argument. `id_key` is a stringified id (e.g. "42"
 * for an int request id). Re-registering the same id returns the
 * existing flag (idempotent for out-of-order cancel races). */
_Atomic bool *ilsp_cancel_register(IronLsp_CancelRegistry *r, const char *id_key);

/* Signal cancellation for the given id. Returns true if a flag was
 * found and flipped; false if the id was not registered. */
bool ilsp_cancel_signal(IronLsp_CancelRegistry *r, const char *id_key);

/* Unregister (call when a response is sent or when the request has
 * completed). Frees the heap-allocated atomic. */
void ilsp_cancel_unregister(IronLsp_CancelRegistry *r, const char *id_key);

#endif /* IRON_LSP_SERVER_CANCEL_H */
