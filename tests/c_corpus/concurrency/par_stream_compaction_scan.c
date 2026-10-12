/*
 * title: Parallel stream compaction using an exclusive scan of flags
 * topic: concurrency
 * covers: predicate flags, per-chunk counts, exclusive offsets, order-preserving scatter, multiple predicates
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

enum { N = 9000 };

typedef int (*Pred)(int);

static int p_even(int v) { return (v & 1) == 0; }
static int p_neg(int v) { return v < 0; }
static int p_div7_not3(int v) { return v % 7 == 0 && v % 3 != 0; }
static int p_none(int v) { (void)v; return 0; }
static int p_all(int v) { (void)v; return 1; }

typedef struct {
    const int *in;
    int *out;
    unsigned char *flag;
    Pred pred;
    int n;
    int cnt[8];
    int off[8];
} Cp;

static void flag_worker(void *ctx, int tid, int nt) {
    Cp *c = ctx;
    int lo, hi;
    par_range(c->n, tid, nt, &lo, &hi);
    int n = 0;
    for (int i = lo; i < hi; i++) {
        c->flag[i] = (unsigned char)c->pred(c->in[i]);
        n += c->flag[i];
    }
    c->cnt[tid] = n;
}

static void scatter_worker(void *ctx, int tid, int nt) {
    Cp *c = ctx;
    int lo, hi;
    par_range(c->n, tid, nt, &lo, &hi);
    int o = c->off[tid];
    for (int i = lo; i < hi; i++)
        if (c->flag[i])
            c->out[o++] = c->in[i];
}

static int compact(int nt, Pred pred, const int *in, int n, int *out, unsigned char *flag) {
    Cp c;
    memset(&c, 0, sizeof c);
    c.in = in;
    c.out = out;
    c.flag = flag;
    c.pred = pred;
    c.n = n;
    par_run(nt, flag_worker, &c);
    int run = 0;
    for (int t = 0; t < nt; t++) {
        c.off[t] = run;
        run += c.cnt[t];
    }
    par_run(nt, scatter_worker, &c);
    return run;
}

int main(void) {
    static int in[N], out[N], ref[N];
    static unsigned char flag[N];
    uint64_t seed = 6502;
    for (int i = 0; i < N; i++)
        in[i] = (int)(sm64(&seed) % 2001u) - 1000;
    Pred preds[] = {p_even, p_neg, p_div7_not3, p_none, p_all};
    const char *names[] = {"even", "negative", "div7-not-div3", "none", "all"};
    for (int p = 0; p < 5; p++) {
        int nref = 0;
        for (int i = 0; i < N; i++)
            if (preds[p](in[i]))
                ref[nref++] = in[i];
        for (int nt = 1; nt <= 8; nt++) {
            memset(out, 0x55, sizeof out);
            int n = compact(nt, preds[p], in, N, out, flag);
            check(n == nref, "count");
            check(memcmp(out, ref, (size_t)n * sizeof(int)) == 0, "order preserved");
        }
        long sum = 0;
        for (int i = 0; i < nref; i++)
            sum += ref[i];
        printf("%-14s kept=%4d sum=%ld first=%d last=%d\n", names[p], nref, sum,
               nref ? ref[0] : 0, nref ? ref[nref - 1] : 0);
    }
    /* chained compaction: negatives, then evens of those */
    int n1 = compact(5, p_neg, in, N, out, flag);
    static int out2[N];
    int n2 = compact(3, p_even, out, n1, out2, flag);
    int r1 = 0, r2 = 0;
    for (int i = 0; i < N; i++)
        if (in[i] < 0) {
            r1++;
            if ((in[i] & 1) == 0)
                r2++;
        }
    check(n1 == r1 && n2 == r2, "chained compaction counts");
    printf("chained: negatives=%d then even=%d\n", n1, n2);
    return 0;
}
