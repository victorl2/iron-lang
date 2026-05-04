#ifndef IRON_LSP_STORE_UTF_H
#define IRON_LSP_STORE_UTF_H

/* Phase 2 Plan 04 Task 01 (CORE-11) -- UTF-8 ↔ UTF-16 ↔ byte-offset
 * conversion.
 *
 * Iron source is UTF-8; Iron_Span columns are byte-based; LSP default
 * position encoding is UTF-16 code units. These helpers are the SINGLE
 * chokepoint for column math at the facade boundary -- every diagnostic
 * / hover / goto range flows through here.
 *
 * Algorithm: UTF-8 lead-byte classification (ASCII / 2-byte / 3-byte /
 * 4-byte) drives both byte advance and UTF-16 code-unit advance:
 *   - 0x00-0x7F:   1 byte  -> 1 UTF-16 code unit  (ASCII fast path)
 *   - 0x80-0xBF:   continuation byte (mid-codepoint; advance by 1 byte
 *                  and 1 unit for safety when called with a truncated
 *                  offset; prevents infinite loops)
 *   - 0xC0-0xDF:   2 bytes -> 1 UTF-16 code unit
 *   - 0xE0-0xEF:   3 bytes -> 1 UTF-16 code unit (includes BOM U+FEFF)
 *   - 0xF0-0xF7:   4 bytes -> 2 UTF-16 code units (surrogate pair)
 *
 * Overshoot policy: all four functions clamp to line_len_bytes. A column
 * that walks past the last byte returns the end-of-line offset rather
 * than asserting. This matches LSP 3.17 §Position semantics: if a
 * position is outside a line's range, it is clamped.
 *
 * No global/static mutable state (CLAUDE.md invariant). */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Given a UTF-8 line, convert byte offset within line to UTF-16 code
 * unit column. Surrogate pairs count as 2 units; combining marks are
 * separate codepoints and count independently (no NFC normalization). */
uint32_t ilsp_utf8_byte_to_utf16_column(const char *line_utf8,
                                         size_t      line_len_bytes,
                                         size_t      byte_offset);

/* Inverse: given UTF-16 column within a UTF-8 line, return byte offset.
 * Clamps to line_len_bytes if column overshoots. If a column falls
 * between the two halves of a surrogate pair (a malformed client
 * input), rounds up to the next whole-codepoint boundary. */
size_t   ilsp_utf16_column_to_utf8_byte(const char *line_utf8,
                                         size_t      line_len_bytes,
                                         uint32_t    utf16_column);

/* UTF-8 codepoint index variants (used when the negotiated encoding is
 * UTF-8 -- i.e., "character" in an LSP Position is a codepoint index,
 * NOT a byte index). Supplementary-plane codepoints count as 1 here
 * (vs. 2 in the UTF-16 surrogate form). */
uint32_t ilsp_utf8_byte_to_utf8_column(const char *line_utf8,
                                        size_t      line_len_bytes,
                                        size_t      byte_offset);

size_t   ilsp_utf8_column_to_utf8_byte(const char *line_utf8,
                                        size_t      line_len_bytes,
                                        uint32_t    utf8_column);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_STORE_UTF_H */
