#ifndef ILSP_CODEACTION_CODEACTION_H
#define ILSP_CODEACTION_CODEACTION_H

/* Phase 4 Plan 04-04 Task 02 (EDIT-07, EDIT-08) — code-action facade.
 *
 * Two entry points:
 *
 *   ilsp_facade_code_action
 *     Walks the document's current diagnostic list in the requested
 *     range, optionally filters by context.only kinds, and produces a
 *     lightweight IronLsp_CodeAction array WITHOUT the edit field — the
 *     edit is re-computed lazily via codeAction/resolve.
 *
 *   ilsp_facade_code_action_resolve
 *     Given opaque data {file_version, code, diagnostic_idx} from a
 *     previously-returned CodeAction, re-materializes the edit IF the
 *     file version still matches and the diagnostic is still present.
 *     On staleness or unknown index, writes an empty out (edit_new_text
 *     == NULL) so the handler returns edit:null to the client rather
 *     than an RPC error (D-07).
 *
 * Layering: handlers_edit.c is the sole caller; it owns yyjson
 * serialization. This facade is AST-walking-free: the diagnostic list
 * is pulled from ilsp_facade_compile_for_nav's Iron_DiagList, and the
 * per-diagnostic quickfix handler receives that diag + the raw doc.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "diagnostics/diagnostics.h"
#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/types.h"
#include "util/arena.h"

struct IronLsp_Server;
struct IronLsp_Document;

#ifdef __cplusplus
extern "C" {
#endif

/* Filter for context.only (e.g. ["quickfix"] or ["source.organizeImports"]).
 * NULL array => no filter (accept all). Each entry is a kind string;
 * a diagnostic's handler emits kind="quickfix", so
 * - if any entry is "quickfix" (or starts with "quickfix."): accept
 * - otherwise the quickfix is rejected and not emitted. */
typedef struct {
    const char **kinds;    /* array of kind strings; may be NULL */
    size_t       kind_n;
} IronLsp_CodeActionOnly;

/* Compute CodeAction candidates in `range`. Out arrays are allocated
 * into `arena`; NULL fields (edit_new_text) indicate handlers that
 * refused the input. Caller filters out refusals before emitting. */
void ilsp_facade_code_action(struct IronLsp_Server         *server,
                                struct IronLsp_Document       *doc,
                                IronLsp_Range                  range,
                                const IronLsp_CodeActionOnly  *only,
                                _Atomic bool                  *cancel,
                                Iron_Arena                    *arena,
                                IronLsp_CodeAction           **out_actions,
                                size_t                        *out_n);

/* Re-materialize edit for a previously-returned CodeAction. Writes
 * out->edit_new_text = NULL when stale or unresolvable. NEVER RPC-errors. */
void ilsp_facade_code_action_resolve(struct IronLsp_Server       *server,
                                        struct IronLsp_Document     *doc,
                                        int                          data_file_version,
                                        int                          data_code,
                                        int                          data_diagnostic_idx,
                                        _Atomic bool                *cancel,
                                        Iron_Arena                  *arena,
                                        IronLsp_CodeAction          *out);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_CODEACTION_CODEACTION_H */
