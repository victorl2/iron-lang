/* Phase 12 Plan 12-02 (QF-01) — quickfix for IRON_ERR_V3_RECEIVER_SYNTAX
 * (260) AND IRON_ERR_V3_MUT_RECEIVER (261).
 *
 * Recipe (D-16..D-20):
 *   title         = "Run ironc migrate --from v2 --to v3"
 *   kind          = "quickfix"
 *   command_title = "Iron: Migrate v2 → v3"
 *   command_id    = "iron.migrate.fromV2ToV3"
 *   command_args  = [doc->uri]
 *   is_preferred  = false  (codemod is workspace-destructive; user opts in)
 *
 * No edit; the wire serializer (handlers_edit.c, Plan 12-01 D-15) emits
 *   "command": { "title": ..., "command": "iron.migrate.fromV2ToV3",
 *                "arguments": ["file:///..."] }
 *
 * Server does NOT advertise executeCommandProvider (RESEARCH Open Item
 * #10 verified absent; Phase 14 CMD-01..03). Editor extensions handle
 * this command client-side until Phase 14.
 *
 * Codes 260 AND 261 share this handler (D-18 — same fix is "run the
 * codemod" regardless of whether the v2 receiver had `mut` or not).
 */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/store/document.h"
#include "util/arena.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

void ilsp_quickfix_v3_receiver_syntax(const Iron_Diagnostic           *diag,
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
    if (!doc->uri) return;

    /* Allocate command_args = [doc->uri]. */
    const char **args = (const char **)iron_arena_alloc(
        arena, 1 * sizeof(*args), alignof(const char *));
    if (!args) return;
    size_t uri_len = strlen(doc->uri);
    args[0] = iron_arena_strdup(arena, doc->uri, uri_len);
    if (!args[0]) return;

    out_arr[0].title            = "Run ironc migrate --from v2 --to v3";
    out_arr[0].kind             = "quickfix";
    out_arr[0].originating_diag = diag;
    out_arr[0].is_preferred     = false;
    out_arr[0].command_title    = "Iron: Migrate v2 \xe2\x86\x92 v3";  /* UTF-8 → */
    out_arr[0].command_id       = "iron.migrate.fromV2ToV3";
    out_arr[0].command_args     = args;
    out_arr[0].command_args_n   = 1;
    /* edit_* and edit_text_edits[] stay zero/NULL. */
    *out_n = 1;
}
