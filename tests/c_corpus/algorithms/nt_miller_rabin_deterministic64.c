/*
 * title: Deterministic Miller-Rabin for 64-bit integers
 * topic: algorithms
 * covers: Miller-Rabin, strong pseudoprimes, witness sets, Carmichael numbers, mulmod by doubling
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

/* one strong-probable-prime round to base a */
static int spsp(u64 n, u64 a) {
    u64 d = n - 1;
    int s = 0;
    while (!(d & 1)) { d >>= 1; s++; }
    a %= n;
    if (a == 0) return 1;
    u64 x = powmod(a, d, n);
    if (x == 1 || x == n - 1) return 1;
    for (int i = 1; i < s; i++) {
        x = mulmod(x, x, n);
        if (x == n - 1) return 1;
    }
    return 0;
}

static const u64 WIT[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};

static int is_prime(u64 n) {
    if (n < 2) return 0;
    for (int i = 0; i < 12; i++) {
        if (n % WIT[i] == 0) return n == WIT[i];
    }
    for (int i = 0; i < 12; i++) if (!spsp(n, WIT[i])) return 0;
    return 1;
}

static int trial(u64 n) {
    if (n < 2) return 0;
    for (u64 d = 2; d * d <= n; d++) if (n % d == 0) return 0;
    return 1;
}

int main(void) {
    /* exhaustive agreement with trial division below 30000 */
    int count = 0;
    for (u64 n = 0; n < 30000; n++) {
        if (is_prime(n) != trial(n)) { fprintf(stderr, "mismatch at %llu\n", n); return 1; }
        count += is_prime(n);
    }
    printf("primes below 30000: %d\n", count);

    /* strong pseudoprimes to base 2 below 100000 that are composite */
    printf("base-2 strong pseudoprimes < 3000000:");
    int shown = 0;
    for (u64 n = 3; n < 3000000; n += 2) {
        if (spsp(n, 2) && !trial(n)) { printf(" %llu", n); shown++; }
    }
    printf(" (%d)\n", shown);

    u64 carm[] = {561, 1105, 1729, 2465, 2821, 6601, 8911, 41041, 825265};
    for (size_t i = 0; i < sizeof carm / sizeof carm[0]; i++) {
        u64 n = carm[i];
        /* Fermat base 2 passes, full MR rejects */
        printf("Carmichael %llu: fermat2=%llu mr=%d\n", n, powmod(2, n - 1, n), is_prime(n));
        if (is_prime(n)) return 1;
    }
    u64 known[] = {2147483647ull, 4294967311ull, 1000000007ull * 998244353ull,
                   (1ull << 61) - 1, 18446744073709551557ull, 18446744073709551615ull,
                   3825123056546413051ull, 3215031751ull};
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        printf("%llu -> %s\n", known[i], is_prime(known[i]) ? "prime" : "composite");
    /* 3825123056546413051 is the smallest strong pseudoprime to bases 2..23; base 29,31,37 catch it */
    int first9 = 1;
    for (int i = 0; i < 9; i++) if (!spsp(3825123056546413051ull, WIT[i])) first9 = 0;
    printf("3825123056546413051 passes bases 2..23: %d, full test: %d\n", first9, is_prime(3825123056546413051ull));
    if (!first9 || is_prime(3825123056546413051ull)) return 1;

    /* nearest primes around powers of two */
    for (int k = 20; k <= 60; k += 20) {
        u64 n = (1ull << k);
        u64 below = n - 1, above = n + 1;
        while (!is_prime(below)) below--;
        while (!is_prime(above)) above++;
        printf("around 2^%d: %llu < 2^%d < %llu\n", k, below, k, above);
    }
    return 0;
}
