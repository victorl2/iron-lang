/* Phase 4 Plan 04-04 Task 02 (EDIT-07) — textDocument/codeAction
 * orchestrator (D-06, D-07).
 *
 * Flow:
 *   1. Re-analyze doc via ilsp_facade_compile_for_nav — server-side
 *      diagnostic list is the authoritative source (T-4-3 mitigation;
 *      client-supplied params.context.diagnostics is used only to
 *      select which codes to consider, never to compute edits).
 *   2. For each diag in the current list whose span intersects `range`:
 *        - Look up the quickfix handler via ilsp_quickfix_lookup.
 *        - Filter by context.only (accept "quickfix" kinds).
 *        - Invoke the handler to produce an IronLsp_CodeAction.
 *        - Stamp data = {file_version, code, diagnostic_idx}.
 *   3. Return the array; handlers_edit.c serializes each entry with
 *      NO edit field (lazy fill via codeAction/resolve).
 *
 * Cancel polling: once at entry, once after analyze, and between
 * diagnostic iterations (D-17 per-iteration boundary).
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

/* Test whether kind "quickfix" is accepted by the only-filter. Per LSP
 * 3.17, a filter entry matches if it is equal to the kind or is a
 * prefix delimited by '.'. So "quickfix" matches "quickfix",
 * "quickfix.name", etc. Empty/NULL only => accept. */
static bool only_allows_quickfix(const IronLsp_CodeActionOnly *only) {
    if (!only || !only->kinds || only->kind_n == 0) return true;
    for (size_t i = 0; i < only->kind_n; i++) {
        const char *k = only->kinds[i];
        if (!k) continue;
        if (strcmp(k, "quickfix") == 0) return true;
        /* Accept "quickfix.<anything>" as prefix of our kind? We emit
         * plain "quickfix"; prefix match by LSP spec means the filter
         * "quickfix.*" would require our kind to start with "quickfix."
         * — it doesn't (we emit bare "quickfix"). But if filter is
         * strictly "quickfix" it matches; any broader filter string
         * like "source.organizeImports" does NOT match "quickfix". */
    }
    return false;
}

/* Phase 4 Plan 04-05 (D-08) -- organizeImports is EXPLICIT-trigger only.
 * The action surfaces ONLY when `only` explicitly contains
 * "source.organizeImports" (or a prefix match per LSP 3.17). An absent
 * or unrelated filter returns false so plain codeAction requests at a
 * random cursor position don't leak the source-action into the quickfix
 * dropdown. This matches VSCode's Source Actions right-click path and
 * the editor.codeActionsOnSave:{"source.organizeImports":true} hook. */
static bool only_explicitly_requests_organize_imports(
        const IronLsp_CodeActionOnly *only) {
    if (!only || !only->kinds || only->kind_n == 0) return false;
    for (size_t i = 0; i < only->kind_n; i++) {
        const char *k = only->kinds[i];
        if (!k) continue;
        if (strcmp(k, "source.organizeImports") == 0) return true;
        /* Broader filters like "source" also match organizeImports per
         * LSP CodeActionKind prefix semantics. Our emitted kind is the
         * full "source.organizeImports" string; the filter matches when
         * the filter is a prefix delimited by '.'. */
        if (strcmp(k, "source") == 0) return true;
    }
    return false;
}

/* Is Iron_Span within `range`? Conservative: accept if the span's
 * start line is within the range's line bounds, OR if the range spans
 * >= 1 line. We use whole-line granularity because LSP code-action
 * requests typically carry the selection span which may be a zero-
 * width cursor — a strict per-column intersection test would miss
 * diagnostics that sit at the cursor's line. */
static bool span_intersects(Iron_Span span, IronLsp_Range range) {
    if (span.line == 0) return false;  /* unset span */
    uint32_t sline = span.line - 1;
    uint32_t eline = (span.end_line == 0) ? sline : span.end_line - 1;
    /* Intersection of [sline..eline] with [range.start.line..range.end.line]. */
    if (eline < range.start.line) return false;
    if (sline > range.end.line)   return false;
    return true;
}

void ilsp_facade_code_action(struct IronLsp_Server         *server,
                                struct IronLsp_Document       *doc,
                                IronLsp_Range                  range,
                                const IronLsp_CodeActionOnly  *only,
                                _Atomic bool                  *cancel,
                                Iron_Arena                    *arena,
                                IronLsp_CodeAction           **out_actions,
                                size_t                        *out_n) {
    if (out_actions) *out_actions = NULL;
    if (out_n)       *out_n       = 0;
    if (!server || !doc || !arena || !out_actions || !out_n) return;

    bool emit_quickfixes     = only_allows_quickfix(only);
    bool emit_organize       = only_explicitly_requests_organize_imports(only);

    /* Nothing to compute when both paths are filtered out. */
    if (!emit_quickfixes && !emit_organize) return;

    if (cancel && atomic_load(cancel)) return;

    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                     .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(doc, &req,
                                                           &walk_arena, &walk_diags);

    if (cancel && atomic_load(cancel)) goto done;

    /* Upper bound: one quickfix per diagnostic + one organize slot. */
    size_t cap = (size_t)(walk_diags.count > 0 ? walk_diags.count : 0) + 1;
    IronLsp_CodeAction *arr = (IronLsp_CodeAction *)iron_arena_alloc(
        arena, cap * sizeof(IronLsp_CodeAction),
        _Alignof(IronLsp_CodeAction));
    if (!arr) goto done;

    size_t n = 0;

    /* Quickfix path: iterate diagnostics in [range], dispatch to the
     * registered quickfix handler, stamp data for lazy-resolve. */
    if (emit_quickfixes && walk_diags.count > 0) {
        for (int i = 0; i < walk_diags.count; i++) {
            if (cancel && atomic_load(cancel)) break;
            const Iron_Diagnostic *d = &walk_diags.items[i];
            if (!span_intersects(d->span, range)) continue;
            IronLsp_QuickfixFn handler = ilsp_quickfix_lookup(d->code);
            if (!handler) continue;

            IronLsp_CodeAction tmp;
            handler(d, doc, server->workspace_index, arena, &tmp);
            /* Handler may refuse (NULL edit_new_text). */
            if (!tmp.edit_new_text) continue;

            tmp.data_file_version   = doc->version;
            tmp.data_code           = d->code;
            tmp.data_diagnostic_idx = i;

            arr[n++] = tmp;
        }
    }

    /* Organize-imports path: emit a single lightweight CodeAction
     * (no edit field -- the edit is computed lazily via
     * codeAction/resolve, per D-07). Pre-flight a facade call so we
     * don't surface the action on documents with zero imports; this
     * keeps the action list uncluttered on code that has nothing to
     * organize. */
    if (emit_organize && !(cancel && atomic_load(cancel)) && program) {
        IronLsp_OrganizeImportsResult pre = {0};
        Iron_Arena pre_arena = iron_arena_create(8 * 1024);
        ilsp_organize_imports(program, doc, &walk_diags,
                                server->workspace_index,
                                cancel, &pre_arena, &pre);
        bool has_imports = (pre.new_text != NULL);
        bool cold_flagged = pre.cold_workspace_warning;
        iron_arena_free(&pre_arena);

        if (has_imports) {
            IronLsp_CodeAction org;
            memset(&org, 0, sizeof(org));
            org.title               = "Organize Imports";
            org.kind                = "source.organizeImports";
            org.originating_diag    = NULL;
            org.is_preferred        = false;
            /* Placeholder edit_new_text: non-NULL so downstream
             * filtering doesn't drop the action. handlers_edit.c's
             * serializer only includes the edit field on resolve
             * (include_edit=false for the initial codeAction reply),
             * so this string never reaches the wire. */
            org.edit_new_text       = "";
            org.edit_start_line     = 0;
            org.edit_start_char     = 0;
            org.edit_end_line       = 0;
            org.edit_end_char       = 0;
            org.data_file_version   = doc->version;
            org.data_code           = ILSP_ORGANIZE_IMPORTS_SENTINEL;
            org.data_diagnostic_idx = 0;
            arr[n++] = org;

            /* Cold-workspace heads-up: emit Info showMessage once here
             * so users see the explanation even if they cancel the
             * resolve round-trip. The resolve path re-emits the same
             * message on the final edit compute (D-08). */
            if (cold_flagged) {
                ilsp_send_window_showmessage(server, doc->uri,
                    ILSP_MESSAGE_TYPE_INFO,
                    "organizeImports: full unused-removal pending "
                    "workspace analysis. Invoke Find References first, "
                    "or re-run organizeImports later.");
            }
        }
    }

    if (n > 0) {
        *out_actions = arr;
        *out_n       = n;
    }

done:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
