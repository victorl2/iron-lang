#ifndef ILSP_FACADE_COMPLETE_RESOLVE_H
#define ILSP_FACADE_COMPLETE_RESOLVE_H

/* Phase 4 Plan 04-02 Task 03 (EDIT-03, D-04) -- completionItem/resolve
 * facade.
 *
 * The initial textDocument/completion response carries a lightweight
 * `data` opaque handle per item. When the client highlights an item it
 * sends completionItem/resolve back with that handle; the server then
 * fills in `documentation` (markdown) + upgraded `detail` via the hover
 * renderer.
 *
 * NEVER errors. On stale / missing lookup data both outputs are NULL
 * and the handler returns the item unchanged (D-04 contract).
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/arena.h"   /* Iron_Arena typedef (anonymous struct) */

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_Server;

/* Opaque-data round-trip: this is the exact shape we embedded in
 * `data` on the outbound CompletionItem. The client echoes it back
 * verbatim. */
typedef struct {
    const char *canonical_path;
    const char *name_path;
    int         bucket;
    uint64_t    content_hash;
} IronLsp_CompletionResolveData;

/* Render markdown documentation + upgraded detail for a single
 * CompletionItem. NEVER errors: on cache miss returns detail_out=NULL,
 * documentation_out=NULL. Uses src/lsp/facade/hover.c-style signature +
 * doc-comment formatting per D-04. */
void ilsp_facade_completion_resolve(struct IronLsp_Server              *server,
                                       const IronLsp_CompletionResolveData *data,
                                       _Atomic bool                       *cancel,
                                       Iron_Arena                         *arena,
                                       const char                        **detail_out,
                                       const char                        **documentation_markdown_out);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_FACADE_COMPLETE_RESOLVE_H */
