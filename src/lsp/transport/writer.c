/* Phase 2 Plan 02 Task 03 -- Bounded writer queue + drop policy.
 *
 * The queue is a fixed-size circular array of 256 slots. head/tail/count
 * form the standard ring-buffer invariants; all state transitions happen
 * under `lock`. Enqueuers signal `cv` after inserting; the writer thread
 * waits on `cv` while `count == 0 && !shutdown`.
 *
 * Drop policy (executed inside the enqueue critical section, BEFORE
 * inserting the new item):
 *   1. If count < capacity: normal insert at tail, return OK.
 *   2. Queue full: walk from head looking for the oldest ILSP_PRIO_LOG.
 *      If found: free its body, shift subsequent slots down by one,
 *      insert the new item at the now-open tail slot, return
 *      OK_DROPPED_LOG.
 *   3. Else walk for oldest ILSP_PRIO_NOTIFICATION; same shift, return
 *      OK_DROPPED_NOTIFICATION.
 *   4. Else all slots are responses: free the NEW item's body (caller
 *      relinquished ownership), return FULL_DROPPED.
 *
 * The shift is O(N) in the worst case. At capacity 256 and typical LSP
 * traffic (tens of messages/sec) the extra work is negligible; the
 * invariant "no dropped response while logs exist" is worth the cost. */
#include "lsp/transport/writer.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/iron_runtime.h"   /* IRON_MUTEX_* / IRON_COND_* / IRON_THREAD_* */

struct IronLsp_Writer {
    FILE                 *sink;
    IronLsp_OutboundItem  queue[ILSP_WRITER_QUEUE_CAPACITY];
    int                   head;      /* index of oldest item */
    int                   count;     /* 0..CAPACITY */
    iron_mutex_t          lock;
    iron_cond_t           cv;
    iron_thread_t         thread;
    bool                  started;
    atomic_bool           shutdown;
};

/* Write one framed message to the sink. Caller holds NO lock. */
static void writer_write_one(IronLsp_Writer *w, IronLsp_OutboundItem *item) {
    fprintf(w->sink, "Content-Length: %zu\r\n\r\n", item->len);
    if (item->len > 0 && item->body != NULL) {
        fwrite(item->body, 1, item->len, w->sink);
    }
    fflush(w->sink);
    free(item->body);
    item->body = NULL;
}

/* Writer thread entry. */
static void *writer_thread_main(void *arg) {
    IronLsp_Writer *w = (IronLsp_Writer *)arg;
    for (;;) {
        IRON_MUTEX_LOCK(w->lock);
        while (w->count == 0 && !atomic_load(&w->shutdown)) {
            IRON_COND_WAIT(w->cv, w->lock);
        }
        if (w->count == 0 && atomic_load(&w->shutdown)) {
            IRON_MUTEX_UNLOCK(w->lock);
            break;
        }
        IronLsp_OutboundItem item = w->queue[w->head];
        w->queue[w->head].body = NULL;
        w->head = (w->head + 1) % ILSP_WRITER_QUEUE_CAPACITY;
        w->count--;
        IRON_MUTEX_UNLOCK(w->lock);

        writer_write_one(w, &item);
    }
    return NULL;
}

IronLsp_Writer *ilsp_writer_create(FILE *sink) {
    IronLsp_Writer *w = (IronLsp_Writer *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->sink    = sink;
    w->head    = 0;
    w->count   = 0;
    w->started = false;
    atomic_store(&w->shutdown, false);
    IRON_MUTEX_INIT(w->lock);
    IRON_COND_INIT(w->cv);
    return w;
}

void ilsp_writer_destroy(IronLsp_Writer *w) {
    if (!w) return;
    /* Free any un-drained bodies so tests that skip start/join don't leak. */
    for (int i = 0; i < w->count; i++) {
        int idx = (w->head + i) % ILSP_WRITER_QUEUE_CAPACITY;
        free(w->queue[idx].body);
    }
    IRON_MUTEX_DESTROY(w->lock);
    IRON_COND_DESTROY(w->cv);
    free(w);
}

void ilsp_writer_start(IronLsp_Writer *w) {
    if (!w || w->started) return;
    w->started = true;
    IRON_THREAD_CREATE(w->thread, writer_thread_main, w);
}

void ilsp_writer_shutdown(IronLsp_Writer *w) {
    if (!w) return;
    atomic_store(&w->shutdown, true);
    IRON_MUTEX_LOCK(w->lock);
    IRON_COND_BROADCAST(w->cv);
    IRON_MUTEX_UNLOCK(w->lock);
}

void ilsp_writer_join(IronLsp_Writer *w) {
    if (!w || !w->started) return;
    IRON_THREAD_JOIN(w->thread);
    w->started = false;
}

/* Find the index of the oldest item with the given priority. Returns -1
 * if none exists. Caller holds lock. "Oldest" = closest to head. */
static int find_oldest_of_priority(const IronLsp_Writer *w,
                                   IronLsp_Priority prio) {
    for (int i = 0; i < w->count; i++) {
        int idx = (w->head + i) % ILSP_WRITER_QUEUE_CAPACITY;
        if (w->queue[idx].prio == prio) return i;  /* logical offset */
    }
    return -1;
}

/* Remove the logical-offset item and shift subsequent items down by one.
 * Returns the freed logical slot (always w->count - 1 after shift).
 * Caller holds lock. Frees the removed item's body. */
static void remove_logical(IronLsp_Writer *w, int logical_offset) {
    int idx = (w->head + logical_offset) % ILSP_WRITER_QUEUE_CAPACITY;
    free(w->queue[idx].body);
    w->queue[idx].body = NULL;

    /* Shift each subsequent item one slot earlier. */
    for (int i = logical_offset; i < w->count - 1; i++) {
        int from = (w->head + i + 1) % ILSP_WRITER_QUEUE_CAPACITY;
        int to   = (w->head + i)     % ILSP_WRITER_QUEUE_CAPACITY;
        w->queue[to] = w->queue[from];
    }
    int last = (w->head + w->count - 1) % ILSP_WRITER_QUEUE_CAPACITY;
    w->queue[last].body = NULL;
    w->count--;
}

IronLsp_EnqueueResult ilsp_writer_enqueue(IronLsp_Writer *w,
                                          IronLsp_Priority prio,
                                          char *body, size_t len) {
    if (!w) {
        free(body);
        return ILSP_ENQUEUE_FULL_DROPPED;
    }

    IronLsp_EnqueueResult result = ILSP_ENQUEUE_OK;

    IRON_MUTEX_LOCK(w->lock);

    if (w->count >= ILSP_WRITER_QUEUE_CAPACITY) {
        /* Queue full -- apply drop policy. Try LOG first, then NOTIFICATION. */
        int idx = find_oldest_of_priority(w, ILSP_PRIO_LOG);
        if (idx >= 0) {
            remove_logical(w, idx);
            result = ILSP_ENQUEUE_OK_DROPPED_LOG;
        } else {
            idx = find_oldest_of_priority(w, ILSP_PRIO_NOTIFICATION);
            if (idx >= 0) {
                remove_logical(w, idx);
                result = ILSP_ENQUEUE_OK_DROPPED_NOTIFICATION;
            } else {
                /* All slots are responses: reject the new item. */
                IRON_MUTEX_UNLOCK(w->lock);
                free(body);
                return ILSP_ENQUEUE_FULL_DROPPED;
            }
        }
    }

    /* Insert at tail. */
    int tail = (w->head + w->count) % ILSP_WRITER_QUEUE_CAPACITY;
    w->queue[tail].prio = prio;
    w->queue[tail].body = body;
    w->queue[tail].len  = len;
    w->count++;

    IRON_COND_SIGNAL(w->cv);
    IRON_MUTEX_UNLOCK(w->lock);
    return result;
}

bool ilsp_writer_drain_one(IronLsp_Writer *w) {
    if (!w) return false;
    IRON_MUTEX_LOCK(w->lock);
    if (w->count == 0) {
        IRON_MUTEX_UNLOCK(w->lock);
        return false;
    }
    IronLsp_OutboundItem item = w->queue[w->head];
    w->queue[w->head].body = NULL;
    w->head = (w->head + 1) % ILSP_WRITER_QUEUE_CAPACITY;
    w->count--;
    IRON_MUTEX_UNLOCK(w->lock);

    writer_write_one(w, &item);
    return true;
}
