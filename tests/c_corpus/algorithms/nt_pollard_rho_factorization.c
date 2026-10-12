/*
 * title: Pollard rho factorization with Brent cycle detection
 * topic: algorithms
 * covers: Pollard rho, Brent, batched gcd, Miller-Rabin, recursive splitting, semiprime factoring
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef unsigned long long u64;

static u64 mulmod(u64 a, u64 b, u64 m) {
    u64 r = 0;
    a %= m;
    while (b) {
        if (b & 1) { r += a; if (r < a || r >= m) r -= m; }
        u64 d = a + a;
        if (d < a || d >= m) d -= m;
        a = d;
        b >>= 1;
    }
    return r;
}
static u64 powmod(u64 b, u64 e, u64 m) {
    u64 r = 1;
    b %= m;
    while (e) { if (e & 1) r = mulmod(r, b, m); b = mulmod(b, b, m); e >>= 1; }
    return r;
}
static u64 gcd(u64 a, u64 b) { while (b) { u64 t = a % b; a = b; b = t; } return a; }

static int is_prime(u64 n) {
    static const u64 W[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    if (n < 2) return 0;
    for (int i = 0; i < 12; i++) if (n % W[i] == 0) return n == W[i];
    u64 d = n - 1;
    int s = 0;
    while (!(d & 1)) { d >>= 1; s++; }
    for (int i = 0; i < 12; i++) {
        u64 x = powmod(W[i], d, n);
        if (x == 1 || x == n - 1) continue;
        int ok = 0;
        for (int r = 1; r < s; r++) { x = mulmod(x, x, n); if (x == n - 1) { ok = 1; break; } }
        if (!ok) return 0;
    }
    return 1;
}

static long iterations;

static u64 f(u64 x, u64 c, u64 n) { u64 y = mulmod(x, x, n) + c; return y < c || y >= n ? y - n : y; }

/* Brent's variant with batches of 64 products before each gcd */
static u64 rho(u64 n, u64 c) {
    u64 x = 2, y = 2, q = 1, g = 1, ys = 2;
    u64 r = 1;
    const u64 m = 64;
    do {
        x = y;
        for (u64 i = 0; i < r; i++) y = f(y, c, n);
        u64 k = 0;
        do {
            ys = y;
            for (u64 i = 0; i < m && i < r - k; i++) {
                y = f(y, c, n);
                iterations++;
                q = mulmod(q, x > y ? x - y : y - x, n);
            }
            g = gcd(q, n);
            k += m;
        } while (k < r && g == 1);
        r *= 2;
    } while (g == 1);
    if (g == n) {
        do {
            ys = f(ys, c, n);
            g = gcd(x > ys ? x - ys : ys - x, n);
        } while (g == 1);
    }
    return g;
}

static void split(u64 n, u64 *out, int *cnt) {
    if (n == 1) return;
    if (is_prime(n)) { out[(*cnt)++] = n; return; }
    for (u64 p = 2; p < 50; p++)
        if (n % p == 0) { split(p, out, cnt); split(n / p, out, cnt); return; }
    u64 d = n;
    for (u64 c = 1; d == n; c++) d = rho(n, c);
    split(d, out, cnt);
    split(n / d, out, cnt);
}

static void sort_u64(u64 *a, int n) {
    for (int i = 1; i < n; i++) {
        u64 x = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; }
        a[j + 1] = x;
    }
}

int main(void) {
    u64 tests[] = {
        8051ull, 1000000007ull * 998244353ull, 600851475143ull, 1ull << 40,
        1000000007ull * 1000000007ull, 4294967311ull * 4294967357ull % 1000000000000000000ull,
        999999999989ull * 1000003ull, 2ull * 3 * 5 * 7 * 11 * 13 * 17 * 19 * 23 * 29 * 31 * 37 * 41 * 43 * 47,
        18446744073709551557ull, 9223372036854775807ull, 123456789012345678ull};
    for (size_t t = 0; t < sizeof tests / sizeof tests[0]; t++) {
        u64 n = tests[t], fs[80];
        int k = 0;
        split(n, fs, &k);
        sort_u64(fs, k);
        u64 prod = 1;
        for (int i = 0; i < k; i++) {
            if (!is_prime(fs[i])) { fprintf(stderr, "non-prime factor\n"); return 1; }
            prod *= fs[i];
        }
        if (prod != n) { fprintf(stderr, "product mismatch for %llu\n", n); return 1; }
        printf("%llu =", n);
        for (int i = 0; i < k; i++) printf(" %llu", fs[i]);
        printf("\n");
    }
    /* many small composites vs trial division */
    for (u64 n = 2; n < 20000; n++) {
        u64 fs[80];
        int k = 0;
        split(n, fs, &k);
        sort_u64(fs, k);
        u64 m = n;
        int idx = 0;
        for (u64 d = 2; d * d <= m; d++)
            while (m % d == 0) { if (idx >= k || fs[idx++] != d) { fprintf(stderr, "bad %llu\n", n); return 1; } m /= d; }
        if (m > 1 && (idx >= k || fs[idx++] != m)) { fprintf(stderr, "bad tail %llu\n", n); return 1; }
        if (idx != k) { fprintf(stderr, "count bad %llu\n", n); return 1; }
    }
    printf("all n < 20000 agree with trial division\n");
    printf("rho used iterations: %s\n", iterations > 0 ? "yes" : "no");
    return 0;
}
