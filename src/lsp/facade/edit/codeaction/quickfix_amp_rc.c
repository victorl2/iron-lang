/* Phase 34 LSP-09: quickfix for IRON_ERR_PTR_AMP_ON_RC=296.
 *
 * Recipe (CONTEXT.md "Quickfix UX"):
 *   title         = "Use 'weak rc'"
 *   kind          = "quickfix"
 *   is_preferred  = true (mechanical fix — `r.downgrade()` is the
 *                  canonical weak-rc obtain operation per
 *                  docs/dev/RC-LAYOUT.md §7)
 *   edit          = replace the entire `&<expr>` span with `<expr>.downgrade()`
 *
 * Strategy: the diagnostic span covers the full `&<expr>` expression
 * (POL-07; the compiler emits at typecheck.c IRON_NODE_UNARY-AMP arm).
 * The leading `&` lives at span.col (1-indexed); we strip it and
 * append `.downgrade()`. The expression text is read directly from
 * doc->text[diag_start_byte+1 .. diag_end_byte] (skip the `&`).
 *
 * Consumer-only handler (CORE-22): never emits diagnostics from the
 * LSP side; never adds a second iron_analyze_buffer call. */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/compile.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void ilsp_quickfix_amp_on_rc(const Iron_Diagnostic           *diag,
                              struct IronLsp_Document         *doc,
                              struct IronLsp_WorkspaceIndex   *wi,
                              Iron_Arena                      *arena,
                              IronLsp_CodeAction              *out_arr,
                              size_t                           out_cap,
                              size_t                          *out_n)
{
    (void)wi;
    if (!out_arr || !out_n) return;
    *out_n = 0;
    if (out_cap == 0) return;
    memset(&out_arr[0], 0, sizeof(out_arr[0]));
    if (!diag || !doc || !arena) return;
    if (diag->span.line == 0 || diag->span.col == 0) return;
    if (diag->span.end_col <= diag->span.col) return;
    if (diag->span.end_line != diag->span.line) return;  /* expect single-line */

    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = NULL };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &walk_diags);
    if (!program) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }

    /* Extract the inner expression text. The diag span starts at the
     * `&` token; the inner expression occupies the bytes immediately
     * after, from col+1 (1-indexed) through end_col (exclusive). */
    uint32_t line_0     = (uint32_t)(diag->span.line - 1);
    uint32_t start_0    = (uint32_t)(diag->span.col  - 1);
    uint32_t end_0      = (uint32_t)(diag->span.end_col - 1);
    size_t line_start_byte = ilsp_byte_of_line(&doc->line_idx, line_0);
    size_t start_byte = line_start_byte + (size_t)start_0;
    size_t end_byte   = line_start_byte + (size_t)end_0;
    if (start_byte >= doc->text_len || end_byte > doc->text_len ||
        end_byte <= start_byte + 1) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    /* Verify the leading byte is `&`. */
    if (doc->text[start_byte] != '&') {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }

    size_t inner_len = end_byte - (start_byte + 1);
    /* Build "<inner>.downgrade()". 12 chars (".downgrade()") + inner + NUL. */
    size_t need = inner_len + 13;
    char *new_text = (char *)iron_arena_alloc(arena, need, 1);
    if (!new_text) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    memcpy(new_text, doc->text + start_byte + 1, inner_len);
    memcpy(new_text + inner_len, ".downgrade()", 12);
    new_text[inner_len + 12] = '\0';

    out_arr[0].title             = "Use 'weak rc'";
    out_arr[0].kind              = "quickfix";
    out_arr[0].originating_diag  = diag;
    out_arr[0].is_preferred      = true;
    out_arr[0].edit_start_line   = line_0;
    out_arr[0].edit_start_char   = start_0;
    out_arr[0].edit_end_line     = line_0;
    out_arr[0].edit_end_char     = end_0;
    out_arr[0].edit_new_text     = new_text;
    *out_n = 1;

    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
