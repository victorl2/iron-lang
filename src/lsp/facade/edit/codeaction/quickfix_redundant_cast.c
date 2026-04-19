/* Phase 4 Plan 04-04 Task 01 (EDIT-07) — quickfix for
 * IRON_WARN_REDUNDANT_CAST (612).
 *
 * Recipe (D-06 §5):
 *   title        = "Remove redundant cast"
 *   range        = diag->span (the full `expr as T` span)
 *   newText      = diag->suggestion (the bare inner expression text,
 *                  e.g. "1.0" for `Float(1.0)`, populated by the
 *                  redundant-cast check in typecheck.c, Plan 04-01)
 *   isPreferred  = false
 */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/span.h"
#include "lsp/store/document.h"

#include <string.h>

void ilsp_quickfix_redundant_cast(const Iron_Diagnostic           *diag,
                                     struct IronLsp_Document         *doc,
                                     struct IronLsp_WorkspaceIndex   *wi,
                                     Iron_Arena                      *arena,
                                     IronLsp_CodeAction              *out) {
    (void)wi;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!diag || !doc || !arena) return;
    if (!diag->suggestion) return;  /* need the inner-expr text */

    IronLsp_Range r = ilsp_span_to_lsp_range(diag->span, doc, ILSP_ENC_UTF8);

    out->title            = "Remove redundant cast";
    out->kind             = "quickfix";
    out->originating_diag = diag;
    out->is_preferred     = false;
    out->edit_start_line  = r.start.line;
    out->edit_start_char  = r.start.character;
    out->edit_end_line    = r.end.line;
    out->edit_end_char    = r.end.character;
    out->edit_new_text    = iron_arena_strdup(arena, diag->suggestion,
                                                 strlen(diag->suggestion));
}
