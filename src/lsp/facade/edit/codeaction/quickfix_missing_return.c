/* Phase 4 Plan 04-04 Task 01 (EDIT-07) — quickfix for
 * IRON_ERR_MISSING_RETURN (236).
 *
 * Recipe (D-06 §3):
 *   title        = "Insert '<suggestion>'"    (e.g. "Insert 'return 0;'")
 *   edit         = zero-width insertion on the line BEFORE the closing
 *                  `}` of the enclosing function body
 *   newText      = "<indent>" + diag->suggestion + "\n"
 *                  (suggestion ends with ';' per Plan 04-01 seeding)
 *   isPreferred  = false
 *
 * Design: diag->span is the function body span (from `{` to `}`
 * inclusive, per the emit site in typecheck.c's check_missing_return).
 * We derive the insertion point directly from the span without needing
 * AST access — insert at (end_line - 1, 0) pushes the `}` line down and
 * places the return statement above it.
 *
 * Indentation derivation: inspect the document text between span.line
 * and span.end_line for the first non-blank line; its leading
 * whitespace is the body indent. If the body is empty, fall back to
 * 4 spaces (Iron canonical style). The implementation lives in the
 * shared codeaction_indent.{c,h} helper as of Phase 12 Plan 12-01
 * (D-23) so QF-02 + QF-03 can consume the same source-of-truth.
 *
 * Skip: if suggestion is NULL (return type has no obvious zero) or
 * empty, the handler sets *out_n = 0 — caller drops the action. (Plan
 * 04-01 seeds "return 0;" / "return 0.0;" / "return false;" / "return
 * \"\";" when the return type is one of the corresponding primitives.)
 *
 * Phase 12 Plan 12-01 (D-12) — signature widened to (out_arr, out_cap,
 * out_n) per the multi-action substrate. This handler still emits
 * exactly 1 action on success.
 */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/edit/codeaction/codeaction_indent.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"

#include <stdio.h>
#include <string.h>

void ilsp_quickfix_missing_return(const Iron_Diagnostic           *diag,
                                     struct IronLsp_Document         *doc,
                                     struct IronLsp_WorkspaceIndex   *wi,
                                     Iron_Arena                      *arena,
                                     IronLsp_CodeAction              *out_arr,
                                     size_t                           out_cap,
                                     size_t                          *out_n) {
    (void)wi;
    if (!out_arr || !out_n) return;
    *out_n = 0;
    if (out_cap == 0) return;
    memset(&out_arr[0], 0, sizeof(out_arr[0]));
    if (!diag || !doc || !arena) return;
    if (!diag->suggestion || !diag->suggestion[0]) return;
    if (diag->span.line == 0 || diag->span.end_line == 0) return;
    if (diag->span.end_line <= diag->span.line) return;  /* single-line body */

    /* Title: "Insert '<suggestion>'". */
    size_t slen = strlen(diag->suggestion);
    size_t title_need = slen + 16;
    char *title = (char *)iron_arena_alloc(arena, title_need, 1);
    if (!title) return;
    snprintf(title, title_need, "Insert '%s'", diag->suggestion);

    /* Convert Iron 1-indexed lines to LSP 0-indexed. */
    uint32_t start_line_0 = diag->span.line - 1;
    uint32_t end_line_0   = diag->span.end_line - 1;

    /* Indentation from body interior; default to 4. Phase 12 D-23: lifted
     * helper shared with QF-02 + QF-03. */
    uint32_t indent = ilsp_codeaction_derive_body_indent(doc, start_line_0 + 1, end_line_0);

    /* Build newText: "<indent spaces>" + suggestion + "\n". */
    size_t need = (size_t)indent + slen + 2;  /* +\n +NUL */
    char *new_text = (char *)iron_arena_alloc(arena, need, 1);
    if (!new_text) return;
    size_t pos = 0;
    for (uint32_t i = 0; i < indent && pos + 1 < need; i++) new_text[pos++] = ' ';
    memcpy(new_text + pos, diag->suggestion, slen);
    pos += slen;
    new_text[pos++] = '\n';
    new_text[pos]   = '\0';

    /* Insertion point: zero-width at (end_line_0, 0) — i.e. at the start
     * of the `}` line. Inserting "<indent>return 0;\n" pushes the `}`
     * line one row down while placing the return on the new row with
     * the correct indent. */
    out_arr[0].title            = title;
    out_arr[0].kind             = "quickfix";
    out_arr[0].originating_diag = diag;
    out_arr[0].is_preferred     = false;
    out_arr[0].edit_start_line  = end_line_0;
    out_arr[0].edit_start_char  = 0;
    out_arr[0].edit_end_line    = end_line_0;
    out_arr[0].edit_end_char    = 0;
    out_arr[0].edit_new_text    = new_text;
    *out_n = 1;
}
