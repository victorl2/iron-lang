/* Phase 2 Plan 02 Task 03 -- Writer queue + drop policy.
 *
 * The queue is a circular array that starts at ILSP_WRITER_QUEUE_CAPACITY
 * slots and grows when needed. head/count form the ring invariants; all
 * state transitions happen under `lock`. Enqueuers signal `cv` after
 * inserting; the writer thread waits on `cv` while `count == 0 &&
 * !shutdown`.
 *
 * Policy when the queue holds ILSP_WRITER_QUEUE_CAPACITY items or more:
 *   1. Drop the oldest ILSP_PRIO_LOG (a window/logMessage can be lost)
 *      and return OK_DROPPED_LOG.
 *   2. Otherwise grow the array. Responses and notifications are never
 *      dropped: a request left without its response hangs the client,
 *      and a lost publishDiagnostics leaves stale diagnostics (they used
 *      to be dropped once 256 were queued, #589). A client that stops
 *      reading altogether is bounded by the RSS cap.
 *   3. FULL_DROPPED only when the array cannot grow (out of memory). */
#include "lsp/transport/writer.h"

#include <errno.h>
#include "util/pthread_compat.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/iron_runtime.h"   /* IRON_MUTEX_* / IRON_COND_* / IRON_THREAD_* */
#include "lsp/obs/log.h"            /* Plan 06: EPIPE logging */

struct IronLsp_Writer {
    FILE                 *sink;
    IronLsp_OutboundItem *queue;     /* ring of `cap` slots */
    int                   cap;
    int                   head;      /* index of oldest item */
    int                   count;     /* 0..cap */
    iron_mutex_t          lock;
    iron_cond_t           cv;
    iron_thread_t         thread;
    bool                  started;
    atomic_bool           shutdown;
};

/* Write one framed message to the sink. Caller holds NO lock.
 *
 * Plan 06 (CORE-19): every write-path syscall is checked for EPIPE /
 * short-write / ferror. When the downstream sink goes away (editor
 * crashed, stdout closed) we flip the shutdown flag and broadcast so
 * the writer main loop exits at its next iteration. SIGPIPE is ignored
 * process-wide in main.c, so write(2) returns -1 with errno == EPIPE
 * instead of terminating us. Returns true if the sink is still viable,
 * false if the writer should stop draining. */
static bool writer_write_one(IronLsp_Writer *w, IronLsp_OutboundItem *item) {
    bool sink_ok = true;
    errno = 0;
    int hdr_n = fprintf(w->sink, "Content-Length: %zu\r\n\r\n", item->len);
    if (hdr_n < 0 || ferror(w->sink) || errno == EPIPE) {
        sink_ok = false;
    }
    if (sink_ok && item->len > 0 && item->body != NULL) {
        errno = 0;
        size_t wn = fwrite(item->body, 1, item->len, w->sink);
        if (wn != item->len || ferror(w->sink) || errno == EPIPE) {
            sink_ok = false;
        }
    }
    if (sink_ok) {
        errno = 0;
        if (fflush(w->sink) != 0 || ferror(w->sink) || errno == EPIPE) {
            sink_ok = false;
        }
    }
    free(item->body);
    item->body = NULL;

    if (!sink_ok) {
        ilsp_log(ILSP_LOG_WARN, "writer-epipe",
                 "sink closed (errno=%d); initiating shutdown", errno);
        atomic_store(&w->shutdown, true);
        clearerr(w->sink);
        IRON_MUTEX_LOCK(w->lock);
        IRON_COND_BROADCAST(w->cv);
        IRON_MUTEX_UNLOCK(w->lock);
    }
    return sink_ok;
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
        w->head = (w->head + 1) % w->cap;
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
    w->cap     = ILSP_WRITER_QUEUE_CAPACITY;
    w->queue   = (IronLsp_OutboundItem *)calloc((size_t)w->cap, sizeof(*w->queue));
    if (!w->queue) { free(w); return NULL; }
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
        int idx = (w->head + i) % w->cap;
        free(w->queue[idx].body);
    }
    free(w->queue);
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
        int idx = (w->head + i) % w->cap;
        if (w->queue[idx].prio == prio) return i;  /* logical offset */
    }
    return -1;
}

/* Remove the logical-offset item and shift subsequent items down by one.
 * Returns the freed logical slot (always w->count - 1 after shift).
 * Caller holds lock. Frees the removed item's body. */
static void remove_logical(IronLsp_Writer *w, int logical_offset) {
    int idx = (w->head + logical_offset) % w->cap;
    free(w->queue[idx].body);
    w->queue[idx].body = NULL;

    /* Shift each subsequent item one slot earlier. */
    for (int i = logical_offset; i < w->count - 1; i++) {
        int from = (w->head + i + 1) % w->cap;
        int to   = (w->head + i)     % w->cap;
        w->queue[to] = w->queue[from];
    }
    int last = (w->head + w->count - 1) % w->cap;
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
        int idx = find_oldest_of_priority(w, ILSP_PRIO_LOG);
        if (idx >= 0) {
            remove_logical(w, idx);
            result = ILSP_ENQUEUE_OK_DROPPED_LOG;
        }
    }
    if (w->count >= w->cap) {
        /* Grow, unrolling the ring into the new array. */
        int ncap = w->cap * 2;
        IronLsp_OutboundItem *nq =
            (IronLsp_OutboundItem *)calloc((size_t)ncap, sizeof(*nq));
        if (!nq) {
            IRON_MUTEX_UNLOCK(w->lock);
            free(body);
            return ILSP_ENQUEUE_FULL_DROPPED;
        }
        for (int i = 0; i < w->count; i++) nq[i] = w->queue[(w->head + i) % w->cap];
        free(w->queue);
        w->queue = nq;
        w->cap = ncap;
        w->head = 0;
    }

    /* Insert at tail. */
    int tail = (w->head + w->count) % w->cap;
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
    w->head = (w->head + 1) % w->cap;
    w->count--;
    IRON_MUTEX_UNLOCK(w->lock);

    writer_write_one(w, &item);
    return true;
}
