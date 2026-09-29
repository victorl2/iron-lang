/*
 * title: Parallel Hillis-Steele inclusive scan with log-step rounds
 * topic: concurrency
 * covers: log2 rounds, double buffering, strided reads, non-power-of-two length, work-count comparison with Blelloch-style two-phase
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

enum { NT = 6 };

typedef struct {
    const unsigned long *src;
    unsigned long *dst;
    int n;
    int stride;
} Rd;

static void round_worker(void *ctx, int tid, int nt) {
    Rd *r = ctx;
    int lo, hi;
    par_range(r->n, tid, nt, &lo, &hi);
    for (int i = lo; i < hi; i++)
        r->dst[i] = i >= r->stride ? r->src[i] + r->src[i - r->stride] : r->src[i];
}

/* returns rounds used; result pointer is written to *res (one of the buffers) */
static int hillis_steele(unsigned long *buf0, unsigned long *buf1, int n, unsigned long **res, long *adds) {
    unsigned long *src = buf0, *dst = buf1;
    int rounds = 0;
    *adds = 0;
    for (int stride = 1; stride < n; stride <<= 1) {
        Rd r = {src, dst, n, stride};
        par_run(NT, round_worker, &r);
        *adds += n - stride;
        unsigned long *t = src;
        src = dst;
        dst = t;
        rounds++;
    }
    *res = src;
    return rounds;
}

int main(void) {
    uint64_t seed = 1010;
    int sizes[] = {1, 2, 3, 8, 100, 1000, 4097};
    for (int t = 0; t < 7; t++) {
        int n = sizes[t];
        unsigned long *a = malloc((size_t)n * sizeof(unsigned long));
        unsigned long *b = malloc((size_t)n * sizeof(unsigned long));
        unsigned long *ref = malloc((size_t)n * sizeof(unsigned long));
        check(a && b && ref, "alloc");
        unsigned long acc = 0;
        for (int i = 0; i < n; i++) {
            a[i] = (unsigned long)(sm64(&seed) % 1000u);
            acc += a[i];
            ref[i] = acc;
        }
        unsigned long *res;
        long adds;
        int rounds = hillis_steele(a, b, n, &res, &adds);
        check(memcmp(res, ref, (size_t)n * sizeof(unsigned long)) == 0, "scan equals sequential");
        int lg = 0;
        while ((1 << lg) < n)
            lg++;
        check(rounds == lg, "ceil(log2 n) rounds");
        /* a work-efficient scan does about 2n additions; Hillis-Steele does n*log n */
        printf("n=%4d rounds=%2d additions=%6ld (work-efficient ~%d) total=%lu\n", n, rounds, adds,
               2 * n, ref[n - 1]);
        free(a);
        free(b);
        free(ref);
    }
    return 0;
}
