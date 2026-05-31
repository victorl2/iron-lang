#ifndef ILSP_COMPLETE_DEFER_FREE_H
#define ILSP_COMPLETE_DEFER_FREE_H

/* Phase 34 LSP-04 (Plan 34-03) — `defer free <binding>` snippet helpers.
 *
 * Surfaces the backward-scan that walks the cursor's enclosing function
 * body collecting up to N most-recent heap/rc-allocated `val|var <name>`
 * bindings. The orchestrator in complete.c calls
 * ilsp_collect_recent_heap_rc_bindings once per request; the test driver
 * (tests/lsp/complete/test_complete_defer_free_snippet.c) exercises the
 * same helper against synthetic programs.
 *
 * CORE-22 reminder: this helper consumes the Iron_Program returned by
 * the single ilsp_facade_compile_for_nav call in complete.c — it does
 * NOT invoke iron_analyze_buffer.
 */

#include <stddef.h>
#include <stdint.h>

#include "parser/ast.h"
#include "util/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cap on number of recent heap/rc bindings we surface as snippet
 * candidates. Matches the CONTEXT.md "last 5 statements" rule. */
#define ILSP_DEFER_FREE_MAX_CANDIDATES 5

/* Walk `program` backward from (cursor_line_1, 1-indexed) within the
 * enclosing function body, collecting up to `out_cap` heap/rc binding
 * names. Names are arena-strdup'd into `arena`. Most-recent binding
 * appears at out_names[0]. Returns count written.
 *
 * If `program` is NULL, the cursor is outside any function body, or
 * no eligible bindings exist, returns 0 and leaves out_names untouched.
 */
size_t ilsp_collect_recent_heap_rc_bindings(const Iron_Program *program,
                                              uint32_t            cursor_line_1,
                                              Iron_Arena         *arena,
                                              const char        **out_names,
                                              size_t              out_cap);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_COMPLETE_DEFER_FREE_H */
