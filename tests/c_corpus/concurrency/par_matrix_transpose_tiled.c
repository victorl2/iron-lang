/*
 * title: Parallel tiled matrix transpose, square and rectangular
 * topic: concurrency
 * covers: tile ownership, rectangular matrices, double transpose identity, in-place square transpose
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

enum { T = 8, NT = 5 };

typedef struct {
    const int *src;
    int *dst;
    int rows, cols;
} Tr;

static void tr_worker(void *ctx, int tid, int nt) {
    Tr *t = ctx;
    int tr = (t->rows + T - 1) / T, tc = (t->cols + T - 1) / T;
    int total = tr * tc;
    int lo, hi;
    par_range(total, tid, nt, &lo, &hi);
    for (int k = lo; k < hi; k++) {
        int bi = k / tc, bj = k % tc;
        for (int i = bi * T; i < bi * T + T && i < t->rows; i++)
            for (int j = bj * T; j < bj * T + T && j < t->cols; j++)
                t->dst[j * t->rows + i] = t->src[i * t->cols + j];
    }
}

typedef struct {
    int *a;
    int n;
} Sq;

/* In-place: thread owns tile pairs (bi <= bj); swaps mirror tiles. */
static void sq_worker(void *ctx, int tid, int nt) {
    Sq *s = ctx;
    int nb = (s->n + T - 1) / T;
    int idx = 0;
    for (int bi = 0; bi < nb; bi++)
        for (int bj = bi; bj < nb; bj++, idx++) {
            if (idx % nt != tid)
                continue;
            for (int i = bi * T; i < bi * T + T && i < s->n; i++)
                for (int j = bj * T; j < bj * T + T && j < s->n; j++)
                    if (bi != bj || j > i) {
                        int x = s->a[i * s->n + j];
                        s->a[i * s->n + j] = s->a[j * s->n + i];
                        s->a[j * s->n + i] = x;
                    }
        }
}

int main(void) {
    uint64_t seed = 555;
    int shapes[][2] = {{1, 1}, {8, 8}, {13, 29}, {50, 7}, {64, 64}, {37, 37}};
    for (int s = 0; s < 6; s++) {
        int r = shapes[s][0], c = shapes[s][1];
        int *a = malloc((size_t)r * c * sizeof(int));
        int *b = malloc((size_t)r * c * sizeof(int));
        int *back = malloc((size_t)r * c * sizeof(int));
        check(a && b && back, "alloc");
        for (int i = 0; i < r * c; i++)
            a[i] = (int)(sm64(&seed) % 100000u);
        Tr t = {a, b, r, c};
        par_run(NT, tr_worker, &t);
        for (int i = 0; i < r; i++)
            for (int j = 0; j < c; j++)
                check(b[j * r + i] == a[i * c + j], "element moved");
        Tr u = {b, back, c, r};
        par_run(3, tr_worker, &u);
        check(memcmp(a, back, (size_t)r * c * sizeof(int)) == 0, "double transpose is identity");
        long chk = 0;
        for (int i = 0; i < r * c; i++)
            chk = (chk * 31 + b[i]) % 1000000007L;
        printf("%dx%d -> %dx%d checksum=%ld corner=%d\n", r, c, c, r, chk, b[c * r - 1]);
        if (r == c) {
            Sq q = {a, r};
            par_run(NT, sq_worker, &q);
            check(memcmp(a, b, (size_t)r * c * sizeof(int)) == 0, "in-place square transpose");
            printf("  in-place square transpose matches\n");
        }
        free(a);
        free(b);
        free(back);
    }
    return 0;
}
