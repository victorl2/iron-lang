#ifndef IRON_LSP_FACADE_DIAGNOSTICS_H
#define IRON_LSP_FACADE_DIAGNOSTICS_H

/* Phase 2 Plan 05 Task 02 (CORE-16, CORE-17) -- Iron_DiagList -> LSP
 * Diagnostic[] translation + notification/response builders.
 *
 * Severity mapping (LSP 3.17 §Diagnostic):
 *   IRON_DIAG_ERROR   -> 1 (Error)
 *   IRON_DIAG_WARNING -> 2 (Warning)
 *   IRON_DIAG_NOTE    -> 3 (Information)
 *
 * Code field: stringified "E0204" form (prefix 'E' + zero-padded 4-digit).
 * Source field: always "iron".
 * Message field: from Iron_Diagnostic.message (arena-interned).
 * If d->suggestion is non-NULL, it is attached as a single
 * relatedInformation entry at the same range with the prefix "suggestion: ".
 *
 * Both builders return a yyjson_mut_doc* the caller must free via
 * yyjson_mut_doc_free, but the body bytes live inside the provided
 * arena (routed through ilsp_json_alc). */

#include "diagnostics/diagnostics.h"   /* Iron_DiagList */
#include "lsp/facade/types.h"          /* IronLsp_PositionEncoding */
#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_Document;

/* Build a textDocument/publishDiagnostics notification. Returned doc's
 * root is `{ "jsonrpc":"2.0", "method":"textDocument/publishDiagnostics",
 * "params": { "uri": ..., "version": ..., "diagnostics": [...] } }`. */
yyjson_mut_doc *ilsp_build_publish_diagnostics(const Iron_DiagList           *diags,
                                                const struct IronLsp_Document *doc,
                                                IronLsp_PositionEncoding       enc,
                                                Iron_Arena                    *arena);

/* Build a textDocument/diagnostic response body (pull-diagnostic).
 * Returned doc's root is `{ "jsonrpc":"2.0", "id": ...,
 * "result": { "kind":"full", "resultId":"v<version>", "items":[...] } }`.
 * The caller clones/attaches the id. The response id is written into
 * `out_root`; caller composes the full response object. */
yyjson_mut_doc *ilsp_build_pull_diagnostic_report(const Iron_DiagList           *diags,
                                                   const struct IronLsp_Document *doc,
                                                   IronLsp_PositionEncoding       enc,
                                                   const char                    *request_id,
                                                   Iron_Arena                    *arena);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_DIAGNOSTICS_H */
