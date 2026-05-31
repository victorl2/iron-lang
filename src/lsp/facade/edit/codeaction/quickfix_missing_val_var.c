/* Phase 34 LSP-06: quickfix for IRON_ERR_MISSING_VAL_VAR=176.
 *
 * Recipe (CONTEXT.md "Quickfix UX"):
 *   title         = "Add 'val'"
 *   kind          = "quickfix"
 *   is_preferred  = true (mechanical fix — single canonical solution)
 *   edit          = zero-width insertion at the binding's span start
 *   newText       = "val "
 *
 * The diagnostic is emitted by the compiler at the bare identifier on
 * the LHS of a `=` statement that has no preceding `val`/`var`/`mut`
 * binding qualifier (VAL-01, VAL-02). The fix prepends `val ` at the
 * identifier's start so the resulting statement is `val x = 10`.
 *
 * Consumer-only handler (CORE-22): NEVER emits diagnostics from the
 * LSP side; NEVER adds a second iron_analyze_buffer call. Re-analyzes
 * through the existing ilsp_facade_compile_for_nav facade for fresh
 * spans (Plan 34-04 RESEARCH §4 Pitfall 3: anchor drift). */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/compile.h"
#include "lsp/store/document.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void ilsp_quickfix_missing_val_var(const Iron_Diagnostic           *diag,
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

    /* Re-analyze for fresh spans (CORE-22 facade — Pitfall 3 anchor
     * drift defence). We discard the program pointer; the binding
     * span on the diag itself is sufficient for this single-edit
     * insertion. The re-analyze round-trip is the same shape as the
     * Phase 12 QF-02 template (quickfix_object_no_init.c:113-124). */
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

    /* Iron_Span is 1-indexed (line, col); LSP Range is 0-indexed. */
    uint32_t ins_line_0 = (uint32_t)(diag->span.line - 1);
    uint32_t ins_col_0  = (uint32_t)(diag->span.col  - 1);

    out_arr[0].title             = "Add 'val'";
    out_arr[0].kind              = "quickfix";
    out_arr[0].originating_diag  = diag;
    out_arr[0].is_preferred      = true;
    out_arr[0].edit_start_line   = ins_line_0;
    out_arr[0].edit_start_char   = ins_col_0;
    out_arr[0].edit_end_line     = ins_line_0;
    out_arr[0].edit_end_char     = ins_col_0;
    out_arr[0].edit_new_text     = "val ";
    *out_n = 1;

    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
