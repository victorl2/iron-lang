/*
 * title: Linear sieve with smallest prime factors
 * topic: algorithms
 * covers: linear sieve, smallest prime factor, factorization via spf, omega/Omega, each composite marked once
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define N 200000

static int spf[N + 1];
static int primes[20000];
static int nprimes;
static long marks;

static void linear_sieve(void) {
    for (int i = 2; i <= N; i++) {
        if (!spf[i]) { spf[i] = i; primes[nprimes++] = i; }
        for (int j = 0; j < nprimes; j++) {
            int p = primes[j];
            if (p > spf[i] || (long)p * i > N) break;
            spf[p * i] = p;
            marks++;
        }
    }
}

static int factorize(int n, int *ps, int *es) {
    int k = 0;
    while (n > 1) {
        int p = spf[n], e = 0;
        while (n % p == 0) { n /= p; e++; }
        ps[k] = p; es[k] = e; k++;
    }
    return k;
}

int main(void) {
    linear_sieve();
    printf("primes up to %d: %d\n", N, nprimes);
    printf("composites marked: %ld (each exactly once)\n", marks);
    if (marks != (long)(N - 1 - nprimes)) { fprintf(stderr, "marks wrong\n"); return 1; }

    int probes[] = {2, 360, 1001, 65536, 99991, 123456, 199999, 200000};
    for (size_t t = 0; t < sizeof probes / sizeof probes[0]; t++) {
        int ps[16], es[16];
        int n = probes[t];
        int k = factorize(n, ps, es);
        printf("%d =", n);
        long back = 1;
        int big_omega = 0;
        for (int i = 0; i < k; i++) {
            printf(i ? " * %d^%d" : " %d^%d", ps[i], es[i]);
            for (int e = 0; e < es[i]; e++) back *= ps[i];
            big_omega += es[i];
        }
        printf("  (omega=%d, Omega=%d)\n", k, big_omega);
        if (back != n) { fprintf(stderr, "refactor mismatch %d\n", n); return 1; }
    }

    /* histogram of distinct prime factor counts */
    int hist[8] = {0};
    long sum_omega = 0;
    for (int n = 2; n <= N; n++) {
        int ps[16], es[16];
        int k = factorize(n, ps, es);
        hist[k]++;
        sum_omega += k;
    }
    for (int k = 1; k < 8; k++) if (hist[k]) printf("numbers with omega=%d: %d\n", k, hist[k]);
    printf("sum of omega: %ld\n", sum_omega);

    /* brute-force check of spf on a subset */
    for (int n = 2; n <= 5000; n++) {
        int d = 2;
        while (n % d) d++;
        if (spf[n] != d) { fprintf(stderr, "spf mismatch %d\n", n); return 1; }
    }
    return 0;
}
