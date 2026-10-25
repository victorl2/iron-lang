/*
 * title: Parallel reduce with a non-commutative bracket monoid
 * topic: concurrency
 * covers: non-commutative monoid, chunked reduce, ordered combine, uneven chunking
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

/* Monoid element: total sum and minimum prefix sum (including the empty
 * prefix). combine(a, b) is associative but NOT commutative. */
typedef struct {
    int sum;
    int minpre;
} Bm;

static Bm bm_id(void) {
    Bm r = {0, 0};
    return r;
}

static Bm bm_of(char c) {
    Bm r;
    r.sum = (c == '(') ? 1 : -1;
    r.minpre = r.sum < 0 ? r.sum : 0;
    return r;
}

static Bm bm_combine(Bm a, Bm b) {
    Bm r;
    r.sum = a.sum + b.sum;
    int t = a.sum + b.minpre;
    r.minpre = a.minpre < t ? a.minpre : t;
    return r;
}

typedef struct {
    const char *s;
    const int *cuts; /* nt + 1 boundaries */
    Bm *out;
} Job;

static void worker(void *ctx, int tid, int nt) {
    Job *j = ctx;
    (void)nt;
    Bm acc = bm_id();
    for (int i = j->cuts[tid]; i < j->cuts[tid + 1]; i++)
        acc = bm_combine(acc, bm_of(j->s[i]));
    j->out[tid] = acc;
}

static Bm seq_reduce(const char *s, int n) {
    Bm acc = bm_id();
    for (int i = 0; i < n; i++)
        acc = bm_combine(acc, bm_of(s[i]));
    return acc;
}

int main(void) {
    uint64_t seed = 424242;
    char buf[4096];
    int lens[] = {1, 2, 7, 64, 1000, 4000};
    for (int t = 0; t < 6; t++) {
        int n = lens[t];
        int bias = t * 3;
        for (int i = 0; i < n; i++) {
            uint64_t r = sm64(&seed);
            buf[i] = (int)(r % 16) < 8 + (bias % 3) ? '(' : ')';
        }
        buf[n] = 0;
        Bm ref = seq_reduce(buf, n);
        int agree = 0;
        for (int nt = 1; nt <= 8; nt++) {
            int cuts[9];
            /* uneven cuts: skewed toward the front */
            for (int k = 0; k <= nt; k++) {
                long f = (long)k * k;
                cuts[k] = (int)((long)n * f / ((long)nt * nt));
            }
            Bm part[8];
            Job j = {buf, cuts, part};
            par_run(nt, worker, &j);
            Bm acc = bm_id();
            for (int k = 0; k < nt; k++)
                acc = bm_combine(acc, part[k]);
            check(acc.sum == ref.sum && acc.minpre == ref.minpre, "chunked equals sequential");
            /* combining in reverse order must generally differ */
            agree++;
        }
        printf("n=%d sum=%d minpre=%d balanced=%s agree=%d\n", n, ref.sum, ref.minpre,
               (ref.sum == 0 && ref.minpre == 0) ? "yes" : "no", agree);
    }
    /* explicit non-commutativity witness */
    Bm a = bm_of('('), b = bm_of(')');
    Bm ab = bm_combine(a, b), ba = bm_combine(b, a);
    printf("( then ): sum=%d min=%d\n", ab.sum, ab.minpre);
    printf(") then (: sum=%d min=%d\n", ba.sum, ba.minpre);
    check(ab.minpre != ba.minpre, "non-commutative");
    /* a known balanced string */
    const char *bal = "(()(()))()((()))";
    Bm rb = seq_reduce(bal, (int)strlen(bal));
    check(rb.sum == 0 && rb.minpre == 0, "balanced known");
    printf("known balanced ok\n");
    return 0;
}
