#ifndef IRON_LSP_OBS_ABORT_HANDLER_H
#define IRON_LSP_OBS_ABORT_HANDLER_H

/* Phase 2 Plan 05 Task 01 (CORE-18) -- SIGABRT boundary installer.
 *
 * The ASTWorker wraps every call into ilsp_facade_compile with
 * sigsetjmp(doc->abort_jmp, 1). The signal handler installed by
 * ilsp_install_abort_handler() uses a TLS pointer (ilsp_current_doc_tls,
 * defined in ast_worker.c) to find the right jmp_buf and siglongjmp's
 * into it. If the TLS pointer is NULL (e.g. SIGABRT outside a compile
 * on the main thread) the handler falls through to _exit(134) -- the
 * conventional 128+SIGABRT exit code.
 *
 * Install once per process, before any worker thread starts. */

#ifdef __cplusplus
extern "C" {
#endif

/* Install the SIGABRT sigaction with SA_SIGINFO | SA_NODEFER. Idempotent
 * across repeated calls but intended to run once at server startup. */
void ilsp_install_abort_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_OBS_ABORT_HANDLER_H */
