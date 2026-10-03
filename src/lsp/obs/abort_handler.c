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
#include "util/os.h"

static void ilsp_abort_recover(void) {
    IronLsp_Document *doc = ilsp_current_doc_tls;
    if (doc) {
        /* Returns control to the worker's ILSP_SETJMP(doc->abort_jmp)
         * call, where the non-zero branch handles the strike. */
        ILSP_LONGJMP(doc->abort_jmp, 1);
    }
    /* Fell through -- no registered jmp_buf. Exit with 128+SIGABRT. */
    _exit(134);
}

#ifdef _WIN32
static void ilsp_abort_handler_impl(int sig) {
    (void)sig;
    ilsp_abort_recover();
}
#else
static void ilsp_abort_handler_impl(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    (void)info;
    (void)ucontext;
    ilsp_abort_recover();
}
#endif

void ilsp_install_abort_handler(void) {
#ifdef _WIN32
    /* abort() raises SIGABRT on the calling thread through the CRT's
     * signal(); a longjmp out of that handler is a plain unwind. */
    signal(SIGABRT, ilsp_abort_handler_impl);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ilsp_abort_handler_impl;
    sa.sa_flags     = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGABRT, &sa, NULL);
#endif
}
