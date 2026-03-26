/* iron_threads.c — Threading primitives for the Iron runtime.
 *
 * Implements:
 *   Iron_Pool    — fixed-size thread pool with FIFO work queue
 *   Iron_Handle  — future for the spawn/await pattern
 *   Iron_Channel — bounded ring-buffer with blocking send/recv
 *   Iron_Mutex   — value-wrapping mutex
 *   Lock/CondVar — thin pthread wrappers
 *
 * All synchronization is via POSIX pthreads.
 */

#include "iron_runtime.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
#endif

/* ── Global pool instance ────────────────────────────────────────────────── */

Iron_Pool *Iron_global_pool = NULL;

/* ── Iron_Pool internals ─────────────────────────────────────────────────── */

typedef struct {
    void (*fn)(void *arg);
    void  *arg;
} Iron_WorkItem;

struct Iron_Pool {
    iron_thread_t   *threads;
    int              thread_count;
    Iron_WorkItem   *queue;      /* circular buffer */
    int              queue_head;
    int              queue_tail;
    int              queue_count;
    int              queue_capacity;
    iron_mutex_t     lock;
    iron_cond_t      work_ready;
    iron_cond_t      work_done;
    int              pending;
    bool             shutdown;
    const char      *name;
};

/* Expand the circular-buffer work queue to double its capacity.
 * Must be called with pool->lock held. */
static void pool_queue_grow(Iron_Pool *pool) {
    int new_cap = pool->queue_capacity * 2;
    Iron_WorkItem *new_q = (Iron_WorkItem *)malloc(
        (size_t)new_cap * sizeof(Iron_WorkItem));
    if (!new_q) return; /* silently drop expansion on OOM */

    /* Copy items from circular buffer to linear new buffer */
    for (int i = 0; i < pool->queue_count; i++) {
        new_q[i] = pool->queue[(pool->queue_head + i) % pool->queue_capacity];
    }
    free(pool->queue);
    pool->queue          = new_q;
    pool->queue_head     = 0;
    pool->queue_tail     = pool->queue_count;
    pool->queue_capacity = new_cap;
}

/* Worker thread entry point */
#ifdef _WIN32
static int pool_worker(void *arg) {
#else
static void *pool_worker(void *arg) {
#endif
    Iron_Pool *pool = (Iron_Pool *)arg;

    for (;;) {
        IRON_MUTEX_LOCK(pool->lock);

        /* Wait until there is work or the pool is shutting down */
        while (pool->queue_count == 0 && !pool->shutdown) {
            IRON_COND_WAIT(pool->work_ready, pool->lock);
        }

        if (pool->shutdown && pool->queue_count == 0) {
            IRON_MUTEX_UNLOCK(pool->lock);
#ifdef _WIN32
            return 0;
#else
            return NULL;
#endif
        }

        /* Dequeue one item */
        Iron_WorkItem item = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1) % pool->queue_capacity;
        pool->queue_count--;

        IRON_MUTEX_UNLOCK(pool->lock);

        /* Execute the work item */
        item.fn(item.arg);

        /* Decrement pending and signal barrier waiters if done */
        IRON_MUTEX_LOCK(pool->lock);
        pool->pending--;
        if (pool->pending == 0) {
            IRON_COND_BROADCAST(pool->work_done);
        }
        IRON_MUTEX_UNLOCK(pool->lock);
    }
}

/* ── Iron_Pool public API ────────────────────────────────────────────────── */

Iron_Pool *Iron_pool_create(const char *name, int thread_count) {
    if (thread_count < 1) thread_count = 1;

    Iron_Pool *pool = (Iron_Pool *)malloc(sizeof(Iron_Pool));
    if (!pool) return NULL;

    pool->name           = name;
    pool->thread_count   = thread_count;
    pool->queue_capacity = 256;
    pool->queue_head     = 0;
    pool->queue_tail     = 0;
    pool->queue_count    = 0;
    pool->pending        = 0;
    pool->shutdown       = false;

    pool->queue = (Iron_WorkItem *)malloc(
        (size_t)pool->queue_capacity * sizeof(Iron_WorkItem));
    if (!pool->queue) {
        free(pool);
        return NULL;
    }

    IRON_MUTEX_INIT(pool->lock);
    IRON_COND_INIT(pool->work_ready);
    IRON_COND_INIT(pool->work_done);

    pool->threads = (iron_thread_t *)malloc(
        (size_t)thread_count * sizeof(iron_thread_t));
    if (!pool->threads) {
        free(pool->queue);
        free(pool);
        return NULL;
    }

    for (int i = 0; i < thread_count; i++) {
        IRON_THREAD_CREATE(pool->threads[i], pool_worker, pool);
    }

    return pool;
}

void Iron_pool_submit(Iron_Pool *pool, void (*fn)(void *), void *arg) {
    if (!pool || !fn) return;

    IRON_MUTEX_LOCK(pool->lock);

    if (pool->queue_count == pool->queue_capacity) {
        pool_queue_grow(pool);
    }

    pool->queue[pool->queue_tail].fn  = fn;
    pool->queue[pool->queue_tail].arg = arg;
    pool->queue_tail  = (pool->queue_tail + 1) % pool->queue_capacity;
    pool->queue_count++;
    pool->pending++;

    IRON_COND_SIGNAL(pool->work_ready);
    IRON_MUTEX_UNLOCK(pool->lock);
}

void Iron_pool_barrier(Iron_Pool *pool) {
    if (!pool) return;
    IRON_MUTEX_LOCK(pool->lock);
    while (pool->pending > 0) {
        IRON_COND_WAIT(pool->work_done, pool->lock);
    }
    IRON_MUTEX_UNLOCK(pool->lock);
}

int Iron_pool_thread_count(const Iron_Pool *pool) {
    return pool ? pool->thread_count : 0;
}

void Iron_pool_destroy(Iron_Pool *pool) {
    if (!pool) return;

    IRON_MUTEX_LOCK(pool->lock);
    pool->shutdown = true;
    IRON_COND_BROADCAST(pool->work_ready);
    IRON_MUTEX_UNLOCK(pool->lock);

    for (int i = 0; i < pool->thread_count; i++) {
        IRON_THREAD_JOIN(pool->threads[i]);
    }

    free(pool->threads);
    free(pool->queue);
    IRON_MUTEX_DESTROY(pool->lock);
    IRON_COND_DESTROY(pool->work_ready);
    IRON_COND_DESTROY(pool->work_done);
    free(pool);
}

/* ── Iron_Handle (spawn / await) ─────────────────────────────────────────── */

typedef struct {
    Iron_Handle *handle;
    void (*fn)(void *);
    void *arg;
} HandleWrapper;

#ifdef _WIN32
static int handle_thread_fn(void *arg) {
#else
static void *handle_thread_fn(void *arg) {
#endif
    HandleWrapper *w = (HandleWrapper *)arg;
    Iron_Handle *h   = w->handle;
    void (*fn)(void *) = w->fn;
    void *fn_arg       = w->arg;
    free(w);

    fn(fn_arg);

    IRON_MUTEX_LOCK(h->lock);
    h->done = true;
    IRON_COND_SIGNAL(h->cond);
    IRON_MUTEX_UNLOCK(h->lock);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

Iron_Handle *Iron_handle_create(void (*fn)(void *), void *arg) {
    Iron_Handle *h = (Iron_Handle *)malloc(sizeof(Iron_Handle));
    if (!h) return NULL;

    h->done      = false;
    h->result    = NULL;
    h->panic_msg = NULL;
    IRON_MUTEX_INIT(h->lock);
    IRON_COND_INIT(h->cond);

    HandleWrapper *w = (HandleWrapper *)malloc(sizeof(HandleWrapper));
    if (!w) {
        free(h);
        return NULL;
    }
    w->handle = h;
    w->fn     = fn;
    w->arg    = arg;

    IRON_THREAD_CREATE(h->thread, handle_thread_fn, w);
    return h;
}

void Iron_handle_wait(Iron_Handle *handle) {
    if (!handle) return;

    IRON_MUTEX_LOCK(handle->lock);
    while (!handle->done) {
        IRON_COND_WAIT(handle->cond, handle->lock);
    }
    IRON_MUTEX_UNLOCK(handle->lock);

    IRON_THREAD_JOIN(handle->thread);

    if (handle->panic_msg) {
        fprintf(stderr, "panic in spawned task: %s\n", handle->panic_msg);
        abort();
    }
}

void Iron_handle_destroy(Iron_Handle *handle) {
    if (!handle) return;
    IRON_MUTEX_DESTROY(handle->lock);
    IRON_COND_DESTROY(handle->cond);
    free(handle->panic_msg);
    free(handle);
}

/* ── Iron_Channel ────────────────────────────────────────────────────────── */

struct Iron_Channel {
    void          **ring;
    int             capacity;
    int             head;
    int             tail;
    int             count;
    iron_mutex_t    lock;
    iron_cond_t     not_full;
    iron_cond_t     not_empty;
    bool            closed;
};

Iron_Channel *Iron_channel_create(int capacity) {
    if (capacity < 1) capacity = 1; /* unbuffered = capacity 1 */
    Iron_Channel *ch = (Iron_Channel *)malloc(sizeof(Iron_Channel));
    if (!ch) return NULL;

    ch->ring     = (void **)malloc((size_t)capacity * sizeof(void *));
    if (!ch->ring) {
        free(ch);
        return NULL;
    }
    ch->capacity = capacity;
    ch->head     = 0;
    ch->tail     = 0;
    ch->count    = 0;
    ch->closed   = false;

    IRON_MUTEX_INIT(ch->lock);
    IRON_COND_INIT(ch->not_full);
    IRON_COND_INIT(ch->not_empty);
    return ch;
}

void Iron_channel_send(Iron_Channel *ch, void *item) {
    if (!ch) return;
    IRON_MUTEX_LOCK(ch->lock);
    while (ch->count == ch->capacity && !ch->closed) {
        IRON_COND_WAIT(ch->not_full, ch->lock);
    }
    if (ch->closed) {
        IRON_MUTEX_UNLOCK(ch->lock);
        return;
    }
    ch->ring[ch->tail] = item;
    ch->tail = (ch->tail + 1) % ch->capacity;
    ch->count++;
    IRON_COND_SIGNAL(ch->not_empty);
    IRON_MUTEX_UNLOCK(ch->lock);
}

void *Iron_channel_recv(Iron_Channel *ch) {
    if (!ch) return NULL;
    IRON_MUTEX_LOCK(ch->lock);
    while (ch->count == 0 && !ch->closed) {
        IRON_COND_WAIT(ch->not_empty, ch->lock);
    }
    if (ch->count == 0 && ch->closed) {
        IRON_MUTEX_UNLOCK(ch->lock);
        return NULL;
    }
    void *item   = ch->ring[ch->head];
    ch->head     = (ch->head + 1) % ch->capacity;
    ch->count--;
    IRON_COND_SIGNAL(ch->not_full);
    IRON_MUTEX_UNLOCK(ch->lock);
    return item;
}

bool Iron_channel_try_recv(Iron_Channel *ch, void **out) {
    if (!ch || !out) return false;
    IRON_MUTEX_LOCK(ch->lock);
    if (ch->count == 0) {
        IRON_MUTEX_UNLOCK(ch->lock);
        return false;
    }
    *out     = ch->ring[ch->head];
    ch->head = (ch->head + 1) % ch->capacity;
    ch->count--;
    IRON_COND_SIGNAL(ch->not_full);
    IRON_MUTEX_UNLOCK(ch->lock);
    return true;
}

void Iron_channel_close(Iron_Channel *ch) {
    if (!ch) return;
    IRON_MUTEX_LOCK(ch->lock);
    ch->closed = true;
    IRON_COND_BROADCAST(ch->not_full);
    IRON_COND_BROADCAST(ch->not_empty);
    IRON_MUTEX_UNLOCK(ch->lock);
}

void Iron_channel_destroy(Iron_Channel *ch) {
    if (!ch) return;
    free(ch->ring);
    IRON_MUTEX_DESTROY(ch->lock);
    IRON_COND_DESTROY(ch->not_full);
    IRON_COND_DESTROY(ch->not_empty);
    free(ch);
}

/* ── Iron_Mutex ──────────────────────────────────────────────────────────── */

Iron_Mutex *Iron_mutex_create(void *initial_value, size_t size) {
    Iron_Mutex *m = (Iron_Mutex *)malloc(sizeof(Iron_Mutex));
    if (!m) return NULL;

    m->value = malloc(size);
    if (!m->value) {
        free(m);
        return NULL;
    }
    m->value_size = size;
    if (initial_value && size > 0) {
        memcpy(m->value, initial_value, size);
    }
    IRON_MUTEX_INIT(m->lock);
    return m;
}

void *Iron_mutex_lock(Iron_Mutex *m) {
    if (!m) return NULL;
    IRON_MUTEX_LOCK(m->lock);
    return m->value;
}

void Iron_mutex_unlock(Iron_Mutex *m) {
    if (!m) return;
    IRON_MUTEX_UNLOCK(m->lock);
}

void Iron_mutex_destroy(Iron_Mutex *m) {
    if (!m) return;
    free(m->value);
    IRON_MUTEX_DESTROY(m->lock);
    free(m);
}

/* ── Lock / CondVar raw primitives ───────────────────────────────────────── */

void Iron_lock_init(Iron_Lock *l)    { IRON_MUTEX_INIT(*l); }
void Iron_lock_acquire(Iron_Lock *l) { IRON_MUTEX_LOCK(*l); }
void Iron_lock_release(Iron_Lock *l) { IRON_MUTEX_UNLOCK(*l); }

void Iron_condvar_init(Iron_CondVar *cv)                       { IRON_COND_INIT(*cv); }
void Iron_condvar_wait(Iron_CondVar *cv, Iron_Lock *l)         { IRON_COND_WAIT(*cv, *l); }
void Iron_condvar_signal(Iron_CondVar *cv)                     { IRON_COND_SIGNAL(*cv); }
void Iron_condvar_broadcast(Iron_CondVar *cv)                  { IRON_COND_BROADCAST(*cv); }

/* ── Global pool lifecycle (called from iron_runtime_init/shutdown) ───────── */

void iron_threads_init(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int cpu_count = (int)si.dwNumberOfProcessors;
#else
    int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    int thread_count = (cpu_count > 1) ? (cpu_count - 1) : 1;
    Iron_global_pool = Iron_pool_create("global", thread_count);
}

void iron_threads_shutdown(void) {
    if (Iron_global_pool) {
        Iron_pool_destroy(Iron_global_pool);
        Iron_global_pool = NULL;
    }
}
