/*
 * title: Segmented sieve over sliding windows
 * topic: algorithms
 * covers: segmented sieve, base primes, window offsets, prime gaps, brute-force cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;

static int base_primes[4096];
static int nbase;

static void build_base(int limit) {
    unsigned char *c = calloc((size_t)limit + 1, 1);
    if (!c) exit(2);
    nbase = 0;
    for (int i = 2; i <= limit; i++) {
        if (!c[i]) {
            base_primes[nbase++] = i;
            for (long j = (long)i * i; j <= limit; j += i) c[j] = 1;
        }
    }
    free(c);
}

/* Sieve [lo, hi) into flags (1 = prime). */
static void sieve_window(u64 lo, u64 hi, unsigned char *flags) {
    u64 len = hi - lo;
    memset(flags, 1, (size_t)len);
    for (u64 i = 0; i < len; i++)
        if (lo + i < 2) flags[i] = 0;
    for (int k = 0; k < nbase; k++) {
        u64 p = (u64)base_primes[k];
        if (p * p >= hi) break;
        u64 start = (lo + p - 1) / p * p;
        if (start < p * p) start = p * p;
        for (u64 m = start; m < hi; m += p) flags[m - lo] = 0;
    }
}

static int is_prime_trial(u64 n) {
    if (n < 2) return 0;
    for (u64 d = 2; d * d <= n; d++)
        if (n % d == 0) return 0;
    return 1;
}

int main(void) {
    build_base(2000); /* covers windows up to 4,000,000 */
    enum { W = 1000 };
    unsigned char flags[W];
    u64 starts[] = {0, 1000, 5000, 99000, 1000000, 3999000};
    int nstarts = (int)(sizeof starts / sizeof starts[0]);
    u64 total = 0;
    u64 prev = 0;
    u64 max_gap = 0, gap_at = 0;
    for (int s = 0; s < nstarts; s++) {
        u64 lo = starts[s], hi = lo + W;
        sieve_window(lo, hi, flags);
        int cnt = 0;
        u64 first = 0, last = 0;
        for (u64 i = 0; i < W; i++) {
            u64 n = lo + i;
            if (flags[i]) {
                if (!cnt) first = n;
                last = n;
                cnt++;
                if (prev && n - prev > max_gap) { max_gap = n - prev; gap_at = prev; }
                prev = n;
            }
            if (i % 37 == 0 || (flags[i] != 0) != (is_prime_trial(n) != 0)) {
                if ((flags[i] != 0) != (is_prime_trial(n) != 0)) {
                    fprintf(stderr, "mismatch at %llu\n", n);
                    return 1;
                }
            }
        }
        total += (u64)cnt;
        printf("window [%llu,%llu): %d primes, first %llu, last %llu\n", lo, hi, cnt, first, last);
    }
    printf("total primes over windows: %llu\n", total);
    printf("largest gap between consecutive found primes: %llu after %llu\n", max_gap, gap_at);

    /* Whole-range consistency: sieve [0, 100000) in windows of varying size. */
    static unsigned char buf[100000];
    u64 sum_a = 0, sum_b = 0;
    sieve_window(0, 100000, buf);
    for (int i = 0; i < 100000; i++) if (buf[i]) sum_a += (u64)i;
    for (u64 lo = 0; lo < 100000; lo += 777) {
        u64 hi = lo + 777 > 100000 ? 100000 : lo + 777;
        sieve_window(lo, hi, flags);
        for (u64 i = 0; i < hi - lo; i++) if (flags[i]) sum_b += lo + i;
    }
    if (sum_a != sum_b) { fprintf(stderr, "window sum mismatch\n"); return 1; }
    printf("sum of primes below 100000: %llu\n", sum_a);
    return 0;
}
