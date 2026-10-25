/*
 * title: Chase-Lev lock-free work-stealing deque
 * topic: concurrency
 * covers: owner push/take at bottom, thieves steal at top, seq_cst fence, last-element CAS race, task tree with pending counter
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Each worker owns a deque: it pushes and takes at the bottom (LIFO, no contention), other
 * workers steal from the top (FIFO) with a CAS on top. The only contested case is one remaining
 * element, where the owner's take and a thief's steal both CAS top and exactly one wins.
 * This is the C11 formulation of Le, Pop, Cohen and Zappa Nardelli (2013), fixed capacity.
 *
 * Workload: an implicit binary tree over node ids 1..LIMIT-1 (children 2i and 2i+1). Processing
 * node i adds weight(i) to the worker's local sum and pushes its children. `pending` counts
 * tasks pushed but not yet processed; the run ends when it reaches 0.
 *
 * Oracle: the total of weight(i) over all nodes, and the count of processed nodes, are fixed
 * whatever the schedule, so no task is lost or run twice.
 */
enum { WORKERS = 4, CAP = 1 << 14, LIMIT = 40000, EMPTY = -1, ABORT = -2 };

typedef struct {
    atomic_long top, bottom;
    atomic_int buf[CAP];
} Deque;

static Deque dq[WORKERS];
static atomic_long pending;
static unsigned long local_sum[WORKERS];
static long local_count[WORKERS];
static long stolen[WORKERS];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void dq_push(Deque *d, int x) {
    long b = atomic_load_explicit(&d->bottom, memory_order_relaxed);
    long t = atomic_load_explicit(&d->top, memory_order_acquire);
    check(b - t < CAP, "deque capacity");
    atomic_store_explicit(&d->buf[b % CAP], x, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
}

static int dq_take(Deque *d) {
    long b = atomic_load_explicit(&d->bottom, memory_order_relaxed) - 1;
    atomic_store_explicit(&d->bottom, b, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    long t = atomic_load_explicit(&d->top, memory_order_relaxed);
    int x;
    if (t <= b) {
        x = atomic_load_explicit(&d->buf[b % CAP], memory_order_relaxed);
        if (t == b) { /* last element: race with thieves */
            if (!atomic_compare_exchange_strong_explicit(&d->top, &t, t + 1, memory_order_seq_cst,
                                                         memory_order_relaxed))
                x = EMPTY;
            atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
        }
    } else {
        x = EMPTY;
        atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
    }
    return x;
}

static int dq_steal(Deque *d) {
    long t = atomic_load_explicit(&d->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    long b = atomic_load_explicit(&d->bottom, memory_order_acquire);
    if (t < b) {
        int x = atomic_load_explicit(&d->buf[t % CAP], memory_order_relaxed);
        if (!atomic_compare_exchange_strong_explicit(&d->top, &t, t + 1, memory_order_seq_cst,
                                                     memory_order_relaxed))
            return ABORT;
        return x;
    }
    return EMPTY;
}

static unsigned weight(int i) {
    unsigned x = (unsigned)i * 2654435761u;
    return (x >> 20) + 1u;
}

static void process(int me, int node) {
    local_sum[me] += weight(node);
    local_count[me]++;
    int kids[2] = {2 * node, 2 * node + 1};
    for (int k = 0; k < 2; k++)
        if (kids[k] < LIMIT) {
            atomic_fetch_add(&pending, 1);
            dq_push(&dq[me], kids[k]);
        }
    atomic_fetch_sub(&pending, 1);
}

static void *worker(void *p) {
    int me = (int)(size_t)p;
    unsigned rs = 77u + 31u * (unsigned)me;
    while (atomic_load(&pending) > 0) {
        int x = dq_take(&dq[me]);
        if (x >= 0) {
            process(me, x);
            continue;
        }
        rs = rs * 1103515245u + 12345u;
        int victim = (int)((rs >> 16) % WORKERS);
        if (victim == me) {
            sched_yield();
            continue;
        }
        x = dq_steal(&dq[victim]);
        if (x >= 0) {
            stolen[me]++;
            process(me, x);
        } else {
            sched_yield();
        }
    }
    return NULL;
}

int main(void) {
    atomic_store(&pending, 1);
    dq_push(&dq[0], 1);
    pthread_t th[WORKERS];
    for (int i = 0; i < WORKERS; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < WORKERS; i++)
        pthread_join(th[i], NULL);
    unsigned long sum = 0, want = 0;
    long cnt = 0;
    for (int i = 0; i < WORKERS; i++) {
        sum += local_sum[i];
        cnt += local_count[i];
    }
    for (int i = 1; i < LIMIT; i++)
        want += weight(i);
    check(cnt == LIMIT - 1, "every node processed once");
    check(sum == want, "weight sum");
    check(atomic_load(&pending) == 0, "no pending tasks");
    for (int i = 0; i < WORKERS; i++)
        check(atomic_load(&dq[i].top) == atomic_load(&dq[i].bottom), "deques empty");
    /* single-threaded semantic check: LIFO for the owner, FIFO for a thief */
    Deque *d = &dq[1];
    for (int i = 10; i < 15; i++)
        dq_push(d, i);
    check(dq_steal(d) == 10 && dq_take(d) == 14 && dq_steal(d) == 11 && dq_take(d) == 13, "end order");
    check(dq_take(d) == 12 && dq_take(d) == EMPTY && dq_steal(d) == EMPTY, "last element then empty");
    printf("nodes processed=%ld weight sum=%lu\n", cnt, sum);
    printf("deque order check: owner LIFO, thief FIFO ok\n");
    return 0;
}
