/*
 * title: Parallel prefix minimum and maximum with argmin tracking
 * topic: concurrency
 * covers: three-phase scan, chunk carries, idempotent operators, leftmost argmin tie-break, running records
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

enum { N = 10000, NT = 7 };

typedef struct {
    int v;
    int idx;
} Best;

/* leftmost minimum: on ties keep the earlier index (a is always earlier) */
static Best bmin(Best a, Best b) {
    return b.v < a.v ? b : a;
}

static Best bmax(Best a, Best b) {
    return b.v > a.v ? b : a;
}

typedef struct {
    const int *a;
    Best *pmin, *pmax;
    Best cmin[8], cmax[8]; /* chunk totals */
    Best carry_min[8], carry_max[8];
} Sc;

static void local_worker(void *ctx, int tid, int nt) {
    Sc *s = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    Best mn = {s->a[lo], lo}, mx = {s->a[lo], lo};
    for (int i = lo; i < hi; i++) {
        Best e = {s->a[i], i};
        mn = bmin(mn, e);
        mx = bmax(mx, e);
        s->pmin[i] = mn;
        s->pmax[i] = mx;
    }
    s->cmin[tid] = mn;
    s->cmax[tid] = mx;
}

static void fix_worker(void *ctx, int tid, int nt) {
    Sc *s = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    if (tid == 0)
        return;
    for (int i = lo; i < hi; i++) {
        s->pmin[i] = bmin(s->carry_min[tid], s->pmin[i]);
        s->pmax[i] = bmax(s->carry_max[tid], s->pmax[i]);
    }
}

int main(void) {
    static int a[N];
    static Best pmin[N], pmax[N];
    uint64_t seed = 31;
    for (int i = 0; i < N; i++)
        a[i] = (int)(sm64(&seed) % 5000u) - 2500 + (i % 997 == 0 ? -3000 : 0);
    Sc s;
    memset(&s, 0, sizeof s);
    s.a = a;
    s.pmin = pmin;
    s.pmax = pmax;
    par_run(NT, local_worker, &s);
    /* serial scan over the NT chunk totals */
    Best am = s.cmin[0], aM = s.cmax[0];
    for (int t = 1; t < NT; t++) {
        s.carry_min[t] = am;
        s.carry_max[t] = aM;
        am = bmin(am, s.cmin[t]);
        aM = bmax(aM, s.cmax[t]);
    }
    par_run(NT, fix_worker, &s);
    Best rm = {a[0], 0}, rM = {a[0], 0};
    int records_min = 0, records_max = 0;
    for (int i = 0; i < N; i++) {
        Best e = {a[i], i};
        if (i > 0 && e.v < rm.v)
            records_min++;
        if (i > 0 && e.v > rM.v)
            records_max++;
        rm = bmin(rm, e);
        rM = bmax(rM, e);
        check(pmin[i].v == rm.v && pmin[i].idx == rm.idx, "prefix min and argmin");
        check(pmax[i].v == rM.v && pmax[i].idx == rM.idx, "prefix max and argmax");
    }
    /* count strict records from the parallel result: positions where the prefix changes */
    int changes = 0;
    for (int i = 1; i < N; i++)
        if (pmin[i].idx != pmin[i - 1].idx)
            changes++;
    check(changes == records_min, "record count equals prefix changes");
    printf("global min %d at %d, global max %d at %d\n", pmin[N - 1].v, pmin[N - 1].idx,
           pmax[N - 1].v, pmax[N - 1].idx);
    printf("running-minimum records=%d running-maximum records=%d\n", records_min, records_max);
    for (int t = 1; t < NT; t++) {
        int lo, hi;
        par_range(N, t, NT, &lo, &hi);
        printf("chunk %d starts at %d: prefix min %d (idx %d) max %d (idx %d)\n", t, lo,
               pmin[lo].v, pmin[lo].idx, pmax[lo].v, pmax[lo].idx);
    }
    return 0;
}
