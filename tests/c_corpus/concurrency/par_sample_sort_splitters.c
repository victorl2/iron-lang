/*
 * title: Parallel sample sort with regular sampling
 * topic: concurrency
 * covers: splitter selection, bucket counting, bucket scatter, per-bucket sort, qsort cross-check
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

enum { N = 5000, NT = 5 };

typedef struct {
    const int *a;
    int *out;
    int splitters[NT - 1];
    int cnt[NT][NT]; /* [source thread][bucket] */
    int off[NT][NT];
    int bstart[NT + 1];
} Ctx;

static int bucket_of(const Ctx *c, int v) {
    int b = 0;
    while (b < NT - 1 && v >= c->splitters[b])
        b++;
    return b;
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

static void count_worker(void *ctx, int tid, int nt) {
    Ctx *c = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    for (int b = 0; b < NT; b++)
        c->cnt[tid][b] = 0;
    for (int i = lo; i < hi; i++)
        c->cnt[tid][bucket_of(c, c->a[i])]++;
}

static void scatter_worker(void *ctx, int tid, int nt) {
    Ctx *c = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    for (int i = lo; i < hi; i++) {
        int b = bucket_of(c, c->a[i]);
        c->out[c->off[tid][b]++] = c->a[i];
    }
}

static void sort_worker(void *ctx, int tid, int nt) {
    Ctx *c = ctx;
    (void)nt;
    qsort(c->out + c->bstart[tid], (size_t)(c->bstart[tid + 1] - c->bstart[tid]), sizeof(int),
          cmp_int);
}

int main(void) {
    static int a[N], out[N], ref[N];
    uint64_t seed = 31337;
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < N; i++) {
            uint64_t r = sm64(&seed);
            int v = (int)(r % 100000u) - 50000;
            if (round == 1)
                v = (int)(r % 40u); /* few distinct values, skewed buckets */
            if (round == 2)
                v = i - N / 2; /* already sorted */
            a[i] = v;
            ref[i] = v;
        }
        qsort(ref, N, sizeof(int), cmp_int);
        Ctx c;
        c.a = a;
        c.out = out;
        /* regular sampling: each thread sorts a local copy of its slice
         * and picks NT samples; splitters come from the sorted samples */
        int samples[NT * NT];
        int ns = 0;
        for (int t = 0; t < NT; t++) {
            int lo, hi;
            par_range(N, t, NT, &lo, &hi);
            int m = hi - lo;
            int *loc = malloc((size_t)m * sizeof(int));
            check(loc != NULL, "alloc");
            memcpy(loc, a + lo, (size_t)m * sizeof(int));
            qsort(loc, (size_t)m, sizeof(int), cmp_int);
            for (int s = 0; s < NT; s++)
                samples[ns++] = loc[(long)m * s / NT];
            free(loc);
        }
        qsort(samples, (size_t)ns, sizeof(int), cmp_int);
        for (int s = 1; s < NT; s++)
            c.splitters[s - 1] = samples[s * NT];
        par_run(NT, count_worker, &c);
        int run = 0;
        for (int b = 0; b < NT; b++) {
            c.bstart[b] = run;
            for (int t = 0; t < NT; t++) {
                c.off[t][b] = run;
                run += c.cnt[t][b];
            }
        }
        c.bstart[NT] = run;
        check(run == N, "all elements bucketed");
        par_run(NT, scatter_worker, &c);
        par_run(NT, sort_worker, &c);
        check(memcmp(out, ref, sizeof out) == 0, "sample sort equals qsort");
        printf("round %d: splitters", round);
        for (int s = 0; s < NT - 1; s++)
            printf(" %d", c.splitters[s]);
        printf(" | bucket sizes");
        for (int b = 0; b < NT; b++)
            printf(" %d", c.bstart[b + 1] - c.bstart[b]);
        printf("\n");
    }
    return 0;
}
