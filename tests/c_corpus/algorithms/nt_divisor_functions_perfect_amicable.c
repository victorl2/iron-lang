/*
 * title: Divisor functions, perfect and amicable numbers
 * topic: algorithms
 * covers: tau and sigma from factorization, divisor sieve, perfect/abundant/deficient, amicable pairs, highly composite
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define N 100000

static unsigned sigma_tab[N + 1];
static unsigned tau_tab[N + 1];

static unsigned sigma_by_factoring(unsigned n, unsigned *tau) {
    unsigned s = 1, t = 1;
    for (unsigned p = 2; p * p <= n; p++) {
        if (n % p) continue;
        unsigned pk = 1, sum = 1, e = 0;
        while (n % p == 0) { n /= p; pk *= p; sum += pk; e++; }
        s *= sum;
        t *= e + 1;
    }
    if (n > 1) { s *= n + 1; t *= 2; }
    *tau = t;
    return s;
}

int main(void) {
    for (unsigned d = 1; d <= N; d++)
        for (unsigned m = d; m <= N; m += d) { sigma_tab[m] += d; tau_tab[m]++; }
    for (unsigned n = 1; n <= N; n += 13) {
        unsigned t, s = sigma_by_factoring(n, &t);
        if (s != sigma_tab[n] || t != tau_tab[n]) { fprintf(stderr, "sigma mismatch %u\n", n); return 1; }
    }
    unsigned perfect[8], np = 0, abundant = 0, deficient = 0;
    for (unsigned n = 2; n <= N; n++) {
        unsigned aliquot = sigma_tab[n] - n;
        if (aliquot == n) { if (np < 8) perfect[np++] = n; }
        else if (aliquot > n) abundant++;
        else deficient++;
    }
    printf("perfect numbers <= %d:", N);
    for (unsigned i = 0; i < np; i++) printf(" %u", perfect[i]);
    printf("\nabundant: %u, deficient: %u\n", abundant, deficient);

    printf("amicable pairs:");
    for (unsigned a = 2; a <= N; a++) {
        unsigned b = sigma_tab[a] - a;
        if (b > a && b <= N && sigma_tab[b] - b == a) printf(" (%u,%u)", a, b);
    }
    printf("\n");

    /* Even perfect numbers are 2^(p-1)(2^p - 1) with 2^p - 1 prime */
    for (int p = 2; p <= 13; p++) {
        unsigned long long m = (1ull << p) - 1, prime = 1;
        for (unsigned long long d = 2; d * d <= m; d++) if (m % d == 0) prime = 0;
        if (prime) {
            unsigned long long v = (1ull << (p - 1)) * m;
            if (v <= N && sigma_tab[v] != 2 * v) { fprintf(stderr, "euclid form fails\n"); return 1; }
            printf("Mersenne exponent %d gives perfect number %llu\n", p, v);
        }
    }
    /* highly composite records */
    printf("highly composite:");
    unsigned best = 0;
    for (unsigned n = 1; n <= N; n++)
        if (tau_tab[n] > best) { best = tau_tab[n]; printf(" %u(%u)", n, best); }
    printf("\n");
    /* n with tau odd are perfect squares */
    unsigned oddtau = 0;
    for (unsigned n = 1; n <= N; n++) {
        if (tau_tab[n] & 1) {
            oddtau++;
            unsigned r = 0;
            while (r * r < n) r++;
            if (r * r != n) { fprintf(stderr, "odd tau non-square\n"); return 1; }
        }
    }
    printf("numbers with odd divisor count: %u (squares up to %d)\n", oddtau, N);
    unsigned long long total_tau = 0;
    for (unsigned n = 1; n <= N; n++) total_tau += tau_tab[n];
    printf("sum tau(n) for n<=%d: %llu\n", N, total_tau);
    printf("sigma(60)=%u tau(60)=%u sigma(97)=%u\n", sigma_tab[60], tau_tab[60], sigma_tab[97]);
    return 0;
}
