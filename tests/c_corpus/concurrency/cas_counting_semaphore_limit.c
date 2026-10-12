/*
 * title: Lock-free counting semaphore with multi-permit try-acquire
 * topic: concurrency
 * covers: CAS decrement if enough, fetch_add release, all-or-nothing n permits, concurrency limit audit, max-inside CAS
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * permits is an atomic counter of free permits. try_acquire(n) loads it and, if at least n are
 * free, CASes it down by n; the CAS makes "check then take" a single step, so the counter never
 * goes negative and the sum of permits held by threads never exceeds the limit K.
 * release(n) is a plain fetch_add.
 *
 * Audit: `inside` tracks the permits currently held; its maximum (updated by a CAS-max loop)
 * must not exceed K. Total permits handed out and returned must balance to K at the end.
 */
enum { K = 5, T = 6, ROUNDS = 1500 };

static atomic_int permits = K;
static atomic_int inside;
static atomic_int max_inside;
static atomic_long grants, granted_permits;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static int try_acquire(int n) {
    int cur = atomic_load_explicit(&permits, memory_order_relaxed);
    while (cur >= n) {
        if (atomic_compare_exchange_weak_explicit(&permits, &cur, cur - n, memory_order_acquire,
                                                  memory_order_relaxed))
            return 1;
    }
    return 0;
}

static void release(int n) {
    atomic_fetch_add_explicit(&permits, n, memory_order_release);
}

static void note_inside(int delta) {
    int now = atomic_fetch_add(&inside, delta) + delta;
    int m = atomic_load(&max_inside);
    while (now > m && !atomic_compare_exchange_weak(&max_inside, &m, now))
        ;
}

static void *worker(void *p) {
    unsigned s = 0x5bd1e995u * (unsigned)((size_t)p + 1);
    for (int r = 0; r < ROUNDS; r++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        int n = 1 + (int)(s % 3u);
        while (!try_acquire(n))
            sched_yield();
        note_inside(n);
        atomic_fetch_add(&grants, 1);
        atomic_fetch_add(&granted_permits, n);
        if (atomic_load(&permits) < 0)
            atomic_fetch_add(&grants, 1000000); /* would fail the check below */
        if ((s & 7u) == 0)
            sched_yield();
        note_inside(-n);
        release(n);
    }
    return NULL;
}

int main(void) {
    /* deterministic single-thread behavior */
    check(try_acquire(3), "3 of 5");
    check(!try_acquire(3), "only 2 left");
    check(try_acquire(2), "the last 2");
    check(!try_acquire(1), "none left");
    release(5);
    check(atomic_load(&permits) == K, "restored");
    printf("single-thread: 3 ok, 3 refused, 2 ok, 1 refused, restored to %d\n", atomic_load(&permits));

    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    check(atomic_load(&grants) == (long)T * ROUNDS, "every round granted once");
    check(atomic_load(&max_inside) <= K, "limit never exceeded");
    check(atomic_load(&max_inside) >= 1, "someone got in");
    check(atomic_load(&permits) == K, "permits returned");
    check(atomic_load(&inside) == 0, "nobody inside");
    printf("grants=%ld, limit exceeded: no, permits back to %d\n", atomic_load(&grants), atomic_load(&permits));
    unsigned long want = 0;
    for (int p = 0; p < T; p++) {
        unsigned s = 0x5bd1e995u * (unsigned)(p + 1);
        for (int r = 0; r < ROUNDS; r++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            want += 1 + s % 3u;
        }
    }
    check((unsigned long)atomic_load(&granted_permits) == want, "permit units granted");
    printf("permit units granted=%lu\n", want);
    return 0;
}
