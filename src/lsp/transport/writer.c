/* Writer queue: a ring buffer that grows on demand; all state transitions
 * happen under `lock`. Enqueuers signal `cv` after inserting; the writer
 * thread waits on `cv` while `count == 0 && !shutdown`.
 *
 * Policy (#589, see writer.h): responses are never dropped (JSON-RPC
 * answers every request), a publishDiagnostics supersedes a queued one for
 * the same document, and above ILSP_WRITER_QUEUE_CAPACITY items only logs
 * are shed. The removal shift is O(N); queues stay short in practice. */
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
    IronLsp_OutboundItem *queue;     /* ring of `cap` slots, grown on demand */
    int                   cap;
    int                   head;      /* index of oldest item */
    int                   count;
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
    IRON_MUTEX_DESTROY(w->lock);
    IRON_COND_DESTROY(w->cv);
    free(w->queue);
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

/* Double the ring (caller holds lock). Returns false when out of memory. */
static bool grow(IronLsp_Writer *w) {
    int nc = w->cap * 2;
    IronLsp_OutboundItem *q = (IronLsp_OutboundItem *)calloc((size_t)nc, sizeof(*q));
    if (!q) return false;
    for (int i = 0; i < w->count; i++) q[i] = w->queue[(w->head + i) % w->cap];
    free(w->queue);
    w->queue = q;
    w->cap = nc;
    w->head = 0;
    return true;
}

static const char *find_bytes(const char *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (!hay || m == 0 || m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, m) == 0) return hay + i;
    return NULL;
}

/* The URI of a textDocument/publishDiagnostics notification body, as a
 * pointer into it and a length, or NULL. */
static const char *diag_uri(const char *body, size_t len, size_t *ulen) {
    if (!find_bytes(body, len, "\"textDocument/publishDiagnostics\"")) return NULL;
    const char *u = find_bytes(body, len, "\"uri\"");
    if (!u) return NULL;
    const char *end_all = body + len;
    u += 5;
    while (u < end_all && (*u == ' ' || *u == ':' || *u == '\t')) u++;
    if (u >= end_all || *u != '"') return NULL;
    u++;
    const char *end = memchr(u, '"', (size_t)(end_all - u));
    if (!end) return NULL;
    *ulen = (size_t)(end - u);
    return u;
}

/* Remove a queued publishDiagnostics for the same document: the new one
 * replaces it (only the latest matters). Caller holds lock. */
static bool supersede_diagnostics(IronLsp_Writer *w, const char *body, size_t len) {
    size_t ulen = 0;
    const char *uri = diag_uri(body, len, &ulen);
    if (!uri) return false;
    for (int i = 0; i < w->count; i++) {
        IronLsp_OutboundItem *it = &w->queue[(w->head + i) % w->cap];
        if (it->prio != ILSP_PRIO_NOTIFICATION) continue;
        size_t ql = 0;
        const char *qu = diag_uri(it->body, it->len, &ql);
        if (qu && ql == ulen && memcmp(qu, uri, ulen) == 0) {
            remove_logical(w, i);
            return true;
        }
    }
    return false;
}

/* Policy (#589): responses are never dropped, since JSON-RPC answers every
 * request, and notifications are never lost: a publishDiagnostics replaces
 * a queued one for the same document, and the queue grows past
 * ILSP_WRITER_QUEUE_CAPACITY rather than drop either. Only log messages
 * are shed when it is that full: the oldest log goes first, and a new log
 * is refused when there is none to shed. */
IronLsp_EnqueueResult ilsp_writer_enqueue(IronLsp_Writer *w,
                                          IronLsp_Priority prio,
                                          char *body, size_t len) {
    if (!w) {
        free(body);
        return ILSP_ENQUEUE_FULL_DROPPED;
    }

    IronLsp_EnqueueResult result = ILSP_ENQUEUE_OK;

    IRON_MUTEX_LOCK(w->lock);

    if (prio == ILSP_PRIO_NOTIFICATION && supersede_diagnostics(w, body, len))
        result = ILSP_ENQUEUE_OK_DROPPED_NOTIFICATION;

    if (w->count >= ILSP_WRITER_QUEUE_CAPACITY) {
        int idx = find_oldest_of_priority(w, ILSP_PRIO_LOG);
        if (idx >= 0) {
            remove_logical(w, idx);
            if (result == ILSP_ENQUEUE_OK) result = ILSP_ENQUEUE_OK_DROPPED_LOG;
        } else if (prio == ILSP_PRIO_LOG) {
            IRON_MUTEX_UNLOCK(w->lock);
            free(body);
            return ILSP_ENQUEUE_FULL_DROPPED;
        }
    }
    if (w->count >= w->cap && !grow(w)) {
        IRON_MUTEX_UNLOCK(w->lock);
        free(body);
        return ILSP_ENQUEUE_FULL_DROPPED;
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
