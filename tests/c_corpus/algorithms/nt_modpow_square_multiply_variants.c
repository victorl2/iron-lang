/*
 * title: Modular exponentiation variants
 * topic: algorithms
 * covers: square and multiply, right-to-left and left-to-right, 64-bit mulmod by doubling, Fermat, Euler reduction
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef unsigned long long u64;

static u64 addmod(u64 a, u64 b, u64 m) {
    u64 s = a + b;
    if (s < a || s >= m) s -= m;
    return s;
}

/* Overflow-safe multiplication for any 64-bit modulus. */
static u64 mulmod(u64 a, u64 b, u64 m) {
    u64 r = 0;
    a %= m;
    while (b) {
        if (b & 1) r = addmod(r, a, m);
        a = addmod(a, a, m);
        b >>= 1;
    }
    return r;
}

static u64 pow_rtl(u64 b, u64 e, u64 m) {
    u64 r = 1 % m;
    b %= m;
    while (e) {
        if (e & 1) r = mulmod(r, b, m);
        b = mulmod(b, b, m);
        e >>= 1;
    }
    return r;
}

static u64 pow_ltr(u64 b, u64 e, u64 m) {
    u64 r = 1 % m;
    b %= m;
    for (int i = 63; i >= 0; i--) {
        r = mulmod(r, r, m);
        if ((e >> i) & 1) r = mulmod(r, b, m);
    }
    return r;
}

/* recursive halving */
static u64 pow_rec(u64 b, u64 e, u64 m) {
    if (e == 0) return 1 % m;
    u64 h = pow_rec(b, e / 2, m);
    u64 r = mulmod(h, h, m);
    return (e & 1) ? mulmod(r, b % m, m) : r;
}

static u64 pow_naive(u64 b, unsigned e, u64 m) {
    u64 r = 1 % m;
    for (unsigned i = 0; i < e; i++) r = mulmod(r, b, m);
    return r;
}

static u64 state = 0x9E3779B97F4A7C15ull;
static u64 next(void) {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int main(void) {
    printf("2^10 mod 1000 = %llu\n", pow_rtl(2, 10, 1000));
    printf("3^200 mod 13 = %llu\n", pow_ltr(3, 200, 13));
    printf("7^0 mod 1 = %llu\n", pow_rec(7, 0, 1));
    u64 M61 = (1ull << 61) - 1;
    printf("2^61 mod M61 = %llu\n", pow_rtl(2, 61, M61));
    printf("5^(M61-1) mod M61 = %llu (Fermat)\n", pow_rtl(5, M61 - 1, M61));
    u64 big = 18446744073709551557ull; /* largest 64-bit prime */
    printf("3^(p-1) mod largest64 prime = %llu\n", pow_ltr(3, big - 1, big));

    for (int i = 0; i < 2000; i++) {
        u64 m = next() % 1000000007ull + 1;
        u64 b = next(), e = next() % 40;
        u64 a1 = pow_rtl(b, e, m), a2 = pow_ltr(b, e, m), a3 = pow_rec(b, e, m), a4 = pow_naive(b, (unsigned)e, m);
        if (a1 != a2 || a1 != a3 || a1 != a4) { fprintf(stderr, "variant mismatch\n"); return 1; }
    }
    /* huge moduli: the three fast variants must agree and satisfy b^(e1+e2) = b^e1 * b^e2 */
    u64 acc = 0;
    for (int i = 0; i < 500; i++) {
        u64 m = next() | 1ull, b = next(), e1 = next() >> 1, e2 = next() >> 1;
        u64 lhs = pow_rtl(b, e1 + e2, m);
        u64 rhs = mulmod(pow_ltr(b, e1, m), pow_rec(b, e2, m), m);
        if (lhs != rhs) { fprintf(stderr, "law mismatch\n"); return 1; }
        acc ^= lhs;
    }
    printf("checksum of 500 large-modulus powers: %llu\n", acc);

    /* Euler: b^e mod n = b^(e mod phi) when gcd = 1; last digits of 7^(7^7) */
    u64 e77 = pow_naive(7, 7, 1000000000000ull); /* 7^7 = 823543 */
    printf("7^(7^7) mod 10^9 = %llu (via e=%llu)\n", pow_rtl(7, e77, 1000000000ull), e77);
    /* period of 10^k mod 7 */
    for (u64 k = 0; k < 8; k++) printf("10^%llu mod 7 = %llu\n", k, pow_rtl(10, k, 7));
    return 0;
}
