#ifndef ILSP_CODEACTION_REGISTRY_H
#define ILSP_CODEACTION_REGISTRY_H

/* Phase 4 Plan 04-04 Task 01 (EDIT-07, EDIT-08) — Code-action registry.
 *
 * Const-sorted table of { Iron_Diagnostic.code → quickfix handler fn
 * pointer } with bsearch lookup. Mirrors the dispatch table pattern in
 * src/lsp/server/dispatch.c:91-138.
 *
 * Each quickfix handler consumes:
 *   diag      — the originating Iron_Diagnostic (code + span + suggestion)
 *   doc       — open document (UTF-8 text buffer, line index, version)
 *   wi        — workspace index (currently unused; reserved for cross-file
 *               quickfixes in future plans)
 *   arena     — per-request Iron_Arena for all returned strings
 *   out_arr   — caller-provided buffer of capacity out_cap
 *               (>= ILSP_QUICKFIX_MAX_VARIANTS)
 *   out_cap   — number of available slots in out_arr
 *   out_n     — handler writes the count of populated slots
 *
 * Returned IronLsp_CodeAction is a minimal server-side shape; handlers_edit.c
 * serializes it to the LSP CodeAction wire format. The five P1 handlers
 * all emit a single-file single-edit TextEdit (no cross-file changes);
 * Plan 04-05's organizeImports added a documentChanges variant; Phase 12
 * extends the shape with multi-edit (D-27) and command-style (D-14)
 * branches plus per-action variant disambiguation (D-31).
 *
 * Data round-trip for codeAction/resolve (D-07 + D-31):
 *   data_file_version, data_code, data_diagnostic_idx, data_variant_idx
 *   are populated by codeaction.c before serialization; on resolve the
 *   same quad is re-decoded and the handler is re-invoked, dispatching
 *   on data_variant_idx to pick which slot of the multi-action result
 *   to return.
 *
 * Refusal protocol (D-11):
 *   When a handler cannot produce any valid action it sets *out_n = 0
 *   and returns. Callers drop diagnostics whose handler emits zero
 *   actions. Per-slot refusal is also supported — a handler may emit
 *   only out_arr[0] (set *out_n = 1) when out_arr[1] is infeasible
 *   (e.g., QF-05 Action B when callee resolution misses).
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "parser/ast.h"

struct IronLsp_Document;
struct IronLsp_WorkspaceIndex;

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 12 Plan 12-01 (D-11) — multi-action quickfix substrate.
 * QF-04/QF-05 (Plan 12-03) emit up to 2 actions per diagnostic; bump
 * this constant when a future requirement needs more. */
#define ILSP_QUICKFIX_MAX_VARIANTS 2

/* Phase 12 Plan 12-01 (D-27) — multi-edit TextEdit array element.
 * QF-03 (Plan 12-03) emits 2 atomic edits per CodeAction (remove inline
 * default + insert into init body). new_text == "" indicates a deletion
 * range; non-empty indicates a replacement. All strings arena-owned. */
typedef struct IronLsp_TextEdit {
    uint32_t    start_line;
    uint32_t    start_char;
    uint32_t    end_line;
    uint32_t    end_char;
    const char *new_text;  /* arena-owned; "" means deletion. */
} IronLsp_TextEdit;

/* Server-side CodeAction value. handlers_edit.c serializes this to LSP
 * wire format; consumers outside the code-action subtree should not need
 * to look inside.
 *
 * Shape rules (Phase 12 D-14 / D-15 / D-27):
 *   Exactly one of the following branches is populated per action; the
 *   wire serializer picks the matching JSON envelope. All branches are
 *   mutually exclusive — when a handler populates command_id it leaves
 *   edit_text_edits[] and the legacy edit_* fields zero/NULL, etc.
 *
 *     A. command-style (D-14): command_id != NULL → wire emits
 *        `command: { title, command, arguments[] }`; no `edit` field.
 *     B. multi-edit (D-27): edit_text_edits_n > 0 → wire emits an
 *        N-entry edits[] array inside documentChanges/changes.
 *     C. legacy single-edit: edit_new_text != NULL → wire emits a
 *        single TextEdit inside documentChanges/changes (preserved
 *        verbatim for the 5 existing handlers + organizeImports).
 *     D. refusal: all three branches empty (handler set *out_n = 0
 *        before returning, so the caller already dropped this slot;
 *        an all-empty action that survives is treated as no-edit). */
typedef struct IronLsp_CodeAction {
    const char             *title;              /* arena-owned, non-NULL on success */
    const char             *kind;               /* always "quickfix" for P1 codes */
    const Iron_Diagnostic  *originating_diag;   /* for CodeAction.diagnostics */
    bool                    is_preferred;

    /* Legacy single-file single-edit shape (Phase 4 D-06). All four
     * endpoints are LSP-encoding-aware (0-indexed line, 0-indexed
     * character in the negotiated encoding). The handler calls
     * ilsp_span_to_lsp_range to fill them. */
    uint32_t                edit_start_line;
    uint32_t                edit_start_char;
    uint32_t                edit_end_line;
    uint32_t                edit_end_char;
    const char             *edit_new_text;      /* arena-owned; NULL => not legacy single-edit */

    /* Opaque data round-trip (set by codeaction.c before serialization). */
    int                     data_file_version;
    int                     data_code;
    int                     data_diagnostic_idx;

    /* Phase 12 Plan 12-01 (D-27) — multi-edit fast path.
     * When edit_text_edits_n > 0 the wire serializer emits an N-entry
     * edits[] array; the legacy edit_start_line/.../edit_new_text
     * fields stay zero for these actions. Mutually exclusive with
     * command_id and the legacy single-edit fields. */
    const IronLsp_TextEdit *edit_text_edits;
    size_t                  edit_text_edits_n;

    /* Phase 12 Plan 12-01 (D-14) — command-style action (QF-01 in Plan
     * 12-03). When command_id != NULL the wire serializer emits a
     * `command: {title, command, arguments[]}` envelope and OMITS the
     * `edit` field. Mutually exclusive with edit_text_edits[] and the
     * legacy edit_* fields. */
    const char             *command_title;      /* arena-owned, may be NULL (falls back to title) */
    const char             *command_id;         /* arena-owned, e.g. "iron.migrate.fromV2ToV3" */
    const char *const      *command_args;       /* arena-owned arg strings */
    size_t                  command_args_n;

    /* Phase 12 Plan 12-01 (D-31) — variant disambiguation for multi-action
     * handlers (QF-04/QF-05). Default 0 for single-action handlers and
     * legacy clients without the data.variant_idx wire key. */
    int                     data_variant_idx;
} IronLsp_CodeAction;

/* Phase 12 Plan 12-01 (D-11) — multi-action emit shape.
 *
 * Caller provides `out_arr` of capacity `out_cap` (>=
 * ILSP_QUICKFIX_MAX_VARIANTS) on the per-request arena; handler
 * writes 0..out_cap actions and sets *out_n to the count emitted.
 * Refusal: *out_n = 0 (do NOT rely on edit_new_text == NULL —
 * multi-edit and command-style actions leave that NULL by design).
 *
 * The orchestrator (codeaction.c) stamps data_file_version /
 * data_code / data_diagnostic_idx / data_variant_idx on each emitted
 * entry; resolve.c dispatches on data_variant_idx to materialize the
 * matching slot's edit. */
typedef void (*IronLsp_QuickfixFn)(const Iron_Diagnostic           *diag,
                                     struct IronLsp_Document         *doc,
                                     struct IronLsp_WorkspaceIndex   *wi,
                                     Iron_Arena                      *arena,
                                     IronLsp_CodeAction              *out_arr,
                                     size_t                           out_cap,
                                     size_t                          *out_n);

typedef struct {
    int                 code;      /* Iron_Diagnostic.code (sorted ASC) */
    IronLsp_QuickfixFn  handler;
} IronLsp_QuickfixEntry;

/* Const-sorted table (sorted by code ASC for bsearch). */
extern const IronLsp_QuickfixEntry ilsp_quickfix_table[];
extern const size_t                ilsp_quickfix_table_size;

/* Return the handler for `code`, or NULL if unknown. Thread-safe
 * (read-only table). */
IronLsp_QuickfixFn ilsp_quickfix_lookup(int code);

/* Per-code handler declarations (one TU each). All match the Phase 12
 * (out_arr, out_cap, out_n) signature. */
void ilsp_quickfix_undefined_var        (const Iron_Diagnostic *, struct IronLsp_Document *, struct IronLsp_WorkspaceIndex *, Iron_Arena *, IronLsp_CodeAction *out_arr, size_t out_cap, size_t *out_n);
void ilsp_quickfix_type_mismatch_literal(const Iron_Diagnostic *, struct IronLsp_Document *, struct IronLsp_WorkspaceIndex *, Iron_Arena *, IronLsp_CodeAction *out_arr, size_t out_cap, size_t *out_n);
void ilsp_quickfix_missing_return       (const Iron_Diagnostic *, struct IronLsp_Document *, struct IronLsp_WorkspaceIndex *, Iron_Arena *, IronLsp_CodeAction *out_arr, size_t out_cap, size_t *out_n);
void ilsp_quickfix_unused_import        (const Iron_Diagnostic *, struct IronLsp_Document *, struct IronLsp_WorkspaceIndex *, Iron_Arena *, IronLsp_CodeAction *out_arr, size_t out_cap, size_t *out_n);
void ilsp_quickfix_redundant_cast       (const Iron_Diagnostic *, struct IronLsp_Document *, struct IronLsp_WorkspaceIndex *, Iron_Arena *, IronLsp_CodeAction *out_arr, size_t out_cap, size_t *out_n);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_CODEACTION_REGISTRY_H */
