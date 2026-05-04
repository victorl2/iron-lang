#ifndef IRON_LSP_WORKERS_AST_WORKER_H
#define IRON_LSP_WORKERS_AST_WORKER_H

/* Phase 2 Plan 05 Task 01 (CORE-14, CORE-16, CORE-18) -- Per-document
 * ASTWorker thread.
 *
 * Each open document owns one ASTWorker whose main loop:
 *   1. Dequeue a message from doc->mailbox.
 *   2. On COMPILE: sleep up to 250 ms on the mailbox condvar
 *      (ilsp_mailbox_timedwait_ms) for coalescing; new posts wake us,
 *      we re-dequeue and pick up the newest coalesced version.
 *   3. Set TLS ilsp_current_doc_tls = doc; sigsetjmp(doc->abort_jmp, 1).
 *      - Return 0 path: call ilsp_facade_compile(server, doc, &req).
 *      - Return 1 path: SIGABRT recovery. Increment doc->abort_count.
 *        Strike 1 -> window/showMessage (Warning). Strike >=2 ->
 *        atomic_store(&doc->quarantined, true) + window/showMessage
 *        (Error).
 *   4. On PULL: call ilsp_facade_pull_diagnostic; free the request id.
 *   5. On SHUTDOWN: break the loop; thread returns.
 *
 * Quarantine: if atomic_load(&doc->quarantined) is true, the worker
 * drains messages but skips actual compile/pull work (just frees pull
 * ids). This prevents the same crash from tripping repeatedly.
 *
 * Thread creation uses IRON_THREAD_CREATE (pthreads on POSIX, Win32
 * HANDLE on Windows). */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_Server;
struct IronLsp_Document;

/* TLS slot used by the SIGABRT handler to find the current document's
 * sigjmp_buf. Worker sets this before each compile and clears after.
 * `_Thread_local` is the C11 spelling. */
extern _Thread_local struct IronLsp_Document *ilsp_current_doc_tls;

/* Context passed to the worker thread's main function.
 * Heap-allocated by ilsp_ast_worker_start; freed by the worker on exit. */
typedef struct IronLsp_AstWorkerCtx {
    struct IronLsp_Server   *server;
    struct IronLsp_Document *doc;
} IronLsp_AstWorkerCtx;

/* Start the ASTWorker for a given document. Creates the mailbox (if
 * not already created on the doc) and spawns the worker thread. After
 * this call, doc->worker_started is true and handlers can post to
 * doc->mailbox. */
bool ilsp_ast_worker_start(struct IronLsp_Server   *server,
                           struct IronLsp_Document *doc);

/* Signal shutdown and join the worker thread. Idempotent. Safe to call
 * on a document whose worker never started (no-op). */
void ilsp_ast_worker_shutdown_and_join(struct IronLsp_Document *doc);

/* ── Test seam (Plan 05 Task 01) ───────────────────────────────────────
 * ilsp_worker_handle_abort_strike is exposed as a non-static function
 * so Unity tests can drive the first/second-strike recovery path
 * directly without having to trigger a real SIGABRT inside the
 * pthread_create'd worker thread. The worker's sigsetjmp-1 branch
 * delegates to this helper.
 *
 * strike is the post-increment value of doc->abort_count (i.e. 1 for
 * first strike, 2+ for subsequent). The helper owns the window/showMessage
 * emission + quarantine side effect. */
void ilsp_worker_handle_abort_strike(struct IronLsp_Server   *server,
                                      struct IronLsp_Document *doc,
                                      unsigned                 strike);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_WORKERS_AST_WORKER_H */
