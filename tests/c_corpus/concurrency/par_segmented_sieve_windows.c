/*
 * title: Parallel segmented sieve over independent windows
 * topic: concurrency
 * covers: base primes, per-thread segments, twin primes across window boundaries, gap statistics
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

enum { LIMIT = 2000000, SEG = 65536, NT = 7 };

typedef struct {
    const int *base;
    int nbase;
    long first_prime[64];   /* first prime in each segment (or -1) */
    long last_prime[64];    /* last prime in each segment (or -1) */
    long count[64];
    long twins_inside[64];
    long maxgap_inside[64];
    int nseg;
} Sv;

static void seg_worker(void *ctx, int tid, int nt) {
    Sv *s = ctx;
    unsigned char *mark = malloc(SEG);
    check(mark != NULL, "alloc");
    for (int sg = tid; sg < s->nseg; sg += nt) {
        long lo = (long)sg * SEG, hi = lo + SEG;
        if (hi > LIMIT + 1)
            hi = LIMIT + 1;
        memset(mark, 1, SEG);
        for (long v = lo; v < 2 && v < hi; v++)
            mark[v - lo] = 0;
        for (int i = 0; i < s->nbase; i++) {
            long p = s->base[i];
            if (p * p >= hi)
                break;
            long start = (lo + p - 1) / p * p;
            if (start < p * p)
                start = p * p;
            for (long m = start; m < hi; m += p)
                mark[m - lo] = 0;
        }
        long cnt = 0, prev = -1, tw = 0, mg = 0, first = -1;
        for (long v = lo; v < hi; v++)
            if (mark[v - lo]) {
                cnt++;
                if (first < 0)
                    first = v;
                if (prev >= 0) {
                    if (v - prev == 2)
                        tw++;
                    if (v - prev > mg)
                        mg = v - prev;
                }
                prev = v;
            }
        s->count[sg] = cnt;
        s->first_prime[sg] = first;
        s->last_prime[sg] = prev;
        s->twins_inside[sg] = tw;
        s->maxgap_inside[sg] = mg;
    }
    free(mark);
}

int main(void) {
    /* base primes up to sqrt(LIMIT) by simple sieve */
    enum { R = 1415 };
    unsigned char small[R + 1];
    memset(small, 1, sizeof small);
    int base[300], nbase = 0;
    for (int i = 2; i <= R; i++)
        if (small[i]) {
            base[nbase++] = i;
            for (int j = i * i; j <= R; j += i)
                small[j] = 0;
        }
    static Sv s;
    s.base = base;
    s.nbase = nbase;
    s.nseg = (LIMIT + SEG) / SEG;
    check(s.nseg <= 64, "segment table");
    par_run(NT, seg_worker, &s);
    long total = 0, twins = 0, maxgap = 0, gapat = 0, prevlast = -1;
    for (int sg = 0; sg < s.nseg; sg++) {
        total += s.count[sg];
        twins += s.twins_inside[sg];
        if (s.maxgap_inside[sg] > maxgap)
            maxgap = s.maxgap_inside[sg];
        if (s.first_prime[sg] >= 0) {
            /* pairs that straddle a boundary */
            if (prevlast >= 0) {
                long g = s.first_prime[sg] - prevlast;
                if (g == 2)
                    twins++;
                if (g > maxgap)
                    maxgap = g;
            }
            prevlast = s.last_prime[sg];
        }
    }
    /* cross-check with a plain sieve */
    unsigned char *all = malloc(LIMIT + 1);
    check(all != NULL, "alloc");
    memset(all, 1, LIMIT + 1);
    all[0] = all[1] = 0;
    for (long i = 2; i * i <= LIMIT; i++)
        if (all[i])
            for (long j = i * i; j <= LIMIT; j += i)
                all[j] = 0;
    long rc = 0, rtw = 0, rmg = 0, rprev = -1;
    for (long v = 2; v <= LIMIT; v++)
        if (all[v]) {
            rc++;
            if (rprev >= 0) {
                if (v - rprev == 2)
                    rtw++;
                if (v - rprev > rmg) {
                    rmg = v - rprev;
                    gapat = rprev;
                }
            }
            rprev = v;
        }
    free(all);
    check(total == rc, "prime count matches");
    check(twins == rtw, "twin count matches");
    check(maxgap == rmg, "max gap matches");
    check(total == 148933, "pi(2e6) known value");
    printf("limit=%d segments=%d primes=%ld twins=%ld maxgap=%ld after %ld\n", LIMIT, s.nseg, total,
           twins, maxgap, gapat);
    return 0;
}
