/* Phase 4 Plan 04-04 Task 01 (EDIT-07) — quickfix for
 * IRON_WARN_UNUSED_IMPORT (611).
 *
 * Recipe (D-06 §2):
 *   title        = "Remove unused import"
 *   range        = full-line span from diag->span.line — start at
 *                  (line-1, 0), end at (line, 0) so the trailing \n is
 *                  included and the line is fully deleted. If the
 *                  following line is blank (canonical fmt inserts a
 *                  blank line after imports), the range extends through
 *                  it so the post-apply source remains iron-fmt-clean
 *                  (Phase 5 Plan 05-05 D-07 gate requirement).
 *   newText      = "" (empty, delete)
 *   isPreferred  = false
 *
 * The compiler seeds diag->suggestion = "" (empty-string delete-line
 * sentinel, Plan 04-01 Task 02); we ignore suggestion value and derive
 * the delete range from diag->span.line.
 */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"

#include <string.h>

/* Return true if the 0-indexed `line` in the document consists only of
 * whitespace (blank line). Beyond-EOF lines are treated as non-blank
 * (nothing to eat). */
static bool line_is_blank(const struct IronLsp_Document *doc, uint32_t line) {
    if (!doc || !doc->text) return false;
    size_t start = ilsp_byte_of_line(&doc->line_idx, line);
    size_t next  = ilsp_byte_of_line(&doc->line_idx, line + 1);
    if (start > doc->text_len) return false;
    if (next  > doc->text_len) next = doc->text_len;
    if (next <= start) return false;
    /* Exclude the trailing \n from the scan. */
    size_t end = next;
    if (end > start && doc->text[end - 1] == '\n') end--;
    for (size_t i = start; i < end; i++) {
        char c = doc->text[i];
        if (c != ' ' && c != '\t' && c != '\r') return false;
    }
    return true;
}

void ilsp_quickfix_unused_import(const Iron_Diagnostic           *diag,
                                    struct IronLsp_Document         *doc,
                                    struct IronLsp_WorkspaceIndex   *wi,
                                    Iron_Arena                      *arena,
                                    IronLsp_CodeAction              *out) {
    (void)wi;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!diag || !arena) return;
    if (diag->span.line == 0) return;  /* no anchor line; skip */

    /* Iron_Span.line is 1-indexed. LSP is 0-indexed. Delete from
     * (line-1, 0) to (line, 0) so the line including the trailing \n
     * disappears entirely. */
    uint32_t del_start_line = diag->span.line - 1;
    uint32_t del_end_line   = diag->span.line;     /* next line, col 0 */

    /* Phase 5 Plan 05-05 (D-07): to keep the post-apply source iron-fmt
     * clean, if the next line is blank (canonical fmt inserts a blank
     * line separating an import run from subsequent decls), swallow it
     * too -- otherwise we leave behind a leading / orphan blank line
     * that fmt would strip. Safe to skip if `doc` is absent (Unity
     * tests that drive the handler directly may pass NULL). */
    if (doc && line_is_blank(doc, del_end_line)) {
        del_end_line += 1;
    }

    out->title            = "Remove unused import";
    out->kind             = "quickfix";
    out->originating_diag = diag;
    out->is_preferred     = false;
    out->edit_start_line  = del_start_line;
    out->edit_start_char  = 0;
    out->edit_end_line    = del_end_line;
    out->edit_end_char    = 0;
    out->edit_new_text    = iron_arena_strdup(arena, "", 0);
    if (!out->edit_new_text) {
        /* iron_arena_strdup of empty may return non-NULL zero-byte, but
         * guard against NULL by using a static empty string. */
        out->edit_new_text = "";
    }
}
