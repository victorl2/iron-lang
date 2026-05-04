#ifndef ILSP_FACADE_SELECTION_RANGE_H
#define ILSP_FACADE_SELECTION_RANGE_H

/* Phase 4 Plan 04-07 Task 03 (EDIT-15, D-14) -- textDocument/selectionRange.
 *
 * Returns a linked-list "chain" of nested ranges per cursor position:
 *   innermost rung = word / identifier token span at the cursor
 *   outer rungs    = enclosing leaf expr -> expression ladder ->
 *                    statement -> block -> decl -> module (program).
 *
 * Parent pointer walks toward the module (outermost). Strict
 * containment invariant: each rung must be contained by its parent.
 *
 * Parser-only: no analyzer re-entry. Builds rungs by walking the
 * parse tree from the root toward the cursor's byte offset. EOF
 * cursor returns a 1-rung chain (module only). */

#include "lsp/facade/types.h"  /* IronLsp_Position */
#include "util/arena.h"        /* Iron_Arena typedef */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct IronLsp_Server;
struct IronLsp_Document;

#ifdef __cplusplus
extern "C" {
#endif

/* Linked-list rung. The outermost rung (module) has parent == NULL. */
typedef struct IronLsp_SelectionRange {
    uint32_t  range_start_line;     /* 0-indexed LSP line */
    uint32_t  range_start_char;
    uint32_t  range_end_line;
    uint32_t  range_end_char;
    struct IronLsp_SelectionRange *parent;  /* arena-owned, NULL at module */
} IronLsp_SelectionRange;

/* Facade entry. For each position in `positions[0..n_positions-1]`
 * returns the innermost rung via out[i]; `out[i]->parent` walks
 * outward. All rungs and the array itself are arena-allocated.
 *
 * T-4-2 mitigation: positions[] is clamped to 256 entries to bound
 * work per request. */
void ilsp_facade_selection_range(struct IronLsp_Server      *server,
                                   struct IronLsp_Document    *doc,
                                   const IronLsp_Position     *positions,
                                   size_t                      n_positions,
                                   _Atomic bool               *cancel,
                                   Iron_Arena                 *arena,
                                   IronLsp_SelectionRange   ***out,
                                   size_t                     *out_n);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_FACADE_SELECTION_RANGE_H */
