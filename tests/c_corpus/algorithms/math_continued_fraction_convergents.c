/*
 * title: Continued fractions and convergents
 * topic: algorithms
 * covers: Euclid expansion of rationals, convergent recurrence, periodic expansion of quadratic surds, best rational approximation, semiconvergents
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

typedef long long i64;

static int cf_rational(i64 p, i64 q, i64 *a, int max) {
    int n = 0;
    while (q && n < max) {
        i64 t = p / q;
        a[n++] = t;
        i64 r = p - t * q;
        p = q;
        q = r;
    }
    return n;
}

static void convergent(const i64 *a, int k, i64 *num, i64 *den) {
    i64 h0 = 1, h1 = a[0], k0 = 0, k1 = 1;
    for (int i = 1; i <= k; i++) {
        i64 h2 = a[i] * h1 + h0, k2 = a[i] * k1 + k0;
        h0 = h1; h1 = h2; k0 = k1; k1 = k2;
    }
    *num = h1;
    *den = k1;
}

static i64 isqrt(i64 n) {
    i64 r = (i64)sqrt((double)n);
    while (r * r > n) r--;
    while ((r + 1) * (r + 1) <= n) r++;
    return r;
}

/* sqrt(D) = [a0; period...]; returns period length, 0 if D is a perfect square */
static int cf_sqrt(i64 D, i64 *a0, i64 *per, int cap) {
    i64 a = isqrt(D);
    *a0 = a;
    if (a * a == D) return 0;
    i64 m = 0, d = 1, x = a;
    int len = 0;
    do {
        m = d * x - m;
        d = (D - m * m) / d;
        x = (a + m) / d;
        if (len < cap) per[len] = x;
        len++;
    } while (x != 2 * a);
    return len;
}

int main(void) {
    struct { i64 p, q; } fr[] = {{415, 93}, {649, 200}, {355, 113}, {-7, 3}, {1, 1}, {0, 5}, {1000000007, 998244353}};
    for (size_t i = 0; i < sizeof fr / sizeof fr[0]; i++) {
        i64 a[64];
        int n = cf_rational(fr[i].p, fr[i].q, a, 64);
        printf("%lld/%lld = [%lld", fr[i].p, fr[i].q, a[0]);
        for (int k = 1; k < n; k++) printf("%s %lld", k == 1 ? ";" : ",", a[k]);
        printf("]\n");
        i64 num, den;
        convergent(a, n - 1, &num, &den);
        /* the last convergent reproduces the reduced fraction */
        if (num * fr[i].q != fr[i].p * den) { fprintf(stderr, "last convergent mismatch\n"); return 1; }
    }
    /* convergents of pi's decimal approximation 3.14159265358979 */
    i64 a[30];
    int n = cf_rational(314159265358979ll, 100000000000000ll, a, 30);
    printf("pi expansion:");
    for (int k = 0; k < 10 && k < n; k++) printf(" %lld", a[k]);
    printf("\nconvergents:");
    for (int k = 0; k < 6; k++) {
        i64 h, q;
        convergent(a, k, &h, &q);
        printf(" %lld/%lld", h, q);
    }
    printf("\n");
    /* quadratic surds */
    i64 Ds[] = {2, 3, 7, 13, 23, 61, 97, 114, 4};
    for (size_t i = 0; i < sizeof Ds / sizeof Ds[0]; i++) {
        i64 a0, per[64];
        int len = cf_sqrt(Ds[i], &a0, per, 64);
        printf("sqrt(%lld) = [%lld", Ds[i], a0);
        if (len) { printf("; ("); for (int k = 0; k < len && k < 12; k++) printf(k ? " %lld" : "%lld", per[k]); printf("%s)]", len > 12 ? " ..." : ""); }
        else printf("]");
        printf("  period %d\n", len);
    }
    /* golden ratio: all ones, convergents are Fibonacci ratios */
    i64 ones[20];
    for (int i = 0; i < 20; i++) ones[i] = 1;
    i64 h, q;
    convergent(ones, 15, &h, &q);
    printf("golden convergent 15: %lld/%lld = %.9f\n", h, q, (double)h / (double)q);
    /* best rational approximation with denominator bound: brute-force check */
    double target = 2.718281828459045;
    i64 e_cf[20] = {2, 1, 2, 1, 1, 4, 1, 1, 6, 1, 1, 8, 1, 1, 10, 1, 1, 12, 1, 1};
    for (int bound = 10; bound <= 10000; bound *= 10) {
        i64 bestn = 0, bestd = 1;
        double bestErr = 1e9;
        for (i64 d = 1; d <= bound; d++) {
            i64 nn = (i64)floor(target * (double)d + 0.5);
            double err = fabs(target - (double)nn / (double)d);
            if (err < bestErr - 1e-15) { bestErr = err; bestn = nn; bestd = d; }
        }
        /* largest convergent with denominator <= bound */
        i64 cn = 0, cd = 1;
        for (int k = 0; k < 20; k++) {
            i64 hh, kk;
            convergent(e_cf, k, &hh, &kk);
            if (kk > bound) break;
            cn = hh; cd = kk;
        }
        printf("e with denominator <= %d: brute %lld/%lld, convergent %lld/%lld\n", bound, bestn, bestd, cn, cd);
        if (bestn != cn || bestd != cd) { fprintf(stderr, "best approx mismatch\n"); return 1; }
    }
    /* Euclid step count for consecutive Fibonacci is worst-case */
    i64 f1 = 1, f2 = 1;
    for (int i = 0; i < 40; i++) { i64 t = f1 + f2; f1 = f2; f2 = t; }
    i64 aa[80];
    printf("expansion length of F(42)/F(41): %d\n", cf_rational(f2, f1, aa, 80));
    return 0;
}
