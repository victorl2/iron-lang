/* Phase 4 Plan 04-04 Task 02 (EDIT-08) — codeAction/resolve lazy edit
 * compute (D-07).
 *
 * Contract:
 *   - If data.file_version != doc->version: stale. Zero-fill out.
 *   - Else re-analyze via ilsp_facade_compile_for_nav to get the fresh
 *     Iron_DiagList; verify that diagnostics[data.diagnostic_idx]
 *     exists AND its code matches data.code. If not: stale. Zero-fill.
 *   - Else look up the quickfix handler via ilsp_quickfix_lookup(code);
 *     invoke it; the handler writes the edit into out.
 *
 * NEVER RPC-errors — every failure path writes out->edit_new_text=NULL
 * and returns; handlers_edit.c emits `edit: null` on the wire so the
 * client drops it gracefully.
 *
 * Threat mitigation (T-4-3 untrusted data payload):
 *   - data.code is validated via registry lookup; unknown code => NULL
 *     edit.
 *   - data.diagnostic_idx is bounds-checked against the CURRENT diag
 *     list (not the client's cached one).
 *   - data.file_version drives the staleness check; missing/mismatched
 *     versions => NULL edit.
 */

#include "lsp/facade/edit/codeaction/codeaction.h"
#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/edit/codeaction/organize_imports.h"
#include "lsp/facade/compile.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "lsp/server/notifications.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

void ilsp_facade_code_action_resolve(struct IronLsp_Server   *server,
                                        struct IronLsp_Document *doc,
                                        int                      data_file_version,
                                        int                      data_code,
                                        int                      data_diagnostic_idx,
                                        _Atomic bool            *cancel,
                                        Iron_Arena              *arena,
                                        IronLsp_CodeAction      *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!server || !doc || !arena) return;

    /* Stale-version check FIRST (cheap; avoids re-analyze). */
    if (doc->version != data_file_version) return;

    if (cancel && atomic_load(cancel)) return;

    /* Phase 4 Plan 04-05 (D-08): organize-imports dispatch takes the
     * ILSP_ORGANIZE_IMPORTS_SENTINEL code. Routed to the facade which
     * re-runs the classify/sort/dedup/unused-gate algorithm against
     * the fresh AST and produces the authoritative edit. */
    if (data_code == ILSP_ORGANIZE_IMPORTS_SENTINEL) {
        Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
        Iron_DiagList walk_diags = iron_diaglist_create();
        IronLsp_CompileRequest req = { .version = doc->version,
                                         .cancel_flag = cancel };
        Iron_Program *program = ilsp_facade_compile_for_nav(
            doc, &req, &walk_arena, &walk_diags);

        if (cancel && atomic_load(cancel)) {
            iron_diaglist_free(&walk_diags);
            iron_arena_free(&walk_arena);
            return;
        }

        IronLsp_OrganizeImportsResult r = {0};
        ilsp_organize_imports(program, doc, &walk_diags,
                                server->workspace_index, cancel, arena, &r);

        if (r.new_text) {
            out->title               = "Organize Imports";
            out->kind                = "source.organizeImports";
            out->originating_diag    = NULL;
            out->is_preferred        = false;
            out->edit_start_line     = r.range_start_line;
            out->edit_start_char     = r.range_start_char;
            out->edit_end_line       = r.range_end_line;
            out->edit_end_char       = r.range_end_char;
            out->edit_new_text       = r.new_text;
            out->data_file_version   = doc->version;
            out->data_code           = ILSP_ORGANIZE_IMPORTS_SENTINEL;
            out->data_diagnostic_idx = 0;
        }

        if (r.cold_workspace_warning) {
            ilsp_send_window_showmessage(server, doc->uri,
                ILSP_MESSAGE_TYPE_INFO,
                "organizeImports: full unused-removal pending "
                "workspace analysis. Invoke Find References first, "
                "or re-run organizeImports later.");
        }

        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }

    /* Unknown code: registry refuses, stay stale. */
    IronLsp_QuickfixFn handler = ilsp_quickfix_lookup(data_code);
    if (!handler) return;

    /* Bounds check. */
    if (data_diagnostic_idx < 0) return;

    /* Re-analyze to get the fresh diag list (the CURRENT list is the
     * authoritative source per T-4-3 mitigation). */
    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                     .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(doc, &req,
                                                           &walk_arena, &walk_diags);
    (void)program; /* only the diag list matters here */

    if (cancel && atomic_load(cancel)) goto done;

    /* Validate idx + code. */
    if (data_diagnostic_idx >= walk_diags.count) goto done;
    const Iron_Diagnostic *d = &walk_diags.items[data_diagnostic_idx];
    if (d->code != data_code) goto done;

    /* Dispatch — handler fills out from d + doc; arena holds strings. */
    handler(d, doc, server->workspace_index, arena, out);

done:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
