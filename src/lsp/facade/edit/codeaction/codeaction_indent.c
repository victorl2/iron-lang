/* Phase 12 Plan 12-01 (D-23 — lift) — shared body-indent helper.
 *
 * Body lifted verbatim from quickfix_missing_return.c:40-72 with the
 * `static` keyword removed and the symbol renamed to the LSP-scoped
 * convention (ilsp_*). Shared by QF-02 + QF-03 in Plan 12-02 / 12-03;
 * quickfix_missing_return.c is updated in Plan 12-01 Task 4 to consume
 * this header instead of carrying the local definition. */

#include "lsp/facade/edit/codeaction/codeaction_indent.h"

#include "lsp/store/document.h"
#include "lsp/store/line_index.h"

uint32_t ilsp_codeaction_derive_body_indent(
        const struct IronLsp_Document *doc,
        uint32_t                       start_line_0,
        uint32_t                       end_line_0) {
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
