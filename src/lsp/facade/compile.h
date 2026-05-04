#ifndef IRON_LSP_FACADE_COMPILE_H
#define IRON_LSP_FACADE_COMPILE_H

/* Phase 2 Plan 05 Task 02 (CORE-16, CORE-17, CORE-22) -- Compilation
 * facade.
 *
 * ilsp_facade_compile is the SINGLE call site for iron_analyze_buffer
 * from anywhere in src/lsp. Every other TU that wants diagnostics for
 * a document must go through this function. This invariant is what
 * makes the CORE-22 parity proof tractable: if parity holds on the
 * facade's output, it holds for the whole LSP.
 *
 * The facade owns a per-request Iron_Arena (64 KB) and Iron_DiagList,
 * passes IRON_ANALYSIS_MODE_LSP, and routes the cancel_flag through
 * unchanged. On success it translates Iron_Diagnostic -> LSP Diagnostic
 * via src/lsp/facade/diagnostics.c and enqueues a
 * publishDiagnostics notification on server->writer. On cancellation
 * (flag observed or IRON_ERR_CANCELLED meta-diag present) the payload
 * is dropped.
 *
 * ilsp_facade_pull_diagnostic is the synchronous variant for the
 * textDocument/diagnostic pull request: it builds a
 * DocumentDiagnosticReport with resultId="v<version>" and enqueues it
 * at ILSP_PRIO_RESPONSE to the given request id.
 *
 * ilsp_facade_compile_pure is the test seam used by the parity harness:
 * it runs iron_analyze_buffer and returns the raw Iron_DiagList via
 * caller-provided arena+diags, without touching the writer queue. */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Iron_Arena + Iron_DiagList are typedefs over anonymous structs in
 * the analyzer headers, so we can't forward-declare them as
 * `struct Iron_Arena`. Include the headers directly; they're small. */
#include "util/arena.h"               /* Iron_Arena */
#include "diagnostics/diagnostics.h"  /* Iron_DiagList */
#include "parser/ast.h"               /* Iron_Program (typedef) */

struct IronLsp_Server;
struct IronLsp_Document;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IronLsp_CompileRequest {
    int32_t       version;
    _Atomic bool *cancel_flag;   /* NULL means never cancel */
} IronLsp_CompileRequest;

/* Full facade: compile + translate + publishDiagnostics enqueue. */
void ilsp_facade_compile(struct IronLsp_Server   *server,
                          struct IronLsp_Document *doc,
                          const IronLsp_CompileRequest *req);

/* Pull diagnostic: builds DocumentDiagnosticReport, enqueues as a
 * response at ILSP_PRIO_RESPONSE bound to the original request id. */
void ilsp_facade_pull_diagnostic(struct IronLsp_Server   *server,
                                  struct IronLsp_Document *doc,
                                  const char              *request_id);

/* Test seam: run the analyzer only; caller provides + owns arena+diags.
 * Used by tests/lsp/parity/test_parity_ironc_lsp.c to compare against
 * the CLI path without touching the writer queue. */
void ilsp_facade_compile_pure(struct IronLsp_Document      *doc,
                               const IronLsp_CompileRequest *req,
                               Iron_Arena                   *arena,
                               Iron_DiagList                *diags);

/* Phase 3 Plan 03 Task 02 -- NAV seam. Same contract as
 * ilsp_facade_compile_pure but also returns the Iron_Program root so
 * nav handlers can walk the resulting AST. Preserves CORE-22: still
 * the single iron_analyze_buffer call site (both entries route through
 * a private static helper inside compile.c). */
Iron_Program *ilsp_facade_compile_for_nav(
    struct IronLsp_Document      *doc,
    const IronLsp_CompileRequest *req,
    Iron_Arena                   *arena,
    Iron_DiagList                *diags);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_COMPILE_H */
