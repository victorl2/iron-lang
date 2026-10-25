/*
 * title: Relaxed statistics aggregator with monotone monitor
 * topic: concurrency
 * covers: relaxed atomics, count/sum/sumsq/min/max, CAS min/max, monotone observation, join as synchronization
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Correctness argument: statistics counters do not guard any other memory, so relaxed ordering
 * is enough. Each counter has its own modification order, and RMWs never lose updates. A monitor
 * thread may read count and sum at different moments (no cross-variable consistency), but a
 * single variable never appears to go backwards (coherence), which the monitor checks.
 * The final totals are read after pthread_join, which synchronizes with each worker's exit.
 */
enum { T = 5, N = 6000 };

typedef struct {
    atomic_ulong count, sum, sumsq;
    atomic_long minv, maxv;
} Stats;

static Stats st;
static atomic_int done_workers;
static unsigned long monitor_reads;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long sample(int t, int i) {
    unsigned x = (unsigned)(t * 7919 + i * 104729 + 13) * 2246822519u;
    x ^= x >> 16;
    x *= 3266489917u;
    x ^= x >> 13;
    return (long)(x % 2001u) - 1000;
}

static void record(long v) {
    atomic_fetch_add_explicit(&st.count, 1ul, memory_order_relaxed);
    atomic_fetch_add_explicit(&st.sum, (unsigned long)v, memory_order_relaxed);
    atomic_fetch_add_explicit(&st.sumsq, (unsigned long)(v * v), memory_order_relaxed);
    long cur = atomic_load_explicit(&st.minv, memory_order_relaxed);
    while (v < cur && !atomic_compare_exchange_weak_explicit(&st.minv, &cur, v, memory_order_relaxed,
                                                             memory_order_relaxed))
        ;
    cur = atomic_load_explicit(&st.maxv, memory_order_relaxed);
    while (v > cur && !atomic_compare_exchange_weak_explicit(&st.maxv, &cur, v, memory_order_relaxed,
                                                             memory_order_relaxed))
        ;
}

static void *worker(void *p) {
    int t = (int)(size_t)p;
    for (int i = 0; i < N; i++)
        record(sample(t, i));
    atomic_fetch_add(&done_workers, 1);
    return NULL;
}

static void *monitor(void *p) {
    (void)p;
    unsigned long last_count = 0;
    long last_min = 1000000, last_max = -1000000;
    while (atomic_load(&done_workers) < T) {
        unsigned long c = atomic_load_explicit(&st.count, memory_order_relaxed);
        long mn = atomic_load_explicit(&st.minv, memory_order_relaxed);
        long mx = atomic_load_explicit(&st.maxv, memory_order_relaxed);
        check(c >= last_count, "count went backwards");
        check(mn <= last_min, "min increased");
        check(mx >= last_max, "max decreased");
        last_count = c;
        last_min = mn;
        last_max = mx;
        monitor_reads++;
        sched_yield();
    }
    return NULL;
}

int main(void) {
    atomic_store(&st.minv, 1000000);
    atomic_store(&st.maxv, -1000000);
    pthread_t th[T], mon;
    check(pthread_create(&mon, NULL, monitor, NULL) == 0, "create");
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    pthread_join(mon, NULL);
    unsigned long c = 0, s = 0, sq = 0;
    long mn = 1000000, mx = -1000000;
    for (int t = 0; t < T; t++)
        for (int i = 0; i < N; i++) {
            long v = sample(t, i);
            c++;
            s += (unsigned long)v;
            sq += (unsigned long)(v * v);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    check(atomic_load(&st.count) == c, "count");
    check(atomic_load(&st.sum) == s, "sum");
    check(atomic_load(&st.sumsq) == sq, "sumsq");
    check(atomic_load(&st.minv) == mn, "min");
    check(atomic_load(&st.maxv) == mx, "max");
    (void)monitor_reads;
    long isum = (long)atomic_load(&st.sum);
    printf("count=%lu sum=%ld sumsq=%lu min=%ld max=%ld\n", atomic_load(&st.count), isum,
           atomic_load(&st.sumsq), atomic_load(&st.minv), atomic_load(&st.maxv));
    printf("mean(x1000)=%ld\n", isum * 1000 / (long)atomic_load(&st.count));
    return 0;
}
