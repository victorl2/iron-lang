/*
 * title: Lagrange interpolation and Faulhaber power sums mod p
 * topic: algorithms
 * covers: Lagrange interpolation, barycentric form, prefix/suffix products, sum of k-th powers as polynomial, finite differences, modular inverses
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef unsigned long long u64;
#define P 1000000007ull

static u64 powmod(u64 b, u64 e) {
    u64 r = 1;
    b %= P;
    while (e) { if (e & 1) r = r * b % P; b = b * b % P; e >>= 1; }
    return r;
}
static u64 inv(u64 a) { return powmod(a, P - 2); }

/* value at x of the unique polynomial through (0,y0)...(n-1,y_{n-1}); x may exceed n */
static u64 lagrange_eval(const u64 *y, int n, u64 x) {
    if (x < (u64)n) return y[x];
    u64 pre[64], suf[64], fact[64], ifact[64];
    x %= P;
    pre[0] = 1;
    for (int i = 0; i < n; i++) pre[i + 1] = pre[i] * ((x + P - (u64)i) % P) % P;
    suf[n] = 1;
    for (int i = n - 1; i >= 0; i--) suf[i] = suf[i + 1] * ((x + P - (u64)i) % P) % P;
    fact[0] = 1;
    for (int i = 1; i < n; i++) fact[i] = fact[i - 1] * (u64)i % P;
    for (int i = 0; i < n; i++) ifact[i] = inv(fact[i]);
    u64 r = 0;
    for (int i = 0; i < n; i++) {
        u64 term = y[i] * pre[i] % P * suf[i + 1] % P * ifact[i] % P * ifact[n - 1 - i] % P;
        if ((n - 1 - i) & 1) term = (P - term) % P;
        r = (r + term) % P;
    }
    return r;
}

/* sum_{i=1}^{n} i^k mod p in O(k log) via interpolation on k+2 points */
static u64 power_sum(u64 n, int k) {
    u64 y[64];
    y[0] = 0;
    for (int i = 1; i <= k + 1; i++) y[i] = (y[i - 1] + powmod((u64)i, (u64)k)) % P;
    return lagrange_eval(y, k + 2, n);
}

static u64 power_sum_naive(u64 n, int k) {
    u64 s = 0;
    for (u64 i = 1; i <= n; i++) s = (s + powmod(i, (u64)k)) % P;
    return s;
}

int main(void) {
    /* interpolate a known cubic 2x^3 - 5x^2 + x + 7 */
    u64 y[4];
    for (int i = 0; i < 4; i++) {
        long long x = i;
        long long v = 2 * x * x * x - 5 * x * x + x + 7;
        y[i] = (u64)((v % (long long)P + (long long)P) % (long long)P);
    }
    for (u64 x = 4; x <= 10; x += 3) {
        long long want = 2 * (long long)(x * x * x) - 5 * (long long)(x * x) + (long long)x + 7;
        u64 got = lagrange_eval(y, 4, x);
        printf("f(%llu) = %llu (direct %lld)\n", x, got, want);
        if ((long long)got != want) return 1;
    }
    u64 far = 1000000000000ull;
    u64 xm = far % P;
    u64 direct = (2 * powmod(xm, 3) + P - 5 * powmod(xm, 2) % P + xm + 7) % P;
    printf("f(10^12) mod p = %llu, direct %llu\n", lagrange_eval(y, 4, far), direct);
    if (lagrange_eval(y, 4, far) != direct) return 1;

    /* Faulhaber: sums of k-th powers */
    for (int k = 0; k <= 10; k++) {
        u64 n = 1000 + (u64)k;
        u64 a = power_sum(n, k), b = power_sum_naive(n, k);
        if (a != b) { fprintf(stderr, "power sum mismatch k=%d\n", k); return 1; }
    }
    printf("interpolated power sums match naive for k=0..10\n");
    for (int k = 1; k <= 6; k++) printf("sum_{i<=10^15} i^%d mod p = %llu\n", k, power_sum(1000000000000000ull, k));
    /* sum of first n cubes equals square of triangular number */
    u64 n = 123456789012ull;
    u64 t = (n % P) * ((n + 1) % P) % P * inv(2) % P;
    printf("sum of cubes to %llu equals T^2: %s\n", n, power_sum(n, 3) == t * t % P ? "yes" : "NO");
    if (power_sum(n, 3) != t * t % P) return 1;
    /* finite differences: k-th difference of a degree-k polynomial is k! * lead coefficient */
    long long v[8];
    for (int i = 0; i < 8; i++) v[i] = 3 * i * i * i * i * i + 2 * i + 1; /* degree 5, lead 3 */
    int len = 8;
    for (int d = 1; d <= 5; d++) { for (int i = 0; i + 1 < len; i++) v[i] = v[i + 1] - v[i]; len--; }
    printf("5th difference of 3x^5+2x+1: %lld (5! * 3 = %d)\n", v[0], 120 * 3);
    if (v[0] != 360) return 1;
    /* Bernoulli-flavoured check: sum i^k mod p is a polynomial of degree k+1 in n */
    u64 yy[12];
    yy[0] = 0;
    for (int i = 1; i < 12; i++) yy[i] = (yy[i - 1] + powmod((u64)i, 9)) % P;
    printf("degree-10 interpolation of sum i^9 at 10^9+6: %llu\n", lagrange_eval(yy, 11, 1000000006ull));
    return 0;
}
