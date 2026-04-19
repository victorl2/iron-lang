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

/* Phase 5 Plan 05-03 (FMT-03, D-04, D-06): range formatting.
 *
 * For every top-level Iron_Program decl whose line-span intersects
 * `range` (any overlap), re-render that decl via
 * iron_print_ast(decl, opts, arena) and emit ONE TextEdit replacing
 * the decl's full-line span.
 *
 * Sorts emitted edits descending by span start offset so clients
 * apply later edits first (prevents offset invalidation during apply).
 *
 * Returns an empty list on:
 *   - parse error (D-03 refusal mirrors full-doc)
 *   - range covers only blank lines / between decls
 *   - NULL doc / NULL arena / inverted range
 *   - cancel observed at any stage boundary (4 polls + per-decl poll)
 *
 * Option resolution priority: explicit `opts_in` > `ws->fmt_opts`
 * cached snapshot > built-in defaults. */
IronLsp_TextEditList ilsp_facade_format_range(
    const struct IronLsp_Document       *doc,
    const struct IronLsp_WorkspaceIndex *ws,
    IronLsp_Range                        range,    /* LSP coords, 0-based */
    const IronFmtOptions                *opts_in,  /* may be NULL */
    Iron_Arena                          *arena,
    const _Atomic bool                  *cancel);

/* Phase 5 Plan 05-04 (FMT-04, D-05, D-14): on-type formatting.
 *
 * When the user types `}` inside an enclosing block and the editor has
 * `editor.formatOnType` enabled, the client sends
 * textDocument/onTypeFormatting with the trigger char and the caret
 * position. We walk the AST from Iron_Program.decls[] to find the
 * deepest enclosing block whose span covers `pos`, re-compute per-line
 * minimal indent edits for every non-blank line in that block whose
 * current leading whitespace differs from the canonical indent, and
 * return those edits sorted descending by line (D-06).
 *
 * Returns an empty list on:
 *   - trigger_char != '}' (v1 policy per D-05; other triggers deferred)
 *   - parse error (D-03 refusal mirrors full-doc)
 *   - no enclosing block at `pos` (typed in string literal, between
 *     decls, malformed source, `}` typed before an existing brace --
 *     RESEARCH Pitfall 5 + Claude's Discretion §381)
 *   - NULL doc / NULL arena
 *   - cancel observed at any stage / per-line boundary (D-12 / D-17)
 *
 * Option resolution priority: explicit `opts_in` > `ws->fmt_opts` cached
 * snapshot > built-in defaults.
 *
 * Single-call-site preservation (D-10): this entry uses the
 * ilsp_facade_fmt_lex_parse helper from format_internal.h -- it does
 * NOT call iron_format_source. The grep invariant stays at 1. */
IronLsp_TextEditList ilsp_facade_format_on_type(
    const struct IronLsp_Document       *doc,
    const struct IronLsp_WorkspaceIndex *ws,
    IronLsp_Position                     pos,
    char                                 trigger_char,
    const IronFmtOptions                *opts_in,  /* may be NULL */
    Iron_Arena                          *arena,
    const _Atomic bool                  *cancel);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_FACADE_FMT_FORMAT_H */
