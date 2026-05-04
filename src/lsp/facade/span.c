/* Phase 2 Plan 05 Task 02 (CORE-16) -- Iron_Span -> LSP Range.
 *
 * Uses doc->line_idx to locate each endpoint's line start, then
 * delegates to src/lsp/store/utf.c for the byte->column conversion in
 * the negotiated encoding.
 *
 * Invariants:
 *   - Iron lines/cols are 1-indexed; LSP is 0-indexed.
 *   - Iron cols are byte offsets on the line; LSP chars are encoding-
 *     aware columns.
 *   - Overshoot clamps to line length (spec-correct).
 *   - Zero-valued Iron lines/cols are treated as the position (1,1) and
 *     mapped to (0,0) -- covers the analyzer's default span on
 *     synthetic diagnostics. */

#include "lsp/facade/span.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "lsp/store/utf.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Locate a line's byte range within doc->text. Returns (start, len);
 * if line is past the last recorded start, returns (text_len, 0). */
static void line_byte_range(const IronLsp_Document *doc, uint32_t line0,
                             size_t *out_start, size_t *out_len) {
    size_t start = ilsp_byte_of_line(&doc->line_idx, line0);
    if (start > doc->text_len) start = doc->text_len;

    size_t next = ilsp_byte_of_line(&doc->line_idx, line0 + 1);
    if (next == 0 && line0 != 0) next = doc->text_len;
    if (next > doc->text_len) next = doc->text_len;

    size_t end_ex;
    if (next > start) {
        /* next points at the first byte AFTER the '\n'. The line
         * content (excluding '\n') ends at next - 1. */
        end_ex = next - 1;
    } else {
        end_ex = doc->text_len;
    }
    if (end_ex < start) end_ex = start;

    *out_start = start;
    *out_len   = end_ex - start;
}

/* One endpoint: (iron_line, iron_col) -> (lsp_line, lsp_char).
 * Iron is 1-indexed; LSP is 0-indexed. iron_col is a byte offset on the
 * line (1 = first byte).
 *
 * `is_exclusive_end` is true when this endpoint is the END of a range
 * and the caller wants LSP-spec exclusive semantics. Iron_Span's
 * end_col is 1-indexed INCLUSIVE (iron_token_span sets it to
 * `col + len - 1`, covering the last byte of the token); LSP ranges
 * are half-open so LSP end.character must point one past the last
 * covered byte. The conversion is: LSP_char_exclusive = iron_col
 * (no -1), which matches selection_range.c:65-75's convention.
 *
 * For start endpoints (is_exclusive_end=false) we keep the standard
 * 1-indexed -> 0-indexed (-1) conversion so LSP start.character
 * points at the first covered byte, inclusive. */
static void convert_endpoint(const IronLsp_Document  *doc,
                              uint32_t                 iron_line,
                              uint32_t                 iron_col,
                              bool                     is_exclusive_end,
                              IronLsp_PositionEncoding enc,
                              uint32_t                *out_lsp_line,
                              uint32_t                *out_lsp_char) {
    uint32_t lsp_line;
    if (iron_line == 0) {
        lsp_line = 0;
    } else {
        lsp_line = iron_line - 1;
    }

    size_t line_start, line_len;
    line_byte_range(doc, lsp_line, &line_start, &line_len);
    const char *line_ptr = doc->text ? doc->text + line_start : "";

    /* iron_col -> byte-offset-within-line.
     * Start endpoints: 1-indexed inclusive -> 0-indexed inclusive = col - 1.
     * End endpoints: 1-indexed inclusive -> 0-indexed exclusive = col.
     * Zero input collapses to 0 either way. */
    uint32_t byte_col;
    if (iron_col == 0) {
        byte_col = 0;
    } else if (is_exclusive_end) {
        byte_col = iron_col;
    } else {
        byte_col = iron_col - 1;
    }
    if ((size_t)byte_col > line_len) byte_col = (uint32_t)line_len;

    uint32_t lsp_char;
    if (enc == ILSP_ENC_UTF8) {
        lsp_char = ilsp_utf8_byte_to_utf8_column(line_ptr, line_len, byte_col);
    } else {
        lsp_char = ilsp_utf8_byte_to_utf16_column(line_ptr, line_len, byte_col);
    }

    *out_lsp_line = lsp_line;
    *out_lsp_char = lsp_char;
}

IronLsp_Range ilsp_span_to_lsp_range(Iron_Span                      span,
                                      const IronLsp_Document        *doc,
                                      IronLsp_PositionEncoding       enc) {
    IronLsp_Range r = {0};
    if (!doc) return r;

    convert_endpoint(doc, span.line,     span.col,     false,
                     enc, &r.start.line, &r.start.character);
    convert_endpoint(doc, span.end_line, span.end_col, true,
                     enc, &r.end.line,   &r.end.character);

    /* Guard: if the analyzer emitted end_line == 0 (unset) while start
     * is set, collapse the range to a zero-width point at start. This
     * matches the pattern Iron_Span uses on tokens that don't carry an
     * explicit end (the typechecker reuses the start position). */
    if (span.end_line == 0 && span.line != 0) {
        r.end = r.start;
    }
    return r;
}
