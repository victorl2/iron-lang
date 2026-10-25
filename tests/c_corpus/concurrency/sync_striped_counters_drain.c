/*
 * title: Striped counters with concurrent drain
 * topic: concurrency
 * covers: striped counter, cache-line padding, per-stripe atomics, exchange-to-zero drain, monotone snapshots, no lost updates
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { STRIPES = 8, ADDERS = 6, ADDS = 5000 };

typedef struct {
    atomic_long v;
    char pad[56]; /* keep stripes on separate cache lines */
} Stripe;

static Stripe stripes[STRIPES];
static atomic_long single_counter;
static long locked_counter;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

static void add(int thread, long n) {
    atomic_fetch_add_explicit(&stripes[thread % STRIPES].v, n, memory_order_relaxed);
}
static long sum_stripes(void) {
    long s = 0;
    for (int i = 0; i < STRIPES; i++)
        s += atomic_load_explicit(&stripes[i].v, memory_order_relaxed);
    return s;
}
/* Moves everything currently counted out of the stripes; concurrent adds land either before or after. */
static long drain(void) {
    long s = 0;
    for (int i = 0; i < STRIPES; i++)
        s += atomic_exchange_explicit(&stripes[i].v, 0, memory_order_relaxed);
    return s;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static long amount_for(int id, int i) { return (long)((id * 31 + i * 17) % 9) + 1; }

static atomic_int adders_done;

static void *adder(void *p) {
    int id = (int)(intptr_t)p;
    for (int i = 0; i < ADDS; i++) {
        long a = amount_for(id, i);
        add(id, a);
        atomic_fetch_add(&single_counter, a);
        pthread_mutex_lock(&lk);
        locked_counter += a;
        pthread_mutex_unlock(&lk);
        if (i % 500 == 0)
            sched_yield();
    }
    atomic_fetch_add(&adders_done, 1);
    return NULL;
}

static long drained_sum;
static int drains;
static void *drainer(void *p) {
    (void)p;
    while (atomic_load(&adders_done) < ADDERS) {
        drained_sum += drain();
        drains++;
        sched_yield();
    }
    drained_sum += drain();
    return NULL;
}

/* Non-draining phase: a reader's successive sums must never go backwards. */
static int regressions;
static long last_reader_sum;
static void *watcher(void *p) {
    (void)p;
    long prev = 0;
    while (atomic_load(&adders_done) < ADDERS) {
        long s = sum_stripes();
        if (s < prev)
            regressions++;
        prev = s;
        sched_yield();
    }
    last_reader_sum = sum_stripes();
    return NULL;
}

int main(void) {
    long expect = 0;
    for (int id = 0; id < ADDERS; id++)
        for (int i = 0; i < ADDS; i++)
            expect += amount_for(id, i);

    /* Phase 1: adders plus a monotone-snapshot watcher. */
    pthread_t th[ADDERS], w;
    pthread_create(&w, NULL, watcher, NULL);
    for (int i = 0; i < ADDERS; i++)
        pthread_create(&th[i], NULL, adder, (void *)(intptr_t)i);
    for (int i = 0; i < ADDERS; i++)
        pthread_join(th[i], NULL);
    pthread_join(w, NULL);
    long total1 = sum_stripes();
    printf("phase 1: striped sum %ld, single atomic %ld, mutex counter %ld, expected %ld\n", total1,
           atomic_load(&single_counter), locked_counter, expect);
    printf("watcher saw regressions: %d, final snapshot equals total: %s\n", regressions,
           last_reader_sum == total1 ? "yes" : "no");
    check(total1 == expect && atomic_load(&single_counter) == expect && locked_counter == expect, "sums");
    check(regressions == 0 && last_reader_sum == total1, "monotone");

    /* Phase 2: reset and repeat with a drainer running concurrently; nothing may be lost or counted twice. */
    (void)drain();
    atomic_store(&single_counter, 0);
    locked_counter = 0;
    atomic_store(&adders_done, 0);
    pthread_t d;
    pthread_create(&d, NULL, drainer, NULL);
    for (int i = 0; i < ADDERS; i++)
        pthread_create(&th[i], NULL, adder, (void *)(intptr_t)i);
    for (int i = 0; i < ADDERS; i++)
        pthread_join(th[i], NULL);
    pthread_join(d, NULL);
    long rest = sum_stripes();
    printf("phase 2: drained total %ld, left in stripes %ld, expected %ld\n", drained_sum, rest, expect);
    check(drained_sum + rest == expect && rest == 0, "drain conservation");
    (void)drains;
    printf("stripe count %d, per-stripe padding bytes %d\n", STRIPES, (int)(sizeof(Stripe) - sizeof(atomic_long)));
    return 0;
}
