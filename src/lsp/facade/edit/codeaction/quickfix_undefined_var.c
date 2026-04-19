/* Phase 4 Plan 04-04 Task 01 (EDIT-07) — quickfix for
 * IRON_ERR_UNDEFINED_VAR (200).
 *
 * Recipe (D-06 §1):
 *   title        = "Replace with '<suggestion>'"
 *   range        = diag->span (the undefined identifier span)
 *   newText      = diag->suggestion (the typo winner string seeded by
 *                  iron_best_typo_candidate in src/analyzer/resolve.c)
 *   isPreferred  = true (LSP editors auto-highlight preferred actions)
 *
 * The compiler seeds diag->suggestion at emit time (Plan 04-01 Task 02).
 * If the seed is NULL (no candidate within Levenshtein distance 2) we
 * skip the action — caller drops it from the result array.
 */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/span.h"
#include "lsp/store/document.h"

#include <stdio.h>
#include <string.h>

void ilsp_quickfix_undefined_var(const Iron_Diagnostic           *diag,
                                    struct IronLsp_Document         *doc,
                                    struct IronLsp_WorkspaceIndex   *wi,
                                    Iron_Arena                      *arena,
                                    IronLsp_CodeAction              *out) {
    (void)wi;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!diag || !doc || !arena) return;

    /* No typo candidate (suggestion NULL or empty) — skip. */
    if (!diag->suggestion || !diag->suggestion[0]) return;

    /* Title: "Replace with '<suggestion>'". Budget a few bytes beyond the
     * suggestion for the constant fragments. */
    size_t slen = strlen(diag->suggestion);
    size_t need = slen + 32;  /* "Replace with '" + "'" + NUL */
    char *title = (char *)iron_arena_alloc(arena, need, 1);
    if (!title) return;
    snprintf(title, need, "Replace with '%s'", diag->suggestion);

    IronLsp_Range r = ilsp_span_to_lsp_range(diag->span, doc,
        /* encoding resolved upstream by the facade; default to UTF-8 here
         * because handlers_edit.c resolves span->range using the server's
         * negotiated encoding. We pick UTF-8 as a safe default when called
         * without a server context (unit tests). */
        ILSP_ENC_UTF8);

    out->title            = title;
    out->kind             = "quickfix";
    out->originating_diag = diag;
    out->is_preferred     = true;
    out->edit_start_line  = r.start.line;
    out->edit_start_char  = r.start.character;
    out->edit_end_line    = r.end.line;
    out->edit_end_char    = r.end.character;
    out->edit_new_text    = iron_arena_strdup(arena, diag->suggestion, slen);
}
