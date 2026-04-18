/* Phase 2 Plan 04 Task 01 (CORE-11) -- UTF-8 ↔ UTF-16 ↔ byte conversion.
 *
 * The only module permitted to do column math on the LSP facade path.
 * All four functions operate on a single line of UTF-8 text (no
 * line-splitting -- the caller, line_index.c, has already scoped the
 * byte range to one line).
 *
 * Invariants:
 *   - No global / static mutable state (CLAUDE.md).
 *   - No heap allocation (hot path; all work in registers + caller buffers).
 *   - Overshoot clamps to line_len_bytes. Underflow impossible (inputs
 *     are unsigned).
 *
 * References:
 *   - LSP 3.17 §Position (positionEncoding governs the "character" unit)
 *   - RFC 3629 UTF-8 (lead-byte classification table)
 *   - Unicode 15.0 §3.9 (surrogate pair encoding of U+10000..U+10FFFF) */
#include "lsp/store/utf.h"

/* Classify a UTF-8 lead byte, return (bytes_consumed, utf16_code_units). */
static void classify(unsigned char c, size_t *out_bytes, uint32_t *out_units) {
    if (c < 0x80) {
        /* ASCII. */
        *out_bytes = 1; *out_units = 1;
    } else if (c < 0xC0) {
        /* Continuation byte encountered as a lead: malformed input or a
         * byte offset pointing mid-codepoint. Advance by 1 / 1 so the
         * outer walk terminates deterministically. */
        *out_bytes = 1; *out_units = 1;
    } else if (c < 0xE0) {
        /* 2-byte UTF-8 (U+0080..U+07FF) -> BMP -> 1 UTF-16 unit. */
        *out_bytes = 2; *out_units = 1;
    } else if (c < 0xF0) {
        /* 3-byte UTF-8 (U+0800..U+FFFF) -> BMP -> 1 UTF-16 unit.
         * Includes the BOM U+FEFF (0xEF 0xBB 0xBF). */
        *out_bytes = 3; *out_units = 1;
    } else {
        /* 4-byte UTF-8 (U+10000..U+10FFFF) -> surrogate pair -> 2 UTF-16 units. */
        *out_bytes = 4; *out_units = 2;
    }
}

uint32_t ilsp_utf8_byte_to_utf16_column(const char *line, size_t len,
                                         size_t target_byte) {
    if (target_byte > len) target_byte = len;
    uint32_t col = 0;
    size_t   i   = 0;
    while (i < target_byte) {
        size_t   nbytes; uint32_t nunits;
        classify((unsigned char)line[i], &nbytes, &nunits);
        /* Do not walk past target_byte (partial codepoint). Advance by
         * the classified width; if that overshoots, truncate -- the
         * count of full codepoints emitted so far is what the caller
         * wants (target_byte lies at the end of the last whole CP). */
        if (i + nbytes > target_byte) {
            /* Target pointer is mid-codepoint. Per contract, count the
             * codepoint as already consumed (return its units). */
            col += nunits;
            break;
        }
        col += nunits;
        i   += nbytes;
    }
    return col;
}

size_t ilsp_utf16_column_to_utf8_byte(const char *line, size_t len,
                                       uint32_t target_col) {
    uint32_t col = 0;
    size_t   i   = 0;
    while (i < len && col < target_col) {
        size_t   nbytes; uint32_t nunits;
        classify((unsigned char)line[i], &nbytes, &nunits);
        if (i + nbytes > len) {
            /* Line ends mid-codepoint: stop at end. */
            return len;
        }
        /* If this codepoint's units would overshoot target_col (surrogate
         * half-way case: target_col == col + 1 but nunits == 2), round
         * up to the whole-codepoint boundary. This is the spec-correct
         * behaviour for a position pointing into a surrogate pair. */
        col += nunits;
        i   += nbytes;
    }
    if (i > len) return len;
    return i;
}

uint32_t ilsp_utf8_byte_to_utf8_column(const char *line, size_t len,
                                        size_t target_byte) {
    if (target_byte > len) target_byte = len;
    uint32_t col = 0;
    size_t   i   = 0;
    while (i < target_byte) {
        size_t   nbytes; uint32_t nunits;
        classify((unsigned char)line[i], &nbytes, &nunits);
        (void)nunits;
        if (i + nbytes > target_byte) {
            /* Mid-codepoint -- count this codepoint. */
            col += 1;
            break;
        }
        col += 1;
        i   += nbytes;
    }
    return col;
}

size_t ilsp_utf8_column_to_utf8_byte(const char *line, size_t len,
                                      uint32_t target_col) {
    uint32_t col = 0;
    size_t   i   = 0;
    while (i < len && col < target_col) {
        size_t   nbytes; uint32_t nunits;
        classify((unsigned char)line[i], &nbytes, &nunits);
        (void)nunits;
        if (i + nbytes > len) return len;
        col += 1;
        i   += nbytes;
    }
    return i;
}
