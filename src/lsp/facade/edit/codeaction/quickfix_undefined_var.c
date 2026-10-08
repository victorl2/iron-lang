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
                                    IronLsp_CodeAction              *out_arr,
                                    size_t                           out_cap,
                                    size_t                          *out_n) {
    (void)wi;
    if (!out_arr || !out_n) return;
    *out_n = 0;
    if (out_cap == 0) return;
    memset(&out_arr[0], 0, sizeof(out_arr[0]));
    if (!diag || !doc || !arena) return;

    /* No typo candidate (suggestion NULL or empty) — skip. */
    if (!diag->suggestion || !diag->suggestion[0]) return;

    /* The compiler's help reads "did you mean 'name'?"; the replacement is
     * the quoted name. Any other help (a prose hint such as "declare 'x'
     * before using it") is not a replacement, so no quickfix. A bare
     * identifier is still accepted. */
    const char *name = diag->suggestion;
    size_t slen = strlen(name);
    static const char prefix[] = "did you mean '";
    if (strncmp(name, prefix, sizeof(prefix) - 1) == 0) {
        name += sizeof(prefix) - 1;
        const char *q = strchr(name, '\'');
        if (!q || q == name) return;
        slen = (size_t)(q - name);
    } else {
        for (size_t i = 0; i < slen; i++) {
            char c = name[i];
            if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9'))) return;
        }
    }
    char *replacement = iron_arena_strdup(arena, name, slen);
    if (!replacement) return;

    /* Title: "Replace with '<name>'". */
    size_t need = slen + 32;  /* "Replace with '" + "'" + NUL */
    char *title = (char *)iron_arena_alloc(arena, need, 1);
    if (!title) return;
    snprintf(title, need, "Replace with '%s'", replacement);

    IronLsp_Range r = ilsp_span_to_lsp_range(diag->span, doc,
        /* encoding resolved upstream by the facade; default to UTF-8 here
         * because handlers_edit.c resolves span->range using the server's
         * negotiated encoding. We pick UTF-8 as a safe default when called
         * without a server context (unit tests). */
        ILSP_ENC_UTF8);

    out_arr[0].title            = title;
    out_arr[0].kind             = "quickfix";
    out_arr[0].originating_diag = diag;
    out_arr[0].is_preferred     = true;
    out_arr[0].edit_start_line  = r.start.line;
    out_arr[0].edit_start_char  = r.start.character;
    out_arr[0].edit_end_line    = r.end.line;
    out_arr[0].edit_end_char    = r.end.character;
    out_arr[0].edit_new_text    = replacement;
    *out_n = 1;
}
