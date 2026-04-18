/* Phase 2 Plan 05 Task 01 (CORE-18) -- SIGABRT boundary installer.
 *
 * The signal handler runs in async-signal context. The ONLY allowed
 * actions here are:
 *   - reading the TLS pointer (a plain C load; not racy because the
 *     TLS slot is thread-local to whichever thread received SIGABRT)
 *   - calling siglongjmp (POSIX-declared async-signal-safe)
 *   - calling _exit (POSIX async-signal-safe)
 *
 * Do NOT add printf / malloc / any library call that is not on the
 * POSIX signal-safe list. Diagnostic reporting happens AFTER the
 * siglongjmp returns into the worker's sigsetjmp-1 branch, where we are
 * back in regular execution context and can log / build JSON. */

#include "lsp/obs/abort_handler.h"
#include "lsp/workers/ast_worker.h"   /* ilsp_current_doc_tls */
#include "lsp/store/document.h"

#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static void ilsp_abort_handler_impl(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    (void)info;
    (void)ucontext;
    IronLsp_Document *doc = ilsp_current_doc_tls;
    if (doc) {
        /* siglongjmp is async-signal-safe per POSIX.1-2008. Returns
         * control to the worker's sigsetjmp(doc->abort_jmp, 1) call,
         * where `1` is the recovery branch that handles the strike. */
        siglongjmp(doc->abort_jmp, 1);
    }
    /* Fell through -- no registered jmp_buf. Exit with 128+SIGABRT. */
    _exit(134);
}

void ilsp_install_abort_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ilsp_abort_handler_impl;
    sa.sa_flags     = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGABRT, &sa, NULL);
}
