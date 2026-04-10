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
  #include <time.h>
  #include <errno.h>
#endif

/* ── Iron_monotonic_now_ms (INFRA-09 foundation) ─────────────────────────── */

#ifdef _WIN32
uint64_t Iron_monotonic_now_ms(void) {
    /* GetTickCount64 is monotonic (never goes backwards, unaffected by
     * wall-clock adjustments) and returns milliseconds since system boot. */
    return (uint64_t)GetTickCount64();
}
#else
uint64_t Iron_monotonic_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif

/* ── iron_cond_timedwait_ms — bounded condvar wait ────────────────────────── */

#ifdef _WIN32
int iron_cond_timedwait_ms(iron_cond_t *cv, iron_mutex_t *lock, int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = 0;
    BOOL ok = SleepConditionVariableCS(cv, lock, (DWORD)timeout_ms);
    if (ok) return IRON_TIMEDWAIT_OK;
    if (GetLastError() == ERROR_TIMEOUT) return IRON_TIMEDWAIT_EXPIRED;
    return IRON_TIMEDWAIT_ERROR;
}
#else
int iron_cond_timedwait_ms(iron_cond_t *cv, iron_mutex_t *lock, int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = 0;
    /* POSIX pthread_cond_timedwait uses CLOCK_REALTIME by default. This may
     * jump during NTP slew; Phase 59 accepts the caveat (TLS phase can
     * upgrade to CLOCK_MONOTONIC via pthread_condattr_setclock). */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec  += 1;
        ts.tv_nsec -= 1000000000L;
    }
    int rc = pthread_cond_timedwait(cv, lock, &ts);
    if (rc == 0)          return IRON_TIMEDWAIT_OK;
    if (rc == ETIMEDOUT)  return IRON_TIMEDWAIT_EXPIRED;
    return IRON_TIMEDWAIT_ERROR;
}
#endif

/* ── Global pool instances ───────────────────────────────────────────────── */

Iron_Pool *Iron_global_pool = NULL;
Iron_Pool *Iron_io_pool     = NULL;

/* ── Iron_Pool internals ─────────────────────────────────────────────────── */

typedef struct {
    void          (*fn)(void *arg);
    void           *arg;
    Iron_PoolWait  *wait;  /* optional — Iron_pool_submit_wait binds this */
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

    /* ── Phase 59 P01b: elastic mode ─────────────────────────────────────
     * When is_elastic is true, workers are spawned on demand (0..max_threads)
     * and self-retire after idle_timeout_ms of no work. idle_threads counts
     * workers currently blocked in iron_cond_timedwait_ms (used to decide
     * whether a submit needs to spawn a new worker). thread_slots_cap is the
     * total capacity of the threads[] array; slots cleared to {0} when a
     * worker retires or leaks so replacements can reuse the slot.
     */
    bool             is_elastic;
    int              max_threads;
    int              idle_threads;
    int              idle_timeout_ms;
    int              thread_slots_cap;
    int              leaked_count;
};

/* ── Iron_PoolWait internals (Phase 59 P01b) ─────────────────────────────── */

struct Iron_PoolWait {
    iron_mutex_t  lock;
    iron_cond_t   cond;
    bool          completed;
    bool          abandoned;
    void         *result;
    void        (*result_destructor)(void*);
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

/* Worker thread entry point (fixed-size pools) */
static void *pool_worker(void *arg) {
    Iron_Pool *pool = (Iron_Pool *)arg;

    for (;;) {
        IRON_MUTEX_LOCK(pool->lock);

        /* Wait until there is work or the pool is shutting down */
        while (pool->queue_count == 0 && !pool->shutdown) {
            IRON_COND_WAIT(pool->work_ready, pool->lock);
        }

        if (pool->shutdown && pool->queue_count == 0) {
            IRON_MUTEX_UNLOCK(pool->lock);
            return NULL;
        }

        /* Dequeue one item */
        Iron_WorkItem item = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1) % pool->queue_capacity;
        pool->queue_count--;

        IRON_MUTEX_UNLOCK(pool->lock);

        /* Execute the work item */
        item.fn(item.arg);
        if (item.wait) {
            /* Treat ordinary submit on fixed pool as "worker finished the
             * work item" from the wait's perspective. NULL result since
             * the fn signature doesn't produce one. */
            Iron_poolwait_worker_finish(item.wait, NULL, NULL);
        }

        /* Decrement pending and signal barrier waiters if done */
        IRON_MUTEX_LOCK(pool->lock);
        pool->pending--;
        if (pool->pending == 0) {
            IRON_COND_BROADCAST(pool->work_done);
        }
        IRON_MUTEX_UNLOCK(pool->lock);
    }
}

/* Forward decl: elastic worker + spawn helper (defined below). */
static void *pool_worker_elastic(void *arg);
static void pool_spawn_elastic_worker_locked(Iron_Pool *pool);

/* ── Iron_Pool public API ────────────────────────────────────────────────── */

Iron_Pool *Iron_pool_create(const char *name, int thread_count) {
    if (thread_count < 1) thread_count = 1;

    Iron_Pool *pool = (Iron_Pool *)malloc(sizeof(Iron_Pool));
    if (!pool) return NULL;

    pool->name             = name;
    pool->thread_count     = thread_count;
    pool->queue_capacity   = 256;
    pool->queue_head       = 0;
    pool->queue_tail       = 0;
    pool->queue_count      = 0;
    pool->pending          = 0;
    pool->shutdown         = false;
    pool->is_elastic       = false;
    pool->max_threads      = thread_count;
    pool->idle_threads     = 0;
    pool->idle_timeout_ms  = 0;
    pool->thread_slots_cap = thread_count;
    pool->leaked_count     = 0;

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

    pool->queue[pool->queue_tail].fn   = fn;
    pool->queue[pool->queue_tail].arg  = arg;
    pool->queue[pool->queue_tail].wait = NULL;
    pool->queue_tail  = (pool->queue_tail + 1) % pool->queue_capacity;
    pool->queue_count++;
    pool->pending++;

    /* Elastic pools spawn a new worker on demand: if no worker is currently
     * parked in the timed wait AND we haven't hit max_threads, spawn now.
     * This ensures the submit never stalls on an empty pool. */
    if (pool->is_elastic
        && pool->idle_threads == 0
        && pool->thread_count < pool->max_threads) {
        pool_spawn_elastic_worker_locked(pool);
    }

    IRON_COND_SIGNAL(pool->work_ready);
    IRON_MUTEX_UNLOCK(pool->lock);
}

void Iron_pool_submit_wait(Iron_Pool *pool,
                           void (*fn)(void *),
                           void *arg,
                           Iron_PoolWait *wait) {
    if (!pool || !fn) return;

    IRON_MUTEX_LOCK(pool->lock);

    if (pool->queue_count == pool->queue_capacity) {
        pool_queue_grow(pool);
    }

    pool->queue[pool->queue_tail].fn   = fn;
    pool->queue[pool->queue_tail].arg  = arg;
    pool->queue[pool->queue_tail].wait = wait;
    pool->queue_tail  = (pool->queue_tail + 1) % pool->queue_capacity;
    pool->queue_count++;
    pool->pending++;

    if (pool->is_elastic
        && pool->idle_threads == 0
        && pool->thread_count < pool->max_threads) {
        pool_spawn_elastic_worker_locked(pool);
    }

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

    if (pool->is_elastic) {
        /* Elastic pools may have sparse threads[] because workers retire
         * themselves on idle timeout and NULL out their slot. Snapshot the
         * live threads under the lock, then join them. New workers cannot
         * spawn because shutdown is already set and Iron_pool_submit is
         * the only spawn site (user must not submit during destroy). */
        iron_thread_t *snapshot = NULL;
        int snapshot_count = 0;

        IRON_MUTEX_LOCK(pool->lock);
        snapshot = (iron_thread_t *)malloc(
            (size_t)pool->thread_slots_cap * sizeof(iron_thread_t));
        if (snapshot) {
            for (int i = 0; i < pool->thread_slots_cap; i++) {
                iron_thread_t zero;
                memset(&zero, 0, sizeof(zero));
                if (memcmp(&pool->threads[i], &zero, sizeof(zero)) != 0) {
                    snapshot[snapshot_count++] = pool->threads[i];
                    memset(&pool->threads[i], 0, sizeof(iron_thread_t));
                }
            }
        }
        IRON_MUTEX_UNLOCK(pool->lock);

        for (int i = 0; i < snapshot_count; i++) {
            IRON_THREAD_JOIN(snapshot[i]);
        }
        free(snapshot);
    } else {
        for (int i = 0; i < pool->thread_count; i++) {
            IRON_THREAD_JOIN(pool->threads[i]);
        }
    }

    free(pool->threads);
    free(pool->queue);
    IRON_MUTEX_DESTROY(pool->lock);
    IRON_COND_DESTROY(pool->work_ready);
    IRON_COND_DESTROY(pool->work_done);
    free(pool);
}

/* ── Iron_Pool read accessors (Phase 59 P01b) ────────────────────────────── */

bool Iron_pool_is_elastic(const Iron_Pool *p) {
    return p ? p->is_elastic : false;
}

int Iron_pool_max_threads(const Iron_Pool *p) {
    return p ? p->max_threads : 0;
}

int Iron_pool_live_thread_count(const Iron_Pool *p) {
    if (!p) return 0;
    /* Cast away const so we can lock — the read is conceptually const. */
    Iron_Pool *mp = (Iron_Pool *)p;
    IRON_MUTEX_LOCK(mp->lock);
    int n = mp->thread_count;
    IRON_MUTEX_UNLOCK(mp->lock);
    return n;
}

int Iron_pool_leaked_count(const Iron_Pool *p) {
    if (!p) return 0;
    Iron_Pool *mp = (Iron_Pool *)p;
    IRON_MUTEX_LOCK(mp->lock);
    int n = mp->leaked_count;
    IRON_MUTEX_UNLOCK(mp->lock);
    return n;
}

/* ── Elastic Iron_Pool impl (Phase 59 P01b) ──────────────────────────────── */

/* Spawn one elastic worker into the first NULL slot of pool->threads[].
 * Caller must hold pool->lock. Increments pool->thread_count on success. */
static void pool_spawn_elastic_worker_locked(Iron_Pool *pool) {
    int slot = -1;
    iron_thread_t zero;
    memset(&zero, 0, sizeof(zero));
    for (int i = 0; i < pool->thread_slots_cap; i++) {
        if (memcmp(&pool->threads[i], &zero, sizeof(zero)) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return;  /* no free slot — elastic math guarantees one */

    /* Reserve the slot so a racing submit doesn't double-spawn into it. */
    pool->thread_count++;
    /* We're still holding the lock; IRON_THREAD_CREATE is cheap and safe
     * to call under the lock on all supported platforms. */
    IRON_THREAD_CREATE(pool->threads[slot], pool_worker_elastic, pool);
}

/* Elastic worker entry point — waits for work with a bounded idle timeout
 * and self-retires on IRON_TIMEDWAIT_EXPIRED with an empty queue. */
static void *pool_worker_elastic(void *arg) {
    Iron_Pool *pool = (Iron_Pool *)arg;

    for (;;) {
        IRON_MUTEX_LOCK(pool->lock);

        /* Park on work_ready with the idle timeout until work arrives,
         * shutdown is set, or the deadline expires. */
        bool expired = false;
        while (pool->queue_count == 0 && !pool->shutdown) {
            pool->idle_threads++;
            int rc = iron_cond_timedwait_ms(&pool->work_ready,
                                            &pool->lock,
                                            pool->idle_timeout_ms);
            pool->idle_threads--;
            if (rc == IRON_TIMEDWAIT_EXPIRED) {
                expired = true;
                break;
            }
            if (rc == IRON_TIMEDWAIT_ERROR) {
                /* Treat error like expired — retire the worker. */
                expired = true;
                break;
            }
            /* OK: spurious wake or a signal landed — loop to recheck. */
        }

        if (pool->shutdown && pool->queue_count == 0) {
            /* Clear our slot so destroy's snapshot doesn't see a stale
             * thread handle (we're about to return). */
            iron_thread_t self_check;
            memset(&self_check, 0, sizeof(self_check));
            /* We don't know our own slot index cheaply — destroy joins on
             * snapshot + zero, and it runs with shutdown already set, so
             * leaving the slot as-is is fine: destroy will snapshot and
             * join us, then zero the slot. */
            (void)self_check;
            pool->thread_count--;
            IRON_MUTEX_UNLOCK(pool->lock);
            return NULL;
        }

        if (expired && pool->queue_count == 0) {
            /* Self-retire: NULL our slot and decrement thread_count so the
             * next submit can spawn a replacement without waiting. */
            iron_thread_t self = {0};
#ifndef _WIN32
            self = pthread_self();
#endif
            for (int i = 0; i < pool->thread_slots_cap; i++) {
#ifdef _WIN32
                /* Windows: compare HANDLE. GetCurrentThread() returns a
                 * pseudo-handle not equal to the real handle stored in the
                 * slot, so we match by the first non-zero slot and rely on
                 * only one worker retiring at a time — acceptable since
                 * destroy is the only other slot-visitor and it runs with
                 * shutdown set. */
                iron_thread_t zero;
                memset(&zero, 0, sizeof(zero));
                if (memcmp(&pool->threads[i], &zero, sizeof(zero)) != 0) {
                    memset(&pool->threads[i], 0, sizeof(iron_thread_t));
                    break;
                }
#else
                if (pthread_equal(pool->threads[i], self)) {
                    memset(&pool->threads[i], 0, sizeof(iron_thread_t));
                    /* Detach so we don't leak the joinable state — destroy
                     * will never see this slot again. */
                    pthread_detach(self);
                    break;
                }
#endif
            }
            pool->thread_count--;
            IRON_MUTEX_UNLOCK(pool->lock);
            return NULL;
        }

        /* Dequeue one item */
        Iron_WorkItem item = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1) % pool->queue_capacity;
        pool->queue_count--;

        IRON_MUTEX_UNLOCK(pool->lock);

        /* Execute the work item */
        item.fn(item.arg);
        if (item.wait) {
            Iron_poolwait_worker_finish(item.wait, NULL, NULL);
        }

        /* Decrement pending and signal barrier waiters if done */
        IRON_MUTEX_LOCK(pool->lock);
        pool->pending--;
        if (pool->pending == 0) {
            IRON_COND_BROADCAST(pool->work_done);
        }
        IRON_MUTEX_UNLOCK(pool->lock);
    }
}

Iron_Pool *Iron_elastic_pool_create(const char *name,
                                    int max_threads,
                                    int idle_timeout_ms) {
    if (max_threads < 1) max_threads = 1;
    if (idle_timeout_ms < 1) idle_timeout_ms = 1;

    Iron_Pool *pool = (Iron_Pool *)malloc(sizeof(Iron_Pool));
    if (!pool) return NULL;

    pool->name             = name;
    pool->thread_count     = 0;
    pool->queue_capacity   = 256;
    pool->queue_head       = 0;
    pool->queue_tail       = 0;
    pool->queue_count      = 0;
    pool->pending          = 0;
    pool->shutdown         = false;
    pool->is_elastic       = true;
    pool->max_threads      = max_threads;
    pool->idle_threads     = 0;
    pool->idle_timeout_ms  = idle_timeout_ms;
    pool->thread_slots_cap = max_threads;
    pool->leaked_count     = 0;

    pool->queue = (Iron_WorkItem *)malloc(
        (size_t)pool->queue_capacity * sizeof(Iron_WorkItem));
    if (!pool->queue) {
        free(pool);
        return NULL;
    }

    IRON_MUTEX_INIT(pool->lock);
    IRON_COND_INIT(pool->work_ready);
    IRON_COND_INIT(pool->work_done);

    /* Pre-allocate the slots array, zeroed — each slot is "empty" until a
     * worker is spawned into it. */
    pool->threads = (iron_thread_t *)calloc(
        (size_t)pool->thread_slots_cap, sizeof(iron_thread_t));
    if (!pool->threads) {
        IRON_MUTEX_DESTROY(pool->lock);
        IRON_COND_DESTROY(pool->work_ready);
        IRON_COND_DESTROY(pool->work_done);
        free(pool->queue);
        free(pool);
        return NULL;
    }

    return pool;
}

void Iron_pool_mark_one_leaked(Iron_Pool *pool) {
    if (!pool) return;
    IRON_MUTEX_LOCK(pool->lock);
    pool->leaked_count++;
    if (pool->pending > 0) pool->pending--;
    /* Decrement thread_count so the next submit can spawn a replacement.
     * The leaked worker is still alive OS-wise but will retire through its
     * own idle path; we accept temporary slot occupancy. */
    if (pool->thread_count > 0) pool->thread_count--;
    /* If pending just hit zero, wake up barrier waiters so they don't
     * block forever on a leaked worker's ghost pending count. */
    if (pool->pending == 0) {
        IRON_COND_BROADCAST(pool->work_done);
    }
    IRON_MUTEX_UNLOCK(pool->lock);
}

/* ── Iron_PoolWait impl (Phase 59 P01b) ──────────────────────────────────── */

Iron_PoolWait *Iron_poolwait_create(void) {
    Iron_PoolWait *w = (Iron_PoolWait *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    IRON_MUTEX_INIT(w->lock);
    IRON_COND_INIT(w->cond);
    w->completed         = false;
    w->abandoned         = false;
    w->result            = NULL;
    w->result_destructor = NULL;
    return w;
}

void Iron_poolwait_destroy(Iron_PoolWait *w) {
    if (!w) return;
    IRON_MUTEX_DESTROY(w->lock);
    IRON_COND_DESTROY(w->cond);
    free(w);
}

bool Iron_poolwait_completed(Iron_PoolWait *w) {
    if (!w) return true;
    IRON_MUTEX_LOCK(w->lock);
    bool c = w->completed;
    IRON_MUTEX_UNLOCK(w->lock);
    return c;
}

int Iron_poolwait_wait_ms(Iron_PoolWait *w, int timeout_ms) {
    if (!w) return 1;
    IRON_MUTEX_LOCK(w->lock);
    int rc = 1;
    if (!w->completed) {
        int wrc = iron_cond_timedwait_ms(&w->cond, &w->lock, timeout_ms);
        if (wrc == IRON_TIMEDWAIT_OK) {
            rc = w->completed ? 1 : 0;
        } else if (wrc == IRON_TIMEDWAIT_EXPIRED) {
            rc = w->completed ? 1 : 0;
        } else {
            rc = -1;
        }
    }
    IRON_MUTEX_UNLOCK(w->lock);
    return rc;
}

void Iron_poolwait_set_abandoned(Iron_PoolWait *w) {
    if (!w) return;
    IRON_MUTEX_LOCK(w->lock);
    w->abandoned = true;
    IRON_MUTEX_UNLOCK(w->lock);
}

void Iron_poolwait_worker_finish(Iron_PoolWait *w,
                                 void *result,
                                 void (*dtor)(void*)) {
    if (!w) return;
    IRON_MUTEX_LOCK(w->lock);
    bool aban = w->abandoned;
    w->completed = true;
    if (aban) {
        /* Caller abandoned — worker owns the result and must destroy it.
         * The wait struct is still caller-owned in the abandoned-flag
         * variant, so we do NOT free w here. */
        IRON_MUTEX_UNLOCK(w->lock);
        if (result && dtor) dtor(result);
        return;
    }
    w->result            = result;
    w->result_destructor = dtor;
    IRON_COND_SIGNAL(w->cond);
    IRON_MUTEX_UNLOCK(w->lock);
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

    IRON_MUTEX_LOCK(h->lock);
    h->done = true;
    IRON_COND_SIGNAL(h->cond);
    IRON_MUTEX_UNLOCK(h->lock);
    return NULL;
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

void *iron_future_await(Iron_Handle *handle) {
    if (!handle) return NULL;
    Iron_handle_wait(handle);       /* blocks until done, joins thread, re-raises panic */
    void *result = handle->result;  /* capture before destroy */
    Iron_handle_destroy(handle);    /* free mutex/cond/handle */
    return result;
}

Iron_Handle *iron_handle_create_self_ref(void (*fn)(void *)) {
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
    w->arg    = h;  /* self-referential: pass handle as arg */

    IRON_THREAD_CREATE(h->thread, handle_thread_fn, w);
    return h;
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
    /* Elastic I/O pool for blocking syscalls (DNS, TLS handshake, etc.).
     * Spawns on demand up to 64 workers; each worker self-retires after
     * 30 seconds of idle time. */
    Iron_io_pool = Iron_elastic_pool_create("io", 64, 30000);
}

void iron_threads_shutdown(void) {
    if (Iron_io_pool) {
        Iron_pool_destroy(Iron_io_pool);
        Iron_io_pool = NULL;
    }
    if (Iron_global_pool) {
        Iron_pool_destroy(Iron_global_pool);
        Iron_global_pool = NULL;
    }
}
