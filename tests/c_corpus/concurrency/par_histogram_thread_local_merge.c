/*
 * title: Parallel histogram with per-thread buckets and ordered merge
 * topic: concurrency
 * covers: private histograms, ordered merge, percentile extraction, chunk-shape independence
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*ParFn)(void *ctx, int tid, int nt);
typedef struct {
    ParFn fn;
    void *ctx;
    int tid;
    int nt;
} ParJob;

static void *par_tramp(void *p) {
    ParJob *j = p;
    j->fn(j->ctx, j->tid, j->nt);
    return NULL;
}

/* Run fn on nt threads (nt <= 8) and join them all. */
static inline void par_run(int nt, ParFn fn, void *ctx) {
    pthread_t th[8];
    ParJob jobs[8];
    for (int i = 0; i < nt; i++) {
        jobs[i].fn = fn;
        jobs[i].ctx = ctx;
        jobs[i].tid = i;
        jobs[i].nt = nt;
        if (pthread_create(&th[i], NULL, par_tramp, &jobs[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }
    for (int i = 0; i < nt; i++)
        pthread_join(th[i], NULL);
}

/* Even split of [0,n) into nt contiguous ranges. */
static inline void par_range(int n, int tid, int nt, int *lo, int *hi) {
    *lo = (int)((long)n * tid / nt);
    *hi = (int)((long)n * (tid + 1) / nt);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline uint64_t sm64(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

enum { N = 20000, BUCKETS = 64 };

typedef struct {
    const unsigned *data;
    long hist[8][BUCKETS];
    long sum[8];
} H;

static void hist_worker(void *ctx, int tid, int nt) {
    H *h = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    memset(h->hist[tid], 0, sizeof h->hist[tid]);
    long s = 0;
    for (int i = lo; i < hi; i++) {
        h->hist[tid][h->data[i] / 16u]++;
        s += h->data[i];
    }
    h->sum[tid] = s;
}

static unsigned skewed(uint64_t r) {
    /* triangular-ish distribution over [0, 1024) */
    unsigned a = (unsigned)(r % 512u), b = (unsigned)((r >> 20) % 512u);
    return a + b;
}

int main(void) {
    static unsigned data[N];
    uint64_t seed = 1001;
    for (int i = 0; i < N; i++)
        data[i] = skewed(sm64(&seed));
    long ref[BUCKETS];
    memset(ref, 0, sizeof ref);
    long refsum = 0;
    for (int i = 0; i < N; i++) {
        ref[data[i] / 16u]++;
        refsum += data[i];
    }
    static H h;
    h.data = data;
    for (int nt = 1; nt <= 8; nt++) {
        par_run(nt, hist_worker, &h);
        long merged[BUCKETS];
        long sum = 0;
        memset(merged, 0, sizeof merged);
        for (int t = 0; t < nt; t++) {
            for (int b = 0; b < BUCKETS; b++)
                merged[b] += h.hist[t][b];
            sum += h.sum[t];
        }
        check(memcmp(merged, ref, sizeof ref) == 0, "merged equals sequential");
        check(sum == refsum, "sum equals");
        if (nt == 8) {
            long cum = 0;
            int q[3] = {25, 50, 90};
            int qi = 0;
            for (int b = 0; b < BUCKETS && qi < 3; b++) {
                cum += merged[b];
                while (qi < 3 && cum * 100 >= (long)q[qi] * N) {
                    printf("p%d falls in bucket %d (values %d..%d)\n", q[qi], b, b * 16, b * 16 + 15);
                    qi++;
                }
            }
            for (int b = 0; b < BUCKETS; b += 4) {
                long s = merged[b] + merged[b + 1] + merged[b + 2] + merged[b + 3];
                printf("[%4d,%4d) %5ld ", b * 16, b * 16 + 64, s);
                for (long k = 0; k < s / 100; k++)
                    putchar('*');
                putchar('\n');
            }
        }
    }
    printf("mean*100=%ld\n", refsum * 100 / N);
    return 0;
}
