/*
 * title: Shared atomic histogram with log2 buckets and percentiles
 * topic: concurrency
 * covers: fetch_add buckets, per-thread local merge vs shared atomics, percentile from cumulative counts, monotone snapshots
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Two histograms are built from the same samples: one shared (every thread does relaxed
 * fetch_add on the bucket) and one private per thread and merged after join. Addition commutes,
 * so both must equal the sequential histogram bucket by bucket. A concurrent reader takes
 * snapshots of the shared histogram; each bucket count it sees must never decrease, and the
 * snapshot total must never exceed the final total.
 */
enum { T = 5, N = 20000, NB = 24, LINEAR = 16 };

static atomic_ulong shared_log[NB];
static atomic_ulong shared_lin[LINEAR];
static unsigned long local_log[T][NB];
static atomic_int done_workers;
static long snapshots;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static uint32_t sample(int t, int i) {
    uint64_t x = (uint64_t)(t * 1000003 + i + 1) * 0x9e3779b97f4a7c15ull;
    x ^= x >> 29;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 32;
    /* heavy-tailed: shift by a pseudo-random amount so buckets are populated unevenly */
    return (uint32_t)((x & 0xffffu) >> ((x >> 40) % 14u));
}

static int log2_bucket(uint32_t v) {
    int b = 0;
    while (v > 1) {
        v >>= 1;
        b++;
    }
    return v == 0 ? 0 : b + 1 < NB ? b + 1 : NB - 1; /* 0 -> 0, 1 -> 1, 2..3 -> 2, ... */
}

static void *worker(void *p) {
    int t = (int)(size_t)p;
    for (int i = 0; i < N; i++) {
        uint32_t v = sample(t, i);
        int b = log2_bucket(v);
        atomic_fetch_add_explicit(&shared_log[b], 1ul, memory_order_relaxed);
        atomic_fetch_add_explicit(&shared_lin[v % LINEAR], 1ul, memory_order_relaxed);
        local_log[t][b]++;
    }
    atomic_fetch_add(&done_workers, 1);
    return NULL;
}

static void *watcher(void *p) {
    (void)p;
    unsigned long last[NB] = {0};
    while (atomic_load(&done_workers) < T) {
        unsigned long total = 0;
        for (int b = 0; b < NB; b++) {
            unsigned long c = atomic_load_explicit(&shared_log[b], memory_order_relaxed);
            check(c >= last[b], "bucket count went backwards");
            last[b] = c;
            total += c;
        }
        check(total <= (unsigned long)T * N, "snapshot total exceeds final");
        snapshots++;
        sched_yield();
    }
    return NULL;
}

static int percentile_bucket(const unsigned long *h, int nb, unsigned long total, int pct) {
    unsigned long need = (total * (unsigned long)pct + 99) / 100, acc = 0;
    for (int b = 0; b < nb; b++) {
        acc += h[b];
        if (acc >= need)
            return b;
    }
    return nb - 1;
}

int main(void) {
    pthread_t th[T], w;
    check(pthread_create(&w, NULL, watcher, NULL) == 0, "create");
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    pthread_join(w, NULL);
    unsigned long seq_log[NB] = {0}, seq_lin[LINEAR] = {0}, merged[NB] = {0}, sh[NB], total = 0;
    for (int t = 0; t < T; t++)
        for (int i = 0; i < N; i++) {
            uint32_t v = sample(t, i);
            seq_log[log2_bucket(v)]++;
            seq_lin[v % LINEAR]++;
        }
    for (int b = 0; b < NB; b++) {
        for (int t = 0; t < T; t++)
            merged[b] += local_log[t][b];
        sh[b] = atomic_load(&shared_log[b]);
        check(sh[b] == seq_log[b], "shared histogram equals sequential");
        check(merged[b] == seq_log[b], "merged local histograms equal sequential");
        total += sh[b];
    }
    for (int b = 0; b < LINEAR; b++)
        check(atomic_load(&shared_lin[b]) == seq_lin[b], "linear histogram");
    check(total == (unsigned long)T * N, "total samples");
    printf("samples=%lu\n", total);
    for (int b = 0; b < NB; b++)
        if (sh[b]) {
            unsigned lo = b == 0 ? 0 : 1u << (b - 1);
            printf("bucket %2d [%5u..): %lu\n", b, lo, sh[b]);
        }
    int p50 = percentile_bucket(sh, NB, total, 50), p90 = percentile_bucket(sh, NB, total, 90),
        p99 = percentile_bucket(sh, NB, total, 99);
    printf("p50 bucket=%d p90 bucket=%d p99 bucket=%d\n", p50, p90, p99);
    printf("mod-16 spread:");
    for (int b = 0; b < LINEAR; b++)
        printf(" %lu", seq_lin[b]);
    printf("\n");
    (void)snapshots;
    return 0;
}
