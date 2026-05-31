/* Phase 34 LSP-07: quickfix for IRON_WARN_FORGOTTEN_FREE=606.
 *
 * Recipe (CONTEXT.md "Quickfix UX"):
 *   title         = "Insert 'defer free <binding>'"   (binding name interpolated)
 *   kind          = "quickfix"
 *   is_preferred  = true (mechanical fix — `defer free <binding>` is
 *                  the canonical lifecycle close for a non-escaping
 *                  heap binding per docs/dev/RC-LAYOUT.md §1)
 *   edit          = zero-width insertion at the start of the line
 *                   AFTER the binding decl (column 0); newText is
 *                   `<indent>defer free <binding>\n` where <indent>
 *                   matches the binding line's leading whitespace.
 *
 * The diagnostic is emitted on a binding line like `val buf = heap Buffer(1024)`
 * (DBG-05); span covers the binding name. Strategy:
 *   1. Extract the binding name from doc->text using the diag span.
 *   2. Compute the binding line's leading-whitespace prefix and reuse it.
 *   3. Insert `<indent>defer free <binding>\n` at column 0 of the next line.
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

void ilsp_quickfix_forgotten_free(const Iron_Diagnostic           *diag,
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

    /* Extract the binding name from doc->text using the diag span.
     * The diag span covers the binding identifier (e.g., `buf`). */
    uint32_t line_0     = (uint32_t)(diag->span.line - 1);
    uint32_t start_0    = (uint32_t)(diag->span.col - 1);
    uint32_t end_0      = (diag->span.end_col > diag->span.col)
                            ? (uint32_t)(diag->span.end_col - 1)
                            : start_0;
    if (end_0 <= start_0) {
        /* Fallback: scan the identifier from start_0. */
        size_t lstart = ilsp_byte_of_line(&doc->line_idx, line_0);
        size_t sb = lstart + (size_t)start_0;
        size_t eb = sb;
        while (eb < doc->text_len) {
            char c = doc->text[eb];
            bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_';
            if (!ident) break;
            eb++;
        }
        end_0 = start_0 + (uint32_t)(eb - sb);
    }
    if (end_0 <= start_0) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    size_t line_start_byte = ilsp_byte_of_line(&doc->line_idx, line_0);
    size_t name_start_byte = line_start_byte + (size_t)start_0;
    size_t name_end_byte   = line_start_byte + (size_t)end_0;
    if (name_start_byte >= doc->text_len || name_end_byte > doc->text_len) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    size_t name_len = name_end_byte - name_start_byte;
    if (name_len == 0 || name_len > 256) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    char binding[260];
    memcpy(binding, doc->text + name_start_byte, name_len);
    binding[name_len] = '\0';

    /* Derive the binding line's leading whitespace as the indent. */
    char indent[64];
    size_t indent_len = 0;
    for (size_t i = line_start_byte;
         i < doc->text_len && indent_len < sizeof(indent) - 1;
         i++) {
        char c = doc->text[i];
        if (c == ' ' || c == '\t') { indent[indent_len++] = c; }
        else break;
    }
    indent[indent_len] = '\0';

    /* Build the title: "Insert 'defer free <binding>'". */
    char title_buf[320];
    int tn = snprintf(title_buf, sizeof(title_buf),
                      "Insert 'defer free %s'", binding);
    if (tn <= 0 || (size_t)tn >= sizeof(title_buf)) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    char *title = (char *)iron_arena_alloc(arena, (size_t)tn + 1, 1);
    if (!title) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    memcpy(title, title_buf, (size_t)tn + 1);

    /* Build new_text = "<indent>defer free <binding>\n". */
    size_t need = indent_len + (size_t)name_len + 32;
    char *new_text = (char *)iron_arena_alloc(arena, need, 1);
    if (!new_text) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    int written = snprintf(new_text, need,
                            "%sdefer free %s\n", indent, binding);
    if (written <= 0 || (size_t)written >= need) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }

    /* Insertion point: column 0 of the line AFTER the binding decl. */
    uint32_t ins_line_0 = line_0 + 1;

    out_arr[0].title             = title;
    out_arr[0].kind              = "quickfix";
    out_arr[0].originating_diag  = diag;
    out_arr[0].is_preferred      = true;
    out_arr[0].edit_start_line   = ins_line_0;
    out_arr[0].edit_start_char   = 0;
    out_arr[0].edit_end_line     = ins_line_0;
    out_arr[0].edit_end_char     = 0;
    out_arr[0].edit_new_text     = new_text;
    *out_n = 1;

    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
