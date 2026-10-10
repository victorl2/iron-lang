/*
 * title: Bitonic sort network with threaded compare-exchange stages
 * topic: concurrency
 * covers: bitonic merge, power-of-two padding, stage/substage loop, direction bit, sentinel padding
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

enum { NT = 8 };

typedef struct {
    int *a;
    int n;
    int k; /* block size */
    int j; /* distance */
} Stage;

static void stage_worker(void *ctx, int tid, int nt) {
    Stage *s = ctx;
    int lo, hi;
    par_range(s->n, tid, nt, &lo, &hi);
    for (int i = lo; i < hi; i++) {
        int l = i ^ s->j;
        if (l > i) {
            int asc = (i & s->k) == 0;
            int x = s->a[i], y = s->a[l];
            if ((x > y) == asc) {
                s->a[i] = y;
                s->a[l] = x;
            }
        }
    }
}

static int bitonic_sort(int *a, int n, int *stages) {
    Stage s;
    s.a = a;
    s.n = n;
    int count = 0;
    for (int k = 2; k <= n; k <<= 1)
        for (int j = k >> 1; j > 0; j >>= 1) {
            s.k = k;
            s.j = j;
            par_run(NT, stage_worker, &s);
            count++;
        }
    *stages = count;
    return 0;
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    uint64_t seed = 77;
    int sizes[] = {1, 5, 16, 100, 333, 1000};
    for (int t = 0; t < 6; t++) {
        int n = sizes[t];
        int m = 1;
        while (m < n)
            m <<= 1;
        int *a = malloc((size_t)m * sizeof(int));
        int *ref = malloc((size_t)n * sizeof(int));
        check(a && ref, "alloc");
        for (int i = 0; i < n; i++) {
            a[i] = (int)(sm64(&seed) % 2001u) - 1000;
            ref[i] = a[i];
        }
        for (int i = n; i < m; i++)
            a[i] = 2147483647; /* sentinel padding sorts to the tail */
        qsort(ref, (size_t)n, sizeof(int), cmp_int);
        int stages = 0;
        if (m > 1)
            bitonic_sort(a, m, &stages);
        check(memcmp(a, ref, (size_t)n * sizeof(int)) == 0, "bitonic equals qsort");
        for (int i = n; i < m; i++)
            check(a[i] == 2147483647, "padding stays at tail");
        int lg = 0;
        while ((1 << lg) < m)
            lg++;
        check(stages == lg * (lg + 1) / 2, "stage count is lg(lg+1)/2");
        long sum = 0;
        for (int i = 0; i < n; i++)
            sum += (long)a[i] * (i + 1);
        printf("n=%d padded=%d stages=%d min=%d max=%d weighted=%ld\n", n, m, stages, a[0],
               a[n - 1], sum);
        free(a);
        free(ref);
    }
    return 0;
}
