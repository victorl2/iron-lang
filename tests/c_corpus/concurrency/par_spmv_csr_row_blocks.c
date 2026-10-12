/*
 * title: Parallel sparse matrix-vector product in CSR
 * topic: concurrency
 * covers: CSR construction from triplets, nnz-balanced row partition, integer arithmetic, transpose product check
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

enum { R = 300, C = 260, NNZ_TARGET = 4000, NT = 6 };

typedef struct {
    int rows, cols, nnz;
    int *ptr, *col;
    long *val;
} Csr;

typedef struct {
    int r, c;
    long v;
} Trip;

static int cmp_trip(const void *x, const void *y) {
    const Trip *a = x, *b = y;
    if (a->r != b->r)
        return a->r - b->r;
    return a->c - b->c;
}

static Csr from_triplets(Trip *t, int n, int rows, int cols) {
    qsort(t, (size_t)n, sizeof(Trip), cmp_trip);
    Csr m;
    m.rows = rows;
    m.cols = cols;
    m.ptr = calloc((size_t)rows + 1, sizeof(int));
    m.col = malloc((size_t)n * sizeof(int));
    m.val = malloc((size_t)n * sizeof(long));
    check(m.ptr && m.col && m.val, "alloc");
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k > 0 && t[i].r == t[i - 1].r && t[i].c == t[i - 1].c) {
            m.val[k - 1] += t[i].v; /* merge duplicates */
            continue;
        }
        m.col[k] = t[i].c;
        m.val[k] = t[i].v;
        m.ptr[t[i].r + 1]++;
        k++;
    }
    for (int r = 0; r < rows; r++)
        m.ptr[r + 1] += m.ptr[r];
    m.nnz = k;
    return m;
}

typedef struct {
    const Csr *m;
    const long *x;
    long *y;
    int cut[9];
} Mv;

static void mv_worker(void *ctx, int tid, int nt) {
    Mv *p = ctx;
    (void)nt;
    for (int r = p->cut[tid]; r < p->cut[tid + 1]; r++) {
        long s = 0;
        for (int k = p->m->ptr[r]; k < p->m->ptr[r + 1]; k++)
            s += p->m->val[k] * p->x[p->m->col[k]];
        p->y[r] = s;
    }
}

/* choose row cuts so each thread gets roughly nnz/nt entries */
static void balance(const Csr *m, int nt, int *cut) {
    cut[0] = 0;
    int r = 0;
    for (int t = 1; t < nt; t++) {
        long target = (long)m->nnz * t / nt;
        while (r < m->rows && m->ptr[r] < target)
            r++;
        cut[t] = r;
    }
    cut[nt] = m->rows;
}

int main(void) {
    static Trip trip[NNZ_TARGET];
    uint64_t seed = 1729;
    for (int i = 0; i < NNZ_TARGET; i++) {
        uint64_t r = sm64(&seed);
        /* skew rows: a few dense rows */
        int row = (r & 3u) == 0 ? (int)((r >> 8) % 5u) : (int)((r >> 8) % R);
        trip[i].r = row;
        trip[i].c = (int)((r >> 32) % C);
        trip[i].v = (long)((r >> 48) % 19u) - 9;
    }
    Trip tcopy[NNZ_TARGET];
    memcpy(tcopy, trip, sizeof trip);
    Csr a = from_triplets(trip, NNZ_TARGET, R, C);
    long x[C], y[R], ref[R];
    for (int i = 0; i < C; i++)
        x[i] = (long)(sm64(&seed) % 41u) - 20;
    /* dense reference from the raw triplets */
    memset(ref, 0, sizeof ref);
    for (int i = 0; i < NNZ_TARGET; i++)
        ref[tcopy[i].r] += tcopy[i].v * x[tcopy[i].c];
    for (int nt = 1; nt <= 8; nt++) {
        Mv p;
        p.m = &a;
        p.x = x;
        p.y = y;
        balance(&a, nt, p.cut);
        memset(y, 0x7f, sizeof y);
        par_run(nt, mv_worker, &p);
        check(memcmp(y, ref, sizeof y) == 0, "y equals dense reference");
        if (nt == 6) {
            printf("row cuts for 6 threads:");
            for (int t = 0; t <= nt; t++)
                printf(" %d", p.cut[t]);
            printf("\nnnz per thread:");
            for (int t = 0; t < nt; t++)
                printf(" %d", a.ptr[p.cut[t + 1]] - a.ptr[p.cut[t]]);
            printf("\n");
        }
    }
    long sum = 0, maxv = ref[0];
    for (int r = 0; r < R; r++) {
        sum += ref[r];
        if (ref[r] > maxv)
            maxv = ref[r];
    }
    printf("nnz after merging duplicates=%d (of %d triplets)\n", a.nnz, NNZ_TARGET);
    printf("sum(y)=%ld max(y)=%ld y[0]=%ld y[%d]=%ld\n", sum, maxv, ref[0], R - 1, ref[R - 1]);
    free(a.ptr);
    free(a.col);
    free(a.val);
    return 0;
}
