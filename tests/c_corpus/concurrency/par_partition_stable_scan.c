/*
 * title: Parallel stable partition around a pivot
 * topic: concurrency
 * covers: stable two-way partition, per-chunk less/geq counts, offset computation, quicksort-style split, three-way variant
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

enum { N = 7000, NT = 6 };

typedef struct {
    int key;
    int seq;
} Rec;

typedef struct {
    const Rec *in;
    Rec *out;
    int pivot;
    int nless[8], neq[8];
    int loff[8], eoff[8], goff[8];
} Pt;

static int cls(int key, int pivot) {
    return key < pivot ? 0 : key == pivot ? 1 : 2;
}

static void count_worker(void *ctx, int tid, int nt) {
    Pt *p = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    int l = 0, e = 0;
    for (int i = lo; i < hi; i++) {
        int c = cls(p->in[i].key, p->pivot);
        l += c == 0;
        e += c == 1;
    }
    p->nless[tid] = l;
    p->neq[tid] = e;
}

static void scatter_worker(void *ctx, int tid, int nt) {
    Pt *p = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    int lo_o = p->loff[tid], eq_o = p->eoff[tid], gr_o = p->goff[tid];
    for (int i = lo; i < hi; i++) {
        int c = cls(p->in[i].key, p->pivot);
        if (c == 0)
            p->out[lo_o++] = p->in[i];
        else if (c == 1)
            p->out[eq_o++] = p->in[i];
        else
            p->out[gr_o++] = p->in[i];
    }
}

/* returns the sizes of the three groups */
static void partition3(const Rec *in, Rec *out, int pivot, int *nl, int *ne, int *ng) {
    Pt p;
    memset(&p, 0, sizeof p);
    p.in = in;
    p.out = out;
    p.pivot = pivot;
    par_run(NT, count_worker, &p);
    int tl = 0, te = 0;
    for (int t = 0; t < NT; t++) {
        tl += p.nless[t];
        te += p.neq[t];
    }
    int l = 0, e = tl;
    for (int t = 0; t < NT; t++) {
        p.loff[t] = l;
        p.eoff[t] = e;
        l += p.nless[t];
        e += p.neq[t];
    }
    /* greater-than offsets need per-chunk greater counts */
    int gg = tl + te;
    for (int t = 0; t < NT; t++) {
        int lo, hi;
        par_range(N, t, NT, &lo, &hi);
        p.goff[t] = gg;
        gg += (hi - lo) - p.nless[t] - p.neq[t];
    }
    par_run(NT, scatter_worker, &p);
    *nl = tl;
    *ne = te;
    *ng = N - tl - te;
}

int main(void) {
    static Rec in[N], out[N], ref[N];
    uint64_t seed = 1984;
    for (int i = 0; i < N; i++) {
        in[i].key = (int)(sm64(&seed) % 100u);
        in[i].seq = i;
    }
    int pivots[] = {0, 1, 50, 99, 100, 37};
    for (int t = 0; t < 6; t++) {
        int pv = pivots[t];
        int nl, ne, ng;
        partition3(in, out, pv, &nl, &ne, &ng);
        /* sequential stable reference */
        int k = 0;
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < N; i++)
                if (cls(in[i].key, pv) == c)
                    ref[k++] = in[i];
        check(memcmp(out, ref, sizeof out) == 0, "stable three-way partition");
        for (int i = 0; i < nl; i++)
            check(out[i].key < pv, "less group");
        for (int i = nl; i < nl + ne; i++)
            check(out[i].key == pv, "equal group");
        for (int i = nl + ne; i < N; i++)
            check(out[i].key > pv, "greater group");
        printf("pivot %3d: less=%4d equal=%4d greater=%4d  first-of-each seq: %d %d %d\n", pv, nl, ne,
               ng, nl ? out[0].seq : -1, ne ? out[nl].seq : -1, ng ? out[nl + ne].seq : -1);
    }
    return 0;
}
