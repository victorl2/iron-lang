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
 * 4 spaces (Iron canonical style).
 *
 * Skip: if suggestion is NULL (return type has no obvious zero) or
 * empty, the handler leaves out->edit_new_text NULL — caller drops the
 * action. (Plan 04-01 seeds "return 0;" / "return 0.0;" / "return
 * false;" / "return \"\";" when the return type is one of the
 * corresponding primitives.)
 */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"

#include <stdio.h>
#include <string.h>

/* Inspect the document text between start_line and end_line (both 0-indexed);
 * return the leading whitespace width of the first non-blank line. On
 * no-such-line or NULL text, returns a default of 4 (Iron canonical). */
static uint32_t derive_body_indent(const struct IronLsp_Document *doc,
                                     uint32_t start_line_0,
                                     uint32_t end_line_0) {
    const uint32_t DEFAULT_INDENT = 4;
    if (!doc || !doc->text || doc->text_len == 0) return DEFAULT_INDENT;

    for (uint32_t ln = start_line_0; ln < end_line_0; ln++) {
        size_t start = ilsp_byte_of_line(&doc->line_idx, ln);
        size_t next  = ilsp_byte_of_line(&doc->line_idx, ln + 1);
        if (start > doc->text_len) start = doc->text_len;
        if (next  > doc->text_len) next  = doc->text_len;
        /* Fallback for the last line: ilsp_byte_of_line clamps to the
         * last recorded start, so next == start for the last line —
         * treat that as "line runs to text_len". */
        if (next <= start) next = doc->text_len;
        if (start >= next) continue;

        size_t end = next;
        if (end > start && doc->text[end - 1] == '\n') end--;

        /* Count leading whitespace. */
        uint32_t indent = 0;
        size_t i = start;
        while (i < end && (doc->text[i] == ' ' || doc->text[i] == '\t')) {
            indent++;
            i++;
        }
        /* Skip blank lines (all whitespace). */
        if (i >= end) continue;
        return indent;
    }
    return DEFAULT_INDENT;
}

void ilsp_quickfix_missing_return(const Iron_Diagnostic           *diag,
                                     struct IronLsp_Document         *doc,
                                     struct IronLsp_WorkspaceIndex   *wi,
                                     Iron_Arena                      *arena,
                                     IronLsp_CodeAction              *out) {
    (void)wi;
    if (!out) return;
    memset(out, 0, sizeof(*out));
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

    /* Indentation from body interior; default to 4. */
    uint32_t indent = derive_body_indent(doc, start_line_0 + 1, end_line_0);

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
    out->title            = title;
    out->kind             = "quickfix";
    out->originating_diag = diag;
    out->is_preferred     = false;
    out->edit_start_line  = end_line_0;
    out->edit_start_char  = 0;
    out->edit_end_line    = end_line_0;
    out->edit_end_char    = 0;
    out->edit_new_text    = new_text;
}
