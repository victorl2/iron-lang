/*
 * title: Parallel LSD radix sort with per-thread histograms
 * topic: concurrency
 * covers: per-thread digit histograms, offset prefix, stable scatter, 4 passes
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

enum { N = 6000, NT = 4, RADIX = 256 };

typedef struct {
    const uint32_t *src;
    uint32_t *dst;
    int shift;
    int hist[8][RADIX];
    int off[8][RADIX];
} Pass;

static void count_worker(void *ctx, int tid, int nt) {
    Pass *p = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    memset(p->hist[tid], 0, sizeof p->hist[tid]);
    for (int i = lo; i < hi; i++)
        p->hist[tid][(p->src[i] >> p->shift) & 255u]++;
}

static void scatter_worker(void *ctx, int tid, int nt) {
    Pass *p = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    for (int i = lo; i < hi; i++) {
        unsigned d = (p->src[i] >> p->shift) & 255u;
        p->dst[p->off[tid][d]++] = p->src[i];
    }
}

static void par_radix_sort(uint32_t *a, uint32_t *tmp, int *scatter_moves) {
    uint32_t *src = a, *dst = tmp;
    for (int shift = 0; shift < 32; shift += 8) {
        Pass p;
        p.src = src;
        p.dst = dst;
        p.shift = shift;
        par_run(NT, count_worker, &p);
        /* digit-major, thread-minor offsets keep the sort stable */
        int run = 0;
        for (int d = 0; d < RADIX; d++)
            for (int t = 0; t < NT; t++) {
                p.off[t][d] = run;
                run += p.hist[t][d];
            }
        check(run == N, "offsets cover input");
        par_run(NT, scatter_worker, &p);
        *scatter_moves += N;
        uint32_t *t = src;
        src = dst;
        dst = t;
    }
    /* four passes: result is back in a */
    check(src == a, "even number of passes");
}

static int cmp_u32(const void *x, const void *y) {
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return a < b ? -1 : a > b;
}

int main(void) {
    static uint32_t a[N], tmp[N], ref[N];
    uint64_t seed = 2024;
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < N; i++) {
            uint32_t v = (uint32_t)sm64(&seed);
            if (round == 1)
                v &= 0xFFu; /* heavy duplicates */
            if (round == 2)
                v = (v & 0xFF000000u) | (uint32_t)(i % 7); /* only top byte and low digits vary */
            a[i] = v;
            ref[i] = v;
        }
        qsort(ref, N, sizeof ref[0], cmp_u32);
        int moves = 0;
        par_radix_sort(a, tmp, &moves);
        check(memcmp(a, ref, sizeof a) == 0, "radix equals qsort");
        uint64_t h = 0;
        for (int i = 0; i < N; i++)
            h = h * 1000003u + a[i];
        printf("round %d: min=%u max=%u median=%u moves=%d hash=%016llx\n", round, (unsigned)a[0],
               (unsigned)a[N - 1], (unsigned)a[N / 2], moves, (unsigned long long)h);
    }
    return 0;
}
