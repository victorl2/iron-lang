/*
 * title: Integer square, cube and n-th roots
 * topic: algorithms
 * covers: bitwise digit-by-digit isqrt, Newton iteration, binary search root, integer nth root, perfect power detection, overflow-safe comparison
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;

/* digit-by-digit (base 4) square root */
static u64 isqrt_bits(u64 n) {
    u64 res = 0, bit = 1ull << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= res + bit) { n -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return res;
}

static int newton_steps;
static u64 isqrt_newton(u64 n) {
    if (n < 2) return n;
    u64 x = n, y = n / 2 + (n & 1);
    newton_steps = 0;
    while (y < x) { x = y; y = (x + n / x) / 2; newton_steps++; }
    return x;
}

/* returns 1 if a^e > n without overflowing */
static int pow_exceeds(u64 a, int e, u64 n) {
    u64 r = 1;
    for (int i = 0; i < e; i++) {
        if (a != 0 && r > n / a) return 1;
        r *= a;
    }
    return r > n;
}

static u64 iroot(u64 n, int k) {
    if (n < 2 || k == 1) return n;
    u64 lo = 1, hi = 1ull << (64 / k + 1);
    if (hi > 0xFFFFFFFFull) hi = 0xFFFFFFFFull;
    while (lo < hi) {
        u64 mid = lo + (hi - lo + 1) / 2;
        if (pow_exceeds(mid, k, n)) hi = mid - 1; else lo = mid;
    }
    return lo;
}

static u64 ipow(u64 b, int e) { u64 r = 1; while (e--) r *= b; return r; }

/* smallest e >= 2 with n = b^e, returns 0 if none */
static int perfect_power(u64 n, u64 *base) {
    for (int e = 2; e < 64; e++) {
        u64 r = iroot(n, e);
        if (r >= 2 && ipow(r, e) == n) { *base = r; return e; }
        if (r < 2) break;
    }
    return 0;
}

static u64 state = 0xC0FFEE1234567ull;
static u64 rnd(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }

int main(void) {
    u64 tests[] = {0, 1, 2, 3, 4, 15, 16, 17, 99, 100, 65535, 65536, 4294967295ull, 4294967296ull,
                   999999999999ull, 18446744065119617025ull, 18446744065119617024ull, 18446744073709551615ull};
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        u64 n = tests[i], a = isqrt_bits(n), b = isqrt_newton(n), c = iroot(n, 2);
        if (a != b || b != c) { fprintf(stderr, "isqrt disagree at %llu\n", n); return 1; }
        if (a * a > n || (a + 1 != 0x100000000ull && (a + 1) * (a + 1) <= n)) { fprintf(stderr, "isqrt bounds at %llu\n", n); return 1; }
        printf("isqrt(%llu) = %llu (newton steps %d)\n", n, a, newton_steps);
    }
    /* random agreement */
    u64 acc = 0;
    for (int i = 0; i < 20000; i++) {
        u64 raw = rnd();
        u64 n = raw >> (rnd() % 60);
        u64 a = isqrt_bits(n);
        if (a != isqrt_newton(n) || a != iroot(n, 2)) { fprintf(stderr, "random isqrt mismatch\n"); return 1; }
        acc += a;
    }
    printf("sum of 20000 random isqrt values: %llu\n", acc);
    /* cube roots and higher */
    u64 cubes[] = {0, 1, 7, 8, 26, 27, 999999999999ull, 1000000000000ull, 18446724184312856125ull};
    for (size_t i = 0; i < sizeof cubes / sizeof cubes[0]; i++)
        printf("icbrt(%llu) = %llu\n", cubes[i], iroot(cubes[i], 3));
    for (int k = 2; k <= 12; k++) {
        u64 n = 18446744073709551615ull;
        u64 r = iroot(n, k);
        if (pow_exceeds(r, k, n) || !pow_exceeds(r + 1, k, n)) { fprintf(stderr, "root %d wrong\n", k); return 1; }
        printf("floor(2^64-1)^(1/%d) = %llu\n", k, r);
    }
    /* perfect powers below 1000 */
    printf("perfect powers < 400:");
    for (u64 n = 2; n < 400; n++) {
        u64 b;
        int e = perfect_power(n, &b);
        if (e) printf(" %llu=%llu^%d", n, b, e);
    }
    printf("\n");
    u64 big = ipow(3, 40);
    u64 base;
    int e = perfect_power(big, &base);
    printf("3^40 detected as %llu^%d\n", base, e);
    /* is-square test through isqrt for 1..100000 */
    int squares = 0;
    for (u64 n = 1; n <= 100000; n++) { u64 r = isqrt_bits(n); squares += r * r == n; }
    printf("squares up to 100000: %d\n", squares);
    return squares == 316 ? 0 : 1;
}
