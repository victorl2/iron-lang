#ifndef ILSP_FACADE_FMT_FORMAT_H
#define ILSP_FACADE_FMT_FORMAT_H

/* Phase 5 Plan 05-02 (FMT-02, D-10, D-11, D-12): LSP formatting facade.
 *
 * `ilsp_facade_format_full` is the public entry point for
 * `textDocument/formatting`. It is the ONLY indirect path under
 * `src/lsp/` that reaches the compiler-side `iron_format_source`
 * library entry. The single-call-site invariant is enforced by the
 * `test_fmt_single_call_site` CTest grep-invariant (mirrors CORE-22
 * for analyze).
 *
 * Plans 05-03 (rangeFormatting) and 05-04 (onTypeFormatting) will
 * append `_range` and `_on_type` declarations and route them through
 * the same in-TU static helper to preserve the grep count.
 *
 * Threading: dispatcher-synchronous (D-12). Caller owns a per-request
 * Iron_Arena (HARD-06); the facade's TextEdit list points into that
 * arena. Cancel polling at lex / parse / print / emit boundaries
 * (4 polls -- D-12). */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "fmt/options.h"
#include "lsp/facade/types.h"
#include "util/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Single-edit shape used by every fmt facade entry point. The string
 * `new_text` is arena-owned and lives until the caller frees the arena;
 * callers (handlers_fmt.c) serialize it into yyjson before that point. */
typedef struct IronLsp_TextEdit {
    IronLsp_Range  range;
    const char    *new_text;
} IronLsp_TextEdit;

typedef struct IronLsp_TextEditList {
    IronLsp_TextEdit *edits;     /* arena-owned array; NULL when count==0 */
    size_t            count;
} IronLsp_TextEditList;

struct IronLsp_Document;
struct IronLsp_WorkspaceIndex;

/* Full-document formatting (FMT-02). Returns an empty list on:
 *   - parse error (D-03 refusal -- caller emits window/logMessage Info)
 *   - cancel observed at any of the 4 stage boundaries
 *   - NULL doc / NULL arena
 *
 * Option resolution priority: explicit `opts_in` > `ws->fmt_opts` cached
 * snapshot > built-in defaults. Workspace cache may be NULL (no
 * iron.toml) and `opts_in` may be NULL (handler did not pre-resolve);
 * the facade quietly falls through to defaults. */
IronLsp_TextEditList ilsp_facade_format_full(
    const struct IronLsp_Document       *doc,
    const struct IronLsp_WorkspaceIndex *ws,
    const IronFmtOptions                *opts_in,    /* may be NULL */
    Iron_Arena                          *arena,
    const _Atomic bool                  *cancel);

/* Plans 05-03 / 05-04 will append _range and _on_type entry points
 * declared here; they share the same in-TU static helper so the
 * single-call-site grep stays at exactly 1 hit. */

#ifdef __cplusplus
}
#endif

#endif /* ILSP_FACADE_FMT_FORMAT_H */
