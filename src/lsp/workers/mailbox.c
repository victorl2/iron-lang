/* Phase 2 Plan 05 Task 01 (CORE-14, CORE-15) -- Coalescing mailbox impl.
 *
 * See mailbox.h for the contract. Slot layout:
 *
 *   struct IronLsp_Mailbox {
 *     iron_mutex_t  lock;
 *     iron_cond_t   cond;
 *     _Atomic bool  shutdown_flag;   // lock-free shutdown-pending check
 *     bool          compile_has;     // slot occupancy bits (under lock)
 *     bool          pull_has;
 *     bool          shutdown_has;
 *     IronLsp_MailboxMsg compile;    // replace-in-place on post_compile
 *     IronLsp_MailboxMsg pull;
 *   };
 *
 * Dequeue priority: SHUTDOWN > PULL > COMPILE. We always drain every
 * pending item before returning NONE -- but dequeue blocks until at
 * least one slot is occupied, so it never actually returns NONE.
 *
 * All synchronization uses the IRON_MUTEX_* / IRON_COND_* macros from
 * src/runtime/iron_runtime.h. This mirrors the writer queue pattern in
 * src/lsp/transport/writer.c and avoids direct pthread_* calls. */

#include "lsp/workers/mailbox.h"
#include "runtime/iron_runtime.h"

#include <stdlib.h>
#include <string.h>

struct IronLsp_Mailbox {
    iron_mutex_t        lock;
    iron_cond_t         cond;

    /* Lock-free flag for ilsp_mailbox_shutdown_pending -- callers that
     * only need to peek at shutdown can do so without the lock. */
    _Atomic bool        shutdown_flag;

    bool                compile_has;
    bool                pull_has;
    bool                shutdown_has;

    IronLsp_MailboxMsg  compile;
    IronLsp_MailboxMsg  pull;
};

IronLsp_Mailbox *ilsp_mailbox_create(void) {
    IronLsp_Mailbox *m = (IronLsp_Mailbox *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    IRON_MUTEX_INIT(m->lock);
    IRON_COND_INIT(m->cond);
    atomic_init(&m->shutdown_flag, false);
    m->compile_has  = false;
    m->pull_has     = false;
    m->shutdown_has = false;
    return m;
}

void ilsp_mailbox_destroy(IronLsp_Mailbox *m) {
    if (!m) return;
    /* Free any lingering pull request id. Caller is expected to have
     * drained all messages (worker joined) before destroying. */
    if (m->pull_has && m->pull.pull_request_id) {
        free(m->pull.pull_request_id);
    }
    IRON_COND_DESTROY(m->cond);
    IRON_MUTEX_DESTROY(m->lock);
    free(m);
}

void ilsp_mailbox_post_compile(IronLsp_Mailbox *m,
                                int32_t          version,
                                _Atomic bool    *cancel_flag) {
    if (!m) return;
    IRON_MUTEX_LOCK(m->lock);
    /* Replace-in-place: if a COMPILE is already pending, overwrite the
     * version and cancel_flag. The previous cancel_flag pointer is owned
     * by the cancel registry (not us), so we do NOT free it. */
    m->compile.kind            = ILSP_MSG_COMPILE;
    m->compile.version         = version;
    m->compile.cancel_flag     = cancel_flag;
    m->compile.pull_request_id = NULL;
    m->compile_has             = true;
    IRON_COND_SIGNAL(m->cond);
    IRON_MUTEX_UNLOCK(m->lock);
}

void ilsp_mailbox_post_pull(IronLsp_Mailbox *m,
                             int32_t          version,
                             const char      *request_id) {
    if (!m) return;
    char *dup = request_id ? strdup(request_id) : NULL;
    IRON_MUTEX_LOCK(m->lock);
    /* If a pull is already pending, free the old id and replace. */
    if (m->pull_has && m->pull.pull_request_id) {
        free(m->pull.pull_request_id);
    }
    m->pull.kind            = ILSP_MSG_PULL_DIAGNOSTIC;
    m->pull.version         = version;
    m->pull.cancel_flag     = NULL;
    m->pull.pull_request_id = dup;
    m->pull_has             = true;
    IRON_COND_SIGNAL(m->cond);
    IRON_MUTEX_UNLOCK(m->lock);
}

void ilsp_mailbox_post_shutdown(IronLsp_Mailbox *m) {
    if (!m) return;
    /* Set the lock-free flag first so shutdown_pending() readers observe
     * it even before we acquire the mutex. */
    atomic_store(&m->shutdown_flag, true);
    IRON_MUTEX_LOCK(m->lock);
    m->shutdown_has = true;
    IRON_COND_BROADCAST(m->cond);
    IRON_MUTEX_UNLOCK(m->lock);
}

IronLsp_MailboxMsg ilsp_mailbox_dequeue(IronLsp_Mailbox *m) {
    IronLsp_MailboxMsg out = {0};
    if (!m) { out.kind = ILSP_MSG_NONE; return out; }

    IRON_MUTEX_LOCK(m->lock);
    while (!m->shutdown_has && !m->pull_has && !m->compile_has) {
        IRON_COND_WAIT(m->cond, m->lock);
    }
    /* Priority: SHUTDOWN > PULL > COMPILE. */
    if (m->shutdown_has) {
        out.kind = ILSP_MSG_SHUTDOWN;
        out.version = 0;
        out.cancel_flag = NULL;
        out.pull_request_id = NULL;
        m->shutdown_has = false;
    } else if (m->pull_has) {
        out = m->pull;   /* transfers ownership of pull_request_id to caller */
        m->pull.pull_request_id = NULL;
        m->pull_has = false;
    } else {
        out = m->compile;
        m->compile_has = false;
    }
    IRON_MUTEX_UNLOCK(m->lock);
    return out;
}

int ilsp_mailbox_timedwait_ms(IronLsp_Mailbox *m, int timeout_ms) {
    if (!m) return -1;
    int rc;
    IRON_MUTEX_LOCK(m->lock);
    /* Already have an item? Return OK immediately (caller should
     * re-dequeue). This matches the debounce semantics: a post that
     * arrived while we were transitioning wakes us "instantly". */
    if (m->shutdown_has || m->pull_has || m->compile_has) {
        IRON_MUTEX_UNLOCK(m->lock);
        return IRON_TIMEDWAIT_OK;
    }
    rc = iron_cond_timedwait_ms(&m->cond, &m->lock, timeout_ms);
    IRON_MUTEX_UNLOCK(m->lock);
    return rc;
}

bool ilsp_mailbox_shutdown_pending(const IronLsp_Mailbox *m) {
    if (!m) return false;
    /* Cast away const for the atomic load. */
    IronLsp_Mailbox *mm = (IronLsp_Mailbox *)m;
    return atomic_load(&mm->shutdown_flag);
}
