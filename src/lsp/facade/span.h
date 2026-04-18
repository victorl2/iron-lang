#ifndef IRON_LSP_FACADE_SPAN_H
#define IRON_LSP_FACADE_SPAN_H

/* Phase 2 Plan 05 Task 02 (CORE-16) -- Iron_Span -> LSP Range translation.
 *
 * Iron_Span fields (from src/diagnostics/diagnostics.h):
 *   filename (pointer-interned)
 *   line, col, end_line, end_col  (1-indexed, byte-based)
 *
 * LSP Range fields (from src/lsp/facade/types.h):
 *   start.line, start.character  (0-indexed, character in negotiated encoding)
 *   end.line,   end.character
 *
 * Conversion:
 *   lsp_line = iron_line - 1
 *   For each endpoint: find the line's byte-start via line_idx, take the
 *   byte offset (iron_col - 1) WITHIN THAT LINE, convert to negotiated
 *   encoding column via src/lsp/store/utf.c.
 *
 * Overshoot policy: clamp to end-of-line (LSP spec-correct). Columns
 * that walk past line_len return the line's UTF-column length. */

#include "diagnostics/diagnostics.h"   /* Iron_Span */
#include "lsp/facade/types.h"          /* IronLsp_Range, IronLsp_PositionEncoding */

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_Document;

/* Convert a 1-indexed byte-based Iron_Span to a 0-indexed
 * encoding-aware LSP Range. `doc` provides the line index + text for
 * UTF column math. */
IronLsp_Range ilsp_span_to_lsp_range(Iron_Span                       span,
                                      const struct IronLsp_Document  *doc,
                                      IronLsp_PositionEncoding        enc);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_SPAN_H */
