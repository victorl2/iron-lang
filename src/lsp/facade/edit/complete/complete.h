#ifndef ILSP_FACADE_COMPLETE_H
#define ILSP_FACADE_COMPLETE_H

/* Phase 4 Plan 04-02 Task 02 (EDIT-01, D-01, D-17) -- completion facade
 * orchestrator.
 *
 * Pipeline:
 *   1. ilsp_facade_compile_for_nav to get the sealed Iron_Program
 *      (single iron_analyze_buffer call site invariant preserved).
 *   2. pos_to_byte conversion via store/utf.c.
 *   3. Extract query prefix (ident chars immediately before cursor).
 *   4. Classify context via ilsp_completion_context_classify.
 *   5. Build candidate list via ilsp_complete_buckets_build.
 *   6. qsort(bucket asc, -fuzzy asc, label asc).
 *   7. Cap at 128.
 *
 * Poll cancel at each step boundary (per D-17) + every 64 items inside
 * the inner fuzzy-match loop (handled by buckets.c).
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "lsp/facade/edit/complete/buckets.h"
#include "lsp/facade/types.h"   /* IronLsp_Position */

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_Server;
struct IronLsp_Document;

/* Full completion orchestrator. Returns up to 128 ranked candidates in
 * `*out`. `*out_is_incomplete` is currently always false (we always
 * return a complete ranked list up to the cap). */
void ilsp_facade_complete(struct IronLsp_Server          *server,
                            struct IronLsp_Document        *doc,
                            IronLsp_Position                pos,
                            _Atomic bool                   *cancel,
                            Iron_Arena                     *arena,
                            IronLsp_CompletionCandidate   **out,
                            size_t                         *out_n,
                            bool                           *out_is_incomplete);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_FACADE_COMPLETE_H */
