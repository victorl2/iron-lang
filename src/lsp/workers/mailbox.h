#ifndef IRON_LSP_WORKERS_MAILBOX_H
#define IRON_LSP_WORKERS_MAILBOX_H

/* Phase 2 Plan 05 Task 01 (CORE-14, CORE-15) -- Coalescing mailbox.
 *
 * Per-document message queue used by the ASTWorker thread. Four slots:
 *
 *   - COMPILE         : replace-in-place (coalescing). Rapid didChange
 *                       floods collapse to a single pending COMPILE with
 *                       the newest version + cancel_flag.
 *   - PULL_DIAGNOSTIC : single-slot (new pull overwrites a pending pull
 *                       for the same doc; the request id tracks the last
 *                       asker).
 *   - SHUTDOWN        : set-and-stick. Never coalesced with lower prio.
 *   - (reserved)      : future (hover/definition queues).
 *
 * Dequeue priority: SHUTDOWN > PULL_DIAGNOSTIC > COMPILE.
 *
 * Thread discipline: the MAILBOX is multi-producer (didChange handlers
 * on the dispatcher thread, the SIGABRT recovery branch via its
 * notifications path -- though that one writes through a different
 * queue; we don't reach mailbox from signal context), single-consumer
 * (the doc's ASTWorker). Internal mutex + condvar coordinate wakeups.
 *
 * Debounce: the worker calls ilsp_mailbox_timedwait_ms() AFTER draining
 * a COMPILE message to sleep up to 250 ms for follow-up posts; if a new
 * post arrives, timedwait returns IRON_TIMEDWAIT_OK and the worker
 * re-drains, picking up the coalesced newest version. */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IronLsp_MailboxMsgKind {
    ILSP_MSG_NONE             = 0,
    ILSP_MSG_COMPILE          = 1,
    ILSP_MSG_PULL_DIAGNOSTIC  = 2,
    ILSP_MSG_SHUTDOWN         = 3,
} IronLsp_MailboxMsgKind;

typedef struct IronLsp_MailboxMsg {
    IronLsp_MailboxMsgKind kind;
    int32_t                version;           /* document version for COMPILE / PULL */
    _Atomic bool          *cancel_flag;       /* per-request atomic; NULL = never cancel */
    char                  *pull_request_id;   /* malloc'd id key for PULL_DIAGNOSTIC (ownership: mailbox -> worker -> freed after use) */
} IronLsp_MailboxMsg;

typedef struct IronLsp_Mailbox IronLsp_Mailbox;

/* Lifecycle. */
IronLsp_Mailbox *ilsp_mailbox_create(void);
void             ilsp_mailbox_destroy(IronLsp_Mailbox *m);

/* Post a COMPILE. If a pending COMPILE already exists, replace it in
 * place (same slot, new version, new cancel_flag pointer). Coalescing is
 * the entire point of this function. */
void ilsp_mailbox_post_compile(IronLsp_Mailbox *m,
                                int32_t          version,
                                _Atomic bool    *cancel_flag);

/* Post a PULL_DIAGNOSTIC. request_id is duplicated into mailbox-owned
 * memory; the worker frees it after handling. If a pending pull exists,
 * the new id REPLACES the old one (the old id is freed). */
void ilsp_mailbox_post_pull(IronLsp_Mailbox *m,
                             int32_t          version,
                             const char      *request_id);

/* Post SHUTDOWN. Never coalesced; never overwrites. Sets a sticky flag
 * the dequeue path checks first. Wakes a parked worker. */
void ilsp_mailbox_post_shutdown(IronLsp_Mailbox *m);

/* Blocking dequeue. Waits on condvar until SHUTDOWN / PULL / COMPILE
 * is available; returns the highest-priority slot and clears it. Never
 * returns kind==NONE (it blocks). */
IronLsp_MailboxMsg ilsp_mailbox_dequeue(IronLsp_Mailbox *m);

/* Timed wait. Returns IRON_TIMEDWAIT_OK (0) if a new post arrived before
 * timeout, IRON_TIMEDWAIT_EXPIRED (1) on timeout, -1 on error. The
 * worker uses this for debounce: after dequeuing a COMPILE it
 * timedwait_ms(250) to give clients a chance to coalesce; OK return ->
 * re-dequeue; EXPIRED -> run the compile. */
int ilsp_mailbox_timedwait_ms(IronLsp_Mailbox *m, int timeout_ms);

/* Inspect whether a SHUTDOWN is pending without consuming it. Safe to
 * call without the lock (uses an atomic read). */
bool ilsp_mailbox_shutdown_pending(const IronLsp_Mailbox *m);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_WORKERS_MAILBOX_H */
