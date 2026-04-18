/* Phase 2 Plan 05 Task 02 -- Compilation facade.
 *
 * This file is the SINGLE call site for iron_analyze_buffer from
 * anywhere under src/lsp. Task 01 lands empty stubs so the worker can
 * link against the facade symbols while task 02 is in flight. Task 02
 * replaces the stubs with the real implementation:
 *
 *   - ilsp_facade_compile_pure  : run iron_analyze_buffer only.
 *   - ilsp_facade_compile       : + translate + publishDiagnostics enqueue.
 *   - ilsp_facade_pull_diagnostic : + build DocumentDiagnosticReport response.
 *
 * DO NOT add iron_analyze_buffer call sites outside this TU. The CORE-22
 * parity harness asserts that `grep iron_analyze_buffer` across
 * src/lsp yields exactly this file. */

#include "lsp/facade/compile.h"

/* Task 01 stubs: no-ops. Task 02 fills these in. */

void ilsp_facade_compile(struct IronLsp_Server *server,
                          struct IronLsp_Document *doc,
                          const IronLsp_CompileRequest *req) {
    (void)server; (void)doc; (void)req;
}

void ilsp_facade_pull_diagnostic(struct IronLsp_Server *server,
                                  struct IronLsp_Document *doc,
                                  const char *request_id) {
    (void)server; (void)doc; (void)request_id;
}

void ilsp_facade_compile_pure(struct IronLsp_Document      *doc,
                               const IronLsp_CompileRequest *req,
                               struct Iron_Arena            *arena,
                               struct Iron_DiagList         *diags) {
    (void)doc; (void)req; (void)arena; (void)diags;
}
