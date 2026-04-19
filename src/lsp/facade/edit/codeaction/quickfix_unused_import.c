/* Phase 4 Plan 04-04 Task 01 (EDIT-07) — quickfix for
 * IRON_WARN_UNUSED_IMPORT (611).
 *
 * Recipe (D-06 §2):
 *   title        = "Remove unused import"
 *   range        = full-line span from diag->span.line — start at
 *                  (line-1, 0), end at (line, 0) so the trailing \n is
 *                  included and the line is fully deleted.
 *   newText      = "" (empty, delete)
 *   isPreferred  = false
 *
 * The compiler seeds diag->suggestion = "" (empty-string delete-line
 * sentinel, Plan 04-01 Task 02); we ignore suggestion value and derive
 * the delete range from diag->span.line.
 */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/store/document.h"

#include <string.h>

void ilsp_quickfix_unused_import(const Iron_Diagnostic           *diag,
                                    struct IronLsp_Document         *doc,
                                    struct IronLsp_WorkspaceIndex   *wi,
                                    Iron_Arena                      *arena,
                                    IronLsp_CodeAction              *out) {
    (void)wi;
    (void)doc;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!diag || !arena) return;
    if (diag->span.line == 0) return;  /* no anchor line; skip */

    /* Iron_Span.line is 1-indexed. LSP is 0-indexed. Delete from
     * (line-1, 0) to (line, 0) so the line including the trailing \n
     * disappears entirely. */
    out->title            = "Remove unused import";
    out->kind             = "quickfix";
    out->originating_diag = diag;
    out->is_preferred     = false;
    out->edit_start_line  = diag->span.line - 1;
    out->edit_start_char  = 0;
    out->edit_end_line    = diag->span.line;     /* next line, col 0 */
    out->edit_end_char    = 0;
    out->edit_new_text    = iron_arena_strdup(arena, "", 0);
    if (!out->edit_new_text) {
        /* iron_arena_strdup of empty may return non-NULL zero-byte, but
         * guard against NULL by using a static empty string. */
        out->edit_new_text = "";
    }
}
