/*
 * title: Parallel prime counting with deterministic Miller-Rabin
 * topic: concurrency
 * covers: 64-bit modular multiplication, deterministic witness set, strided ranges, interval counts vs sieve
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

enum { NT = 6 };

/* Unsigned 64-bit modular multiplication without a wider type: double-and-add. */
static uint64_t mulmod(uint64_t a, uint64_t b, uint64_t m) {
    uint64_t r = 0;
    a %= m;
    while (b) {
        if (b & 1) {
            r += a;
            if (r >= m || r < a)
                r -= m;
        }
        uint64_t a2 = a << 1;
        if (a2 >= m || a2 < a)
            a2 -= m;
        a = a2;
        b >>= 1;
    }
    return r;
}

static uint64_t powmod(uint64_t b, uint64_t e, uint64_t m) {
    uint64_t r = 1;
    b %= m;
    while (e) {
        if (e & 1)
            r = mulmod(r, b, m);
        b = mulmod(b, b, m);
        e >>= 1;
    }
    return r;
}

static int is_prime(uint64_t n) {
    static const uint64_t small[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    if (n < 2)
        return 0;
    for (int i = 0; i < 12; i++) {
        if (n == small[i])
            return 1;
        if (n % small[i] == 0)
            return 0;
    }
    uint64_t d = n - 1;
    int s = 0;
    while ((d & 1) == 0) {
        d >>= 1;
        s++;
    }
    for (int i = 0; i < 12; i++) {
        uint64_t x = powmod(small[i], d, n);
        if (x == 1 || x == n - 1)
            continue;
        int comp = 1;
        for (int r = 1; r < s; r++) {
            x = mulmod(x, x, n);
            if (x == n - 1) {
                comp = 0;
                break;
            }
        }
        if (comp)
            return 0;
    }
    return 1;
}

typedef struct {
    uint64_t lo, hi; /* [lo, hi) */
    long count[8];
} Rng;

static void count_worker(void *ctx, int tid, int nt) {
    Rng *r = ctx;
    long c = 0;
    /* strided assignment: thread t takes lo+t, lo+t+nt, ... */
    for (uint64_t v = r->lo + (uint64_t)tid; v < r->hi; v += (uint64_t)nt)
        c += is_prime(v);
    r->count[tid] = c;
}

int main(void) {
    /* sieve reference up to 300000 */
    enum { L = 300000 };
    static unsigned char comp[L];
    comp[0] = comp[1] = 1;
    for (int i = 2; (long)i * i < L; i++)
        if (!comp[i])
            for (int j = i * i; j < L; j += i)
                comp[j] = 1;
    long pref[L + 1];
    pref[0] = 0;
    for (int i = 0; i < L; i++)
        pref[i + 1] = pref[i] + !comp[i];
    for (uint64_t lo = 0; lo < L; lo += 100000) {
        Rng r;
        r.lo = lo;
        r.hi = lo + 100000;
        par_run(NT, count_worker, &r);
        long tot = 0;
        for (int t = 0; t < NT; t++)
            tot += r.count[t];
        check(tot == pref[r.hi] - pref[r.lo], "count matches sieve");
        printf("primes in [%llu,%llu) = %ld\n", (unsigned long long)r.lo,
               (unsigned long long)r.hi, tot);
    }
    /* large values: known primes and composites (strong pseudoprimes to small bases) */
    static const uint64_t primes[] = {18446744073709551557ULL, 9223372036854775783ULL,
                                      1000000000000000003ULL, 4294967311ULL};
    static const uint64_t composites[] = {3215031751ULL, 3825123056546413051ULL, 4294967297ULL,
                                          18446744073709551615ULL};
    for (int i = 0; i < 4; i++) {
        check(is_prime(primes[i]), "known large prime");
        printf("prime ok %llu\n", (unsigned long long)primes[i]);
    }
    for (int i = 0; i < 4; i++) {
        check(!is_prime(composites[i]), "known composite rejected");
        printf("composite ok %llu\n", (unsigned long long)composites[i]);
    }
    /* a window of large numbers counted in parallel and sequentially */
    Rng r;
    r.lo = 1000000000000ULL;
    r.hi = r.lo + 20000;
    par_run(NT, count_worker, &r);
    long tot = 0, seqc = 0;
    for (int t = 0; t < NT; t++)
        tot += r.count[t];
    for (uint64_t v = r.lo; v < r.hi; v++)
        seqc += is_prime(v);
    check(tot == seqc, "window equals sequential");
    printf("primes in [1e12, 1e12+20000) = %ld\n", tot);
    return 0;
}
