/* Phase 2 Plan 05 Task 01 (CORE-14, CORE-16, CORE-18) -- ASTWorker.
 *
 * See ast_worker.h for the contract. Main loop shape:
 *
 *   for (;;) {
 *     msg = mailbox_dequeue();
 *     if (msg.kind == SHUTDOWN) break;
 *     if (quarantined) drain-and-continue;
 *     if (msg.kind == COMPILE) {
 *       // Debounce: timedwait 250ms; OK -> coalesce + re-dequeue
 *       while (timedwait_ms(250) == OK) {
 *         next = dequeue();
 *         if (next.kind == SHUTDOWN) return;
 *         if (next.kind == COMPILE && next.version > msg.version)
 *           msg = next;
 *       }
 *       // Run compile inside sigsetjmp boundary
 *       ilsp_current_doc_tls = doc;
 *       if (sigsetjmp(doc->abort_jmp, 1) == 0) {
 *         ilsp_facade_compile(server, doc, &req);
 *       } else {
 *         doc->abort_count++;
 *         ilsp_worker_handle_abort_strike(server, doc, doc->abort_count);
 *       }
 *       ilsp_current_doc_tls = NULL;
 *     } else if (msg.kind == PULL_DIAGNOSTIC) {
 *       ilsp_facade_pull_diagnostic(server, doc, msg.pull_request_id);
 *       free(msg.pull_request_id);
 *     }
 *   }
 */

#include "lsp/workers/ast_worker.h"
#include "lsp/workers/mailbox.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "lsp/server/notifications.h"
#include "lsp/facade/compile.h"
#include "runtime/iron_runtime.h"   /* IRON_THREAD_* + iron_cond_timedwait_ms */

#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Debounce window: after a COMPILE arrives, wait this long for follow-up
 * posts to coalesce before actually running the analyzer. 250 ms is the
 * CORE-16 target -- tight enough that users feel responsive, loose
 * enough that a fast typist doesn't trigger per-keystroke compiles. */
#define ILSP_DEBOUNCE_MS 250

/* TLS slot read by the SIGABRT handler. Definition here; declaration is
 * extern in ast_worker.h. */
_Thread_local IronLsp_Document *ilsp_current_doc_tls = NULL;

void ilsp_worker_handle_abort_strike(IronLsp_Server   *server,
                                      IronLsp_Document *doc,
                                      unsigned          strike) {
    if (!doc) return;
    fprintf(stderr, "ironls: SIGABRT on doc=%s strike=%u\n",
            doc->uri ? doc->uri : "<unknown>", strike);

    if (strike == 1) {
        char text[512];
        snprintf(text, sizeof(text),
                 "iron-lsp: analysis crashed for %s",
                 doc->uri ? doc->uri : "<unknown>");
        ilsp_send_window_showmessage(server, doc->uri,
                                      /*Warning*/ 2, text);
    } else {
        atomic_store(&doc->quarantined, true);
        char text[512];
        snprintf(text, sizeof(text),
                 "iron-lsp: %s quarantined due to repeated analysis crashes",
                 doc->uri ? doc->uri : "<unknown>");
        ilsp_send_window_showmessage(server, doc->uri,
                                      /*Error*/ 1, text);
    }
}

/* Main thread function. arg is a heap IronLsp_AstWorkerCtx* owned by
 * this function; freed before return. */
static void *ilsp_ast_worker_main(void *arg) {
    IronLsp_AstWorkerCtx *ctx = (IronLsp_AstWorkerCtx *)arg;
    IronLsp_Server       *server = ctx->server;
    IronLsp_Document     *doc    = ctx->doc;

    for (;;) {
        IronLsp_MailboxMsg msg = ilsp_mailbox_dequeue(doc->mailbox);

        if (msg.kind == ILSP_MSG_SHUTDOWN) break;

        /* Quarantined doc: drain any message without doing real work. */
        if (atomic_load(&doc->quarantined)) {
            if (msg.pull_request_id) free(msg.pull_request_id);
            continue;
        }

        if (msg.kind == ILSP_MSG_COMPILE) {
            /* Debounce: give the client a 250 ms window to send more
             * edits. Every new post wakes us; we re-dequeue and pick up
             * the newest (coalesced) version. */
            for (;;) {
                int rc = ilsp_mailbox_timedwait_ms(doc->mailbox,
                                                    ILSP_DEBOUNCE_MS);
                if (rc != IRON_TIMEDWAIT_OK) break;
                IronLsp_MailboxMsg next = ilsp_mailbox_dequeue(doc->mailbox);
                if (next.kind == ILSP_MSG_SHUTDOWN) {
                    /* Thread exit: free any pending ids; skip the compile. */
                    if (next.pull_request_id) free(next.pull_request_id);
                    free(ctx);
                    return NULL;
                }
                if (next.kind == ILSP_MSG_COMPILE) {
                    if (next.version >= msg.version) msg = next;
                } else if (next.kind == ILSP_MSG_PULL_DIAGNOSTIC) {
                    /* Handle pull inline: run a facade-pull then continue
                     * debouncing the COMPILE. The pull sees the current
                     * buffer. */
                    ilsp_facade_pull_diagnostic(server, doc,
                                                 next.pull_request_id);
                    if (next.pull_request_id) free(next.pull_request_id);
                }
            }

            /* Debounce expired. Run compile inside the SIGABRT boundary. */
            ilsp_current_doc_tls = doc;
            if (sigsetjmp(doc->abort_jmp, 1) == 0) {
                IronLsp_CompileRequest req = {
                    .version     = msg.version,
                    .cancel_flag = msg.cancel_flag,
                };
                ilsp_facade_compile(server, doc, &req);
            } else {
                /* SIGABRT recovery branch. */
                doc->abort_count++;
                ilsp_worker_handle_abort_strike(server, doc, doc->abort_count);
            }
            ilsp_current_doc_tls = NULL;

        } else if (msg.kind == ILSP_MSG_PULL_DIAGNOSTIC) {
            /* Synchronous pull: run the facade pull path inside the same
             * sigsetjmp boundary so a crash during pull also quarantines. */
            ilsp_current_doc_tls = doc;
            if (sigsetjmp(doc->abort_jmp, 1) == 0) {
                ilsp_facade_pull_diagnostic(server, doc, msg.pull_request_id);
            } else {
                doc->abort_count++;
                ilsp_worker_handle_abort_strike(server, doc, doc->abort_count);
            }
            ilsp_current_doc_tls = NULL;
            if (msg.pull_request_id) free(msg.pull_request_id);
        }
    }

    free(ctx);
    return NULL;
}

bool ilsp_ast_worker_start(IronLsp_Server *server, IronLsp_Document *doc) {
    if (!server || !doc) return false;
    if (doc->worker_started) return true;

    if (!doc->mailbox) {
        doc->mailbox = ilsp_mailbox_create();
        if (!doc->mailbox) return false;
    }

    IronLsp_AstWorkerCtx *ctx = (IronLsp_AstWorkerCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) return false;
    ctx->server = server;
    ctx->doc    = doc;

    if (IRON_THREAD_CREATE(doc->worker_thread,
                            ilsp_ast_worker_main,
                            ctx) != 0) {
        free(ctx);
        return false;
    }
    doc->worker_started = true;
    return true;
}

void ilsp_ast_worker_shutdown_and_join(IronLsp_Document *doc) {
    if (!doc) return;
    if (!doc->worker_started) return;

    /* Mailbox may have been destroyed ahead of us in test teardown --
     * guard against double shutdown. */
    if (doc->mailbox) ilsp_mailbox_post_shutdown(doc->mailbox);
    IRON_THREAD_JOIN(doc->worker_thread);
    doc->worker_started = false;
}
