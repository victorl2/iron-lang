/* Phase 4 Plan 04-04 Task 01 (EDIT-07) — quickfix for
 * IRON_ERR_TYPE_MISMATCH_LITERAL (235).
 *
 * Recipe (D-06 §4):
 *   title        = "Change literal to '<suggestion>'"
 *   range        = diag->span (the literal's span)
 *   newText      = diag->suggestion (the retyped form, e.g. "42.0" or
 *                  "42 as Float", populated by typecheck.c's
 *                  emit_type_mismatch_maybe_literal helper in Plan 04-01)
 *   isPreferred  = false
 */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/span.h"
#include "lsp/store/document.h"

#include <stdio.h>
#include <string.h>

void ilsp_quickfix_type_mismatch_literal(const Iron_Diagnostic           *diag,
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

    size_t slen = strlen(diag->suggestion);
    size_t need = slen + 32;
    char *title = (char *)iron_arena_alloc(arena, need, 1);
    if (!title) return;
    snprintf(title, need, "Change literal to '%s'", diag->suggestion);

    IronLsp_Range r = ilsp_span_to_lsp_range(diag->span, doc, ILSP_ENC_UTF8);

    out_arr[0].title            = title;
    out_arr[0].kind             = "quickfix";
    out_arr[0].originating_diag = diag;
    out_arr[0].is_preferred     = false;
    out_arr[0].edit_start_line  = r.start.line;
    out_arr[0].edit_start_char  = r.start.character;
    out_arr[0].edit_end_line    = r.end.line;
    out_arr[0].edit_end_char    = r.end.character;
    out_arr[0].edit_new_text    = iron_arena_strdup(arena, diag->suggestion, slen);
    *out_n = 1;
}
