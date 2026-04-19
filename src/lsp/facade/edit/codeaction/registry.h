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
 *   out       — pre-allocated IronLsp_CodeAction the handler fills in
 *
 * Returned IronLsp_CodeAction is a minimal server-side shape; handlers_edit.c
 * serializes it to the LSP CodeAction wire format. The five P1 handlers
 * all emit a single-file single-edit TextEdit (no cross-file changes);
 * Plan 04-05's organizeImports will add a documentChanges variant.
 *
 * Data round-trip for codeAction/resolve (D-07):
 *   data_file_version, data_code, data_diagnostic_idx — populated by
 *   handlers_edit.c before serialization; on resolve the same triple is
 *   re-decoded and the handler is re-invoked to re-materialize the edit.
 *
 * When a handler cannot produce a valid edit (e.g. missing_return on a
 * return type without an obvious zero literal) it leaves out->edit_new_text
 * == NULL — caller drops the action from the result array.
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

/* Server-side CodeAction value. handlers_edit.c serializes this to LSP
 * wire format; consumers outside the code-action subtree should not need
 * to look inside. */
typedef struct IronLsp_CodeAction {
    const char             *title;              /* arena-owned, non-NULL on success */
    const char             *kind;               /* always "quickfix" for P1 codes */
    const Iron_Diagnostic  *originating_diag;   /* for CodeAction.diagnostics */
    bool                    is_preferred;

    /* Single-file single-edit shape. All four endpoints are
     * LSP-encoding-aware (0-indexed line, 0-indexed character in the
     * negotiated encoding). The handler calls ilsp_span_to_lsp_range
     * to fill them. */
    uint32_t                edit_start_line;
    uint32_t                edit_start_char;
    uint32_t                edit_end_line;
    uint32_t                edit_end_char;
    const char             *edit_new_text;      /* arena-owned; NULL => skip */

    /* Opaque data round-trip (set by handlers_edit.c before serialization). */
    int                     data_file_version;
    int                     data_code;
    int                     data_diagnostic_idx;
} IronLsp_CodeAction;

typedef void (*IronLsp_QuickfixFn)(const Iron_Diagnostic           *diag,
                                     struct IronLsp_Document         *doc,
                                     struct IronLsp_WorkspaceIndex   *wi,
                                     Iron_Arena                      *arena,
                                     IronLsp_CodeAction              *out);

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

/* Per-code handler declarations (one TU each). */
void ilsp_quickfix_undefined_var        (const Iron_Diagnostic *, struct IronLsp_Document *, struct IronLsp_WorkspaceIndex *, Iron_Arena *, IronLsp_CodeAction *);
void ilsp_quickfix_type_mismatch_literal(const Iron_Diagnostic *, struct IronLsp_Document *, struct IronLsp_WorkspaceIndex *, Iron_Arena *, IronLsp_CodeAction *);
void ilsp_quickfix_missing_return       (const Iron_Diagnostic *, struct IronLsp_Document *, struct IronLsp_WorkspaceIndex *, Iron_Arena *, IronLsp_CodeAction *);
void ilsp_quickfix_unused_import        (const Iron_Diagnostic *, struct IronLsp_Document *, struct IronLsp_WorkspaceIndex *, Iron_Arena *, IronLsp_CodeAction *);
void ilsp_quickfix_redundant_cast       (const Iron_Diagnostic *, struct IronLsp_Document *, struct IronLsp_WorkspaceIndex *, Iron_Arena *, IronLsp_CodeAction *);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_CODEACTION_REGISTRY_H */
