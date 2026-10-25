/*
 * title: Three-state futex-style mutex over a parking lot
 * topic: concurrency
 * covers: Drepper three-state mutex, compare-and-swap fast path, park and wake, lost wakeup avoidance, state transitions
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* "Parking lot": park() sleeps only if the word still holds the expected value, checked under the lot lock. */
static pthread_mutex_t lot_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lot_cv = PTHREAD_COND_INITIALIZER;

static void park(atomic_int *word, int expected) {
    pthread_mutex_lock(&lot_mu);
    if (atomic_load(word) == expected)
        pthread_cond_wait(&lot_cv, &lot_mu);
    pthread_mutex_unlock(&lot_mu);
}
static void wake_all(void) {
    pthread_mutex_lock(&lot_mu); /* taking the lock orders us after any parker's value check */
    pthread_cond_broadcast(&lot_cv);
    pthread_mutex_unlock(&lot_mu);
}

/* 0 = free, 1 = held without waiters, 2 = held and possibly contended. */
typedef struct {
    atomic_int state;
} FMutex;

static atomic_int parks, wakes;

static void fm_lock(FMutex *m) {
    int c = 0;
    if (atomic_compare_exchange_strong_explicit(&m->state, &c, 1, memory_order_acquire, memory_order_relaxed))
        return; /* uncontended fast path */
    if (c != 2)
        c = atomic_exchange_explicit(&m->state, 2, memory_order_acquire);
    while (c != 0) {
        atomic_fetch_add(&parks, 1);
        park(&m->state, 2);
        c = atomic_exchange_explicit(&m->state, 2, memory_order_acquire);
    }
}
static void fm_unlock(FMutex *m) {
    if (atomic_fetch_sub_explicit(&m->state, 1, memory_order_release) != 1) {
        atomic_store_explicit(&m->state, 0, memory_order_release);
        atomic_fetch_add(&wakes, 1);
        wake_all();
    }
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { T = 6, PER = 400 };

static FMutex fm;
static atomic_int in_cs, violations;
static long counter;
static int hist[T];

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    for (int i = 0; i < PER; i++) {
        fm_lock(&fm);
        if (atomic_fetch_add(&in_cs, 1) != 0)
            atomic_fetch_add(&violations, 1);
        long v = counter;
        if (i % 4 == 0)
            sched_yield();
        counter = v + 1;
        hist[id]++;
        atomic_fetch_sub(&in_cs, 1);
        fm_unlock(&fm);
    }
    return NULL;
}

static atomic_int b_in;
static void *blocked(void *p) {
    (void)p;
    atomic_store(&b_in, 1);
    fm_lock(&fm);
    atomic_store(&b_in, 2);
    fm_unlock(&fm);
    return NULL;
}

int main(void) {
    /* Deterministic state walk. */
    printf("initial state %d\n", atomic_load(&fm.state));
    fm_lock(&fm);
    printf("after lock: %d\n", atomic_load(&fm.state));
    pthread_t b;
    pthread_create(&b, NULL, blocked, NULL);
    while (atomic_load(&fm.state) != 2)
        sched_yield();
    printf("with a parked waiter: %d\n", atomic_load(&fm.state));
    check(atomic_load(&b_in) == 1, "waiter still blocked");
    fm_unlock(&fm);
    pthread_join(b, NULL);
    printf("waiter acquired and released: %d, wakes issued %d\n", atomic_load(&fm.state), atomic_load(&wakes) >= 1);
    check(atomic_load(&b_in) == 2 && atomic_load(&fm.state) == 0 && atomic_load(&wakes) >= 1, "wake path");

    pthread_t th[T];
    for (int i = 0; i < T; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    int total = 0;
    for (int i = 0; i < T; i++)
        total += hist[i];
    printf("stress: counter=%ld sections=%d violations=%d\n", counter, total, atomic_load(&violations));
    printf("final state %d\n", atomic_load(&fm.state));
    check(counter == T * PER && total == T * PER, "counter");
    check(atomic_load(&violations) == 0 && atomic_load(&fm.state) == 0, "exclusion");
    return 0;
}
