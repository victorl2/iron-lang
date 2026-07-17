/* Phase 34 LSP-08: quickfix for IRON_WARN_UNUSED_VAR=613.
 *
 * Recipe (CONTEXT.md "Quickfix UX"):
 *   title         = "Drop 'var' modifier"
 *   kind          = "quickfix"
 *   is_preferred  = true (mechanical fix — single canonical solution)
 *   edit          = replace the `var` keyword (3 chars) at the binding
 *                   span with `val`
 *   newText       = "val"
 *
 * The diagnostic is emitted by the compiler when a `var` binding's
 * mutable slot is never reassigned (VAL-05). For the allocation case
 * the binding still owns the value but doesn't need a write-capable
 * slot, so `val` is the canonical demotion.
 *
 * The diagnostic span points at the `var` keyword itself (3 chars wide).
 * We compute end_char = start_char + 3 to cover exactly the keyword;
 * the caller's editor handles surrounding whitespace.
 *
 * Consumer-only handler (CORE-22): never emits diagnostics from the
 * LSP side; never adds a second iron_analyze_buffer call. */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/compile.h"
#include "lsp/store/document.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void ilsp_quickfix_unused_var_alloc(const Iron_Diagnostic           *diag,
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

    uint32_t ed_line_0  = (uint32_t)(diag->span.line - 1);
    uint32_t ed_start_0 = (uint32_t)(diag->span.col  - 1);
    /* The diag span covers the `var` keyword (3 bytes). Use the span's
     * end_col when present; fall back to start+3 when the diag carries
     * only a start anchor. */
    uint32_t ed_end_0;
    if (diag->span.end_col > diag->span.col) {
        ed_end_0 = (uint32_t)(diag->span.end_col - 1);
    } else {
        ed_end_0 = ed_start_0 + 3;
    }

    out_arr[0].title             = "Drop 'var' modifier";
    out_arr[0].kind              = "quickfix";
    out_arr[0].originating_diag  = diag;
    out_arr[0].is_preferred      = true;
    out_arr[0].edit_start_line   = ed_line_0;
    out_arr[0].edit_start_char   = ed_start_0;
    out_arr[0].edit_end_line     = ed_line_0;
    out_arr[0].edit_end_char     = ed_end_0;
    out_arr[0].edit_new_text     = "val";
    *out_n = 1;

    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
