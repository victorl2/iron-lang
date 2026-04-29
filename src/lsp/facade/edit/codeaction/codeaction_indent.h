#ifndef IRON_LSP_FACADE_EDIT_CODEACTION_CODEACTION_INDENT_H
#define IRON_LSP_FACADE_EDIT_CODEACTION_CODEACTION_INDENT_H

/* Phase 12 Plan 12-01 (D-23 — lift) — shared body-indent helper.
 *
 * Quickfix handlers that synthesize a multi-line insertion need to
 * derive the surrounding body's indent so the new text matches the
 * user's existing style. This helper inspects the document text
 * between two 0-indexed line numbers and returns the leading
 * whitespace width of the first non-blank line; defaults to 4 when
 * the buffer is empty or no non-blank line is found.
 *
 * Originally `static derive_body_indent` in
 * src/lsp/facade/edit/codeaction/quickfix_missing_return.c:40-72;
 * lifted in Phase 12 Plan 12-01 so QF-02 (object_no_init synthesis)
 * and QF-03 (inline_default synthesis) can share a single source of
 * truth (CONTEXT.md D-23; RESEARCH.md Open Item #2).
 *
 * Pure: no allocation, no I/O, no globals — safe to call concurrently
 * from any request thread (per CLAUDE.md "Concurrency: requests may
 * run in parallel on shared `iron_compiler` state — all state must
 * be scoped per request/document"). */

#include <stdint.h>

struct IronLsp_Document;

#ifdef __cplusplus
extern "C" {
#endif

/* Inspect document text between start_line_0 and end_line_0 (both
 * 0-indexed); return the leading-whitespace count (in bytes) of the
 * first non-blank line. NULL doc / empty buffer / no non-blank line
 * found -> default 4. */
uint32_t ilsp_codeaction_derive_body_indent(
    const struct IronLsp_Document *doc,
    uint32_t                       start_line_0,
    uint32_t                       end_line_0);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_EDIT_CODEACTION_CODEACTION_INDENT_H */
