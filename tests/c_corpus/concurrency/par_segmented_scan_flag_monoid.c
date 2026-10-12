/*
 * title: Parallel segmented inclusive scan via a flag monoid
 * topic: concurrency
 * covers: segmented scan operator, chunk aggregates, carry propagation across segment heads, per-segment sums
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

enum { N = 6000, NT = 7 };

/* (head flag, value). Associative, not commutative:
 * (f1,v1) + (f2,v2) = (f1|f2, f2 ? v2 : v1+v2) */
typedef struct {
    int f;
    long v;
} Fv;

static Fv op(Fv a, Fv b) {
    Fv r;
    r.f = a.f | b.f;
    r.v = b.f ? b.v : a.v + b.v;
    return r;
}

typedef struct {
    const Fv *in;
    Fv *out;
    Fv total[8];
    Fv carry[8];
    int has_carry[8];
} Ss;

static void local_worker(void *ctx, int tid, int nt) {
    Ss *s = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    Fv acc = s->in[lo];
    s->out[lo] = acc;
    for (int i = lo + 1; i < hi; i++) {
        acc = op(acc, s->in[i]);
        s->out[i] = acc;
    }
    s->total[tid] = acc;
}

static void fix_worker(void *ctx, int tid, int nt) {
    Ss *s = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    if (!s->has_carry[tid])
        return;
    for (int i = lo; i < hi; i++) {
        /* once a head flag is inside the local prefix, the carry no longer matters */
        if (s->out[i].f)
            break;
        s->out[i] = op(s->carry[tid], s->out[i]);
    }
}

int main(void) {
    static Fv in[N], out[N], ref[N];
    uint64_t seed = 12321;
    int densities[] = {2, 30, 300, 1000}; /* head probability per mille */
    for (int d = 0; d < 4; d++) {
        for (int i = 0; i < N; i++) {
            uint64_t r = sm64(&seed);
            in[i].f = i == 0 || (int)(r % 1000u) < densities[d];
            in[i].v = (long)((r >> 20) % 100u) - 30;
        }
        Ss s;
        memset(&s, 0, sizeof s);
        s.in = in;
        s.out = out;
        par_run(NT, local_worker, &s);
        Fv acc = s.total[0];
        for (int t = 1; t < NT; t++) {
            s.carry[t] = acc;
            s.has_carry[t] = 1;
            acc = op(acc, s.total[t]);
        }
        par_run(NT, fix_worker, &s);
        /* the fixed prefix: out[i].f is only "seen a head" within the chunk, so compare values */
        Fv r = in[0];
        ref[0] = r;
        for (int i = 1; i < N; i++) {
            r = op(r, in[i]);
            ref[i] = r;
        }
        for (int i = 0; i < N; i++)
            check(out[i].v == ref[i].v, "segmented scan value");
        /* per-segment totals: value at the position before each head */
        int nseg = 0;
        long maxseg = out[0].v, minseg = out[0].v;
        for (int i = 1; i <= N; i++)
            if (i == N || in[i].f) {
                long sum = out[i - 1].v;
                nseg++;
                if (sum > maxseg)
                    maxseg = sum;
                if (sum < minseg)
                    minseg = sum;
            }
        /* brute force segment sums */
        int nb = 0;
        long run = 0;
        for (int i = 0; i < N; i++) {
            if (in[i].f)
                run = 0;
            run += in[i].v;
            if (i == N - 1 || in[i + 1].f) {
                nb++;
                check(run == out[i].v, "segment total equals brute force");
            }
        }
        check(nb == nseg, "segment count");
        printf("head density %4d/1000: segments=%4d max_total=%ld min_total=%ld last=%ld\n",
               densities[d], nseg, maxseg, minseg, out[N - 1].v);
    }
    return 0;
}
