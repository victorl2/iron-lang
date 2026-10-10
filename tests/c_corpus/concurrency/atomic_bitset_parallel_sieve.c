/*
 * title: Parallel prime sieve on a shared atomic bitset
 * topic: concurrency
 * covers: fetch_or on shared words, work claiming with fetch_add, commutative marking, prime counting, known pi(n) values
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Workers claim the next small prime p (p*p <= LIMIT) with fetch_add on a shared cursor and mark
 * its odd multiples as composite with fetch_or on 64-bit words. OR is commutative and
 * idempotent, so the final bitset does not depend on which thread marked what or in which order,
 * and concurrent marking of the same word by different threads loses no bit.
 * The result is checked against a plain sequential sieve and against known values of pi(n).
 */
enum { LIMIT = 300000, T = 5 };
#define WORDS ((LIMIT / 2) / 64 + 1) /* bit i stands for the odd number 2*i+1 */

static atomic_uint_least64_t composite[WORDS];
static unsigned small_primes[200];
static int nsmall;
static atomic_int cursor;
static atomic_long marks[T];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void *worker(void *p) {
    int me = (int)(size_t)p;
    long m = 0;
    for (;;) {
        int k = atomic_fetch_add(&cursor, 1);
        if (k >= nsmall)
            break;
        unsigned pr = small_primes[k];
        for (unsigned long x = (unsigned long)pr * pr; x <= LIMIT; x += 2ul * pr) {
            unsigned long bit = x / 2;
            atomic_fetch_or_explicit(&composite[bit / 64], (uint64_t)1 << (bit % 64), memory_order_relaxed);
            m++;
        }
    }
    atomic_store(&marks[me], m);
    return NULL;
}

static int is_prime(unsigned long n) {
    if (n < 2)
        return 0;
    if (n == 2)
        return 1;
    if ((n & 1u) == 0)
        return 0;
    unsigned long bit = n / 2;
    return !((atomic_load(&composite[bit / 64]) >> (bit % 64)) & 1u);
}

int main(void) {
    /* small primes up to sqrt(LIMIT) by trial division */
    for (unsigned n = 3; (unsigned long)n * n <= LIMIT; n += 2) {
        int ok = 1;
        for (unsigned d = 3; d * d <= n; d += 2)
            if (n % d == 0)
                ok = 0;
        if (ok)
            small_primes[nsmall++] = n;
    }
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    /* sequential reference */
    static unsigned char ref[LIMIT + 1];
    for (unsigned long i = 2; i <= LIMIT; i++)
        ref[i] = 1;
    for (unsigned long i = 2; i * i <= LIMIT; i++)
        if (ref[i])
            for (unsigned long j = i * i; j <= LIMIT; j += i)
                ref[j] = 0;
    long count = 0, twins = 0, last = 0;
    long by_bound[4] = {0};
    const long bounds[4] = {1000, 10000, 100000, 300000};
    for (unsigned long n = 1; n <= LIMIT; n++) {
        check(is_prime(n) == (ref[n] != 0), "matches sequential sieve");
        if (ref[n]) {
            count++;
            last = (long)n;
            if (n >= 3 && ref[n - 2])
                twins++;
            for (int b = 0; b < 4; b++)
                if ((long)n <= bounds[b])
                    by_bound[b]++;
        }
    }
    check(by_bound[0] == 168 && by_bound[1] == 1229 && by_bound[2] == 9592, "known pi(n) values");
    check(nsmall > 0 && cursor >= nsmall, "all small primes claimed");
    printf("small primes used as sievers: %d\n", nsmall + 1); /* plus 2, handled by using odd numbers only */
    for (int b = 0; b < 4; b++)
        printf("pi(%ld) = %ld\n", bounds[b], by_bound[b]);
    printf("largest prime <= %d: %ld, twin prime pairs: %ld\n", LIMIT, last, twins);
    long tot_marks = 0;
    for (int i = 0; i < T; i++)
        tot_marks += atomic_load(&marks[i]);
    check(tot_marks > count, "marking work was done");
    return 0;
}
