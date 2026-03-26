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
#include <unistd.h>

/* ── Global pool instance ────────────────────────────────────────────────── */

Iron_Pool *Iron_global_pool = NULL;

/* ── Iron_Pool internals ─────────────────────────────────────────────────── */

typedef struct {
    void (*fn)(void *arg);
    void  *arg;
} Iron_WorkItem;

struct Iron_Pool {
    pthread_t       *threads;
    int              thread_count;
    Iron_WorkItem   *queue;      /* circular buffer */
    int              queue_head;
    int              queue_tail;
    int              queue_count;
    int              queue_capacity;
    pthread_mutex_t  lock;
    pthread_cond_t   work_ready;
    pthread_cond_t   work_done;
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
static void *pool_worker(void *arg) {
    Iron_Pool *pool = (Iron_Pool *)arg;

    for (;;) {
        pthread_mutex_lock(&pool->lock);

        /* Wait until there is work or the pool is shutting down */
        while (pool->queue_count == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->work_ready, &pool->lock);
        }

        if (pool->shutdown && pool->queue_count == 0) {
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }

        /* Dequeue one item */
        Iron_WorkItem item = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1) % pool->queue_capacity;
        pool->queue_count--;

        pthread_mutex_unlock(&pool->lock);

        /* Execute the work item */
        item.fn(item.arg);

        /* Decrement pending and signal barrier waiters if done */
        pthread_mutex_lock(&pool->lock);
        pool->pending--;
        if (pool->pending == 0) {
            pthread_cond_broadcast(&pool->work_done);
        }
        pthread_mutex_unlock(&pool->lock);
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

    pthread_mutex_init(&pool->lock,       NULL);
    pthread_cond_init(&pool->work_ready,  NULL);
    pthread_cond_init(&pool->work_done,   NULL);

    pool->threads = (pthread_t *)malloc(
        (size_t)thread_count * sizeof(pthread_t));
    if (!pool->threads) {
        free(pool->queue);
        free(pool);
        return NULL;
    }

    for (int i = 0; i < thread_count; i++) {
        pthread_create(&pool->threads[i], NULL, pool_worker, pool);
    }

    return pool;
}

void Iron_pool_submit(Iron_Pool *pool, void (*fn)(void *), void *arg) {
    if (!pool || !fn) return;

    pthread_mutex_lock(&pool->lock);

    if (pool->queue_count == pool->queue_capacity) {
        pool_queue_grow(pool);
    }

    pool->queue[pool->queue_tail].fn  = fn;
    pool->queue[pool->queue_tail].arg = arg;
    pool->queue_tail  = (pool->queue_tail + 1) % pool->queue_capacity;
    pool->queue_count++;
    pool->pending++;

    pthread_cond_signal(&pool->work_ready);
    pthread_mutex_unlock(&pool->lock);
}

void Iron_pool_barrier(Iron_Pool *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    while (pool->pending > 0) {
        pthread_cond_wait(&pool->work_done, &pool->lock);
    }
    pthread_mutex_unlock(&pool->lock);
}

int Iron_pool_thread_count(const Iron_Pool *pool) {
    return pool ? pool->thread_count : 0;
}

void Iron_pool_destroy(Iron_Pool *pool) {
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->work_ready);
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    free(pool->threads);
    free(pool->queue);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->work_ready);
    pthread_cond_destroy(&pool->work_done);
    free(pool);
}

/* ── Iron_Handle (spawn / await) ─────────────────────────────────────────── */

typedef struct {
    Iron_Handle *handle;
    void (*fn)(void *);
    void *arg;
} HandleWrapper;

static void *handle_thread_fn(void *arg) {
    HandleWrapper *w = (HandleWrapper *)arg;
    Iron_Handle *h   = w->handle;
    void (*fn)(void *) = w->fn;
    void *fn_arg       = w->arg;
    free(w);

    fn(fn_arg);

    pthread_mutex_lock(&h->lock);
    h->done = true;
    pthread_cond_signal(&h->cond);
    pthread_mutex_unlock(&h->lock);
    return NULL;
}

Iron_Handle *Iron_handle_create(void (*fn)(void *), void *arg) {
    Iron_Handle *h = (Iron_Handle *)malloc(sizeof(Iron_Handle));
    if (!h) return NULL;

    h->done      = false;
    h->result    = NULL;
    h->panic_msg = NULL;
    pthread_mutex_init(&h->lock, NULL);
    pthread_cond_init(&h->cond, NULL);

    HandleWrapper *w = (HandleWrapper *)malloc(sizeof(HandleWrapper));
    if (!w) {
        free(h);
        return NULL;
    }
    w->handle = h;
    w->fn     = fn;
    w->arg    = arg;

    pthread_create(&h->thread, NULL, handle_thread_fn, w);
    return h;
}

void Iron_handle_wait(Iron_Handle *handle) {
    if (!handle) return;

    pthread_mutex_lock(&handle->lock);
    while (!handle->done) {
        pthread_cond_wait(&handle->cond, &handle->lock);
    }
    pthread_mutex_unlock(&handle->lock);

    pthread_join(handle->thread, NULL);

    if (handle->panic_msg) {
        fprintf(stderr, "panic in spawned task: %s\n", handle->panic_msg);
        abort();
    }
}

void Iron_handle_destroy(Iron_Handle *handle) {
    if (!handle) return;
    pthread_mutex_destroy(&handle->lock);
    pthread_cond_destroy(&handle->cond);
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
    pthread_mutex_t lock;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
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

    pthread_mutex_init(&ch->lock,      NULL);
    pthread_cond_init(&ch->not_full,   NULL);
    pthread_cond_init(&ch->not_empty,  NULL);
    return ch;
}

void Iron_channel_send(Iron_Channel *ch, void *item) {
    if (!ch) return;
    pthread_mutex_lock(&ch->lock);
    while (ch->count == ch->capacity && !ch->closed) {
        pthread_cond_wait(&ch->not_full, &ch->lock);
    }
    if (ch->closed) {
        pthread_mutex_unlock(&ch->lock);
        return;
    }
    ch->ring[ch->tail] = item;
    ch->tail = (ch->tail + 1) % ch->capacity;
    ch->count++;
    pthread_cond_signal(&ch->not_empty);
    pthread_mutex_unlock(&ch->lock);
}

void *Iron_channel_recv(Iron_Channel *ch) {
    if (!ch) return NULL;
    pthread_mutex_lock(&ch->lock);
    while (ch->count == 0 && !ch->closed) {
        pthread_cond_wait(&ch->not_empty, &ch->lock);
    }
    if (ch->count == 0 && ch->closed) {
        pthread_mutex_unlock(&ch->lock);
        return NULL;
    }
    void *item   = ch->ring[ch->head];
    ch->head     = (ch->head + 1) % ch->capacity;
    ch->count--;
    pthread_cond_signal(&ch->not_full);
    pthread_mutex_unlock(&ch->lock);
    return item;
}

bool Iron_channel_try_recv(Iron_Channel *ch, void **out) {
    if (!ch || !out) return false;
    pthread_mutex_lock(&ch->lock);
    if (ch->count == 0) {
        pthread_mutex_unlock(&ch->lock);
        return false;
    }
    *out     = ch->ring[ch->head];
    ch->head = (ch->head + 1) % ch->capacity;
    ch->count--;
    pthread_cond_signal(&ch->not_full);
    pthread_mutex_unlock(&ch->lock);
    return true;
}

void Iron_channel_close(Iron_Channel *ch) {
    if (!ch) return;
    pthread_mutex_lock(&ch->lock);
    ch->closed = true;
    pthread_cond_broadcast(&ch->not_full);
    pthread_cond_broadcast(&ch->not_empty);
    pthread_mutex_unlock(&ch->lock);
}

void Iron_channel_destroy(Iron_Channel *ch) {
    if (!ch) return;
    free(ch->ring);
    pthread_mutex_destroy(&ch->lock);
    pthread_cond_destroy(&ch->not_full);
    pthread_cond_destroy(&ch->not_empty);
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
    pthread_mutex_init(&m->lock, NULL);
    return m;
}

void *Iron_mutex_lock(Iron_Mutex *m) {
    if (!m) return NULL;
    pthread_mutex_lock(&m->lock);
    return m->value;
}

void Iron_mutex_unlock(Iron_Mutex *m) {
    if (!m) return;
    pthread_mutex_unlock(&m->lock);
}

void Iron_mutex_destroy(Iron_Mutex *m) {
    if (!m) return;
    free(m->value);
    pthread_mutex_destroy(&m->lock);
    free(m);
}

/* ── Lock / CondVar raw primitives ───────────────────────────────────────── */

void Iron_lock_init(Iron_Lock *l)    { pthread_mutex_init(l, NULL); }
void Iron_lock_acquire(Iron_Lock *l) { pthread_mutex_lock(l); }
void Iron_lock_release(Iron_Lock *l) { pthread_mutex_unlock(l); }

void Iron_condvar_init(Iron_CondVar *cv)                       { pthread_cond_init(cv, NULL); }
void Iron_condvar_wait(Iron_CondVar *cv, Iron_Lock *l)         { pthread_cond_wait(cv, l); }
void Iron_condvar_signal(Iron_CondVar *cv)                     { pthread_cond_signal(cv); }
void Iron_condvar_broadcast(Iron_CondVar *cv)                  { pthread_cond_broadcast(cv); }

/* ── Global pool lifecycle (called from iron_runtime_init/shutdown) ───────── */

void iron_threads_init(void) {
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    int  thread_count = (ncpus > 1) ? (int)(ncpus - 1) : 1;
    Iron_global_pool = Iron_pool_create("global", thread_count);
}

void iron_threads_shutdown(void) {
    if (Iron_global_pool) {
        Iron_pool_destroy(Iron_global_pool);
        Iron_global_pool = NULL;
    }
}
