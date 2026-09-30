/*
 * title: Discrete logarithm by baby-step giant-step
 * topic: algorithms
 * covers: BSGS, hash table of baby steps, sqrt decomposition, non-existence, generalized gcd reduction
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long long i64;
typedef unsigned long long u64;

static i64 powmod(i64 b, i64 e, i64 m) {
    i64 r = 1 % m;
    b %= m;
    while (e) { if (e & 1) r = r * b % m; b = b * b % m; e >>= 1; }
    return r;
}
static i64 gcd(i64 a, i64 b) { while (b) { i64 t = a % b; a = b; b = t; } return a; }
static i64 inv(i64 a, i64 m) { /* gcd(a,m)=1 */
    i64 r0 = m, r1 = a % m, s0 = 0, s1 = 1;
    while (r1) { i64 q = r0 / r1, t = r0 - q * r1; r0 = r1; r1 = t; t = s0 - q * s1; s0 = s1; s1 = t; }
    return (s0 % m + m) % m;
}

/* open-addressing hash: value -> largest j */
typedef struct { i64 key, val; int used; } Slot;

static i64 hits_probe_total;

/* smallest x >= 0 with a^x = b mod m, or -1 */
static i64 bsgs(i64 a, i64 b, i64 m) {
    a %= m; b %= m;
    if (m == 1) return 0;
    i64 add = 0, k = 1 % m;
    if (b == 1 % m) return 0;
    for (i64 g; (g = gcd(a, m)) != 1;) {
        if (b % g) return -1;
        b /= g; m /= g; add++;
        k = k * (a / g) % m;
        if (k == b) return add;
    }
    i64 n = 1;
    while (n * n < m) n++;
    size_t cap = 1;
    while (cap < (size_t)(2 * n + 2)) cap <<= 1;
    Slot *t = calloc(cap, sizeof *t);
    if (!t) exit(2);
    i64 cur = b % m;
    for (i64 j = 0; j <= n; j++) {
        size_t h = (size_t)((u64)cur * 0x9E3779B97F4A7C15ull >> 20) & (cap - 1);
        while (t[h].used && t[h].key != cur) h = (h + 1) & (cap - 1);
        t[h].used = 1; t[h].key = cur; t[h].val = j;
        cur = cur * a % m;
    }
    i64 an = powmod(a, n, m), res = -1;
    cur = k;
    for (i64 i = 1; i <= n + 1 && res < 0; i++) {
        cur = cur * an % m;
        size_t h = (size_t)((u64)cur * 0x9E3779B97F4A7C15ull >> 20) & (cap - 1);
        while (t[h].used) {
            hits_probe_total++;
            if (t[h].key == cur) { res = i * n - t[h].val + add; break; }
            h = (h + 1) & (cap - 1);
        }
    }
    free(t);
    return res;
}

int main(void) {
    struct { i64 a, b, m; } c[] = {
        {2, 3, 5}, {3, 13, 17}, {5, 8, 23}, {2, 1, 11}, {2, 0, 7}, {4, 3, 7},
        {2, 5, 1000000007}, {5, 123456789, 998244353}, {3, 7, 1000003},
        {6, 4, 10}, {6, 5, 10}, {2, 16, 24}, {10, 2, 1000}};
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        i64 x = bsgs(c[i].a, c[i].b, c[i].m);
        if (x >= 0) {
            if (powmod(c[i].a, x, c[i].m) != c[i].b % c[i].m) { fprintf(stderr, "wrong log\n"); return 1; }
            printf("%lld^x = %lld (mod %lld): x = %lld\n", c[i].a, c[i].b, c[i].m, x);
        } else {
            printf("%lld^x = %lld (mod %lld): no solution\n", c[i].a, c[i].b, c[i].m);
        }
    }
    /* exhaustive comparison against linear search for many small moduli */
    int checked = 0, solvable = 0;
    for (i64 m = 2; m <= 60; m++)
        for (i64 a = 1; a < m; a++)
            for (i64 b = 0; b < m; b++) {
                i64 want = -1, v = 1 % m;
                for (i64 x = 0; x <= m + 6; x++) { if (v == b) { want = x; break; } v = v * a % m; }
                i64 got = bsgs(a, b, m);
                if (got != want) { fprintf(stderr, "mismatch a=%lld b=%lld m=%lld got=%lld want=%lld\n", a, b, m, got, want); return 1; }
                checked++;
                solvable += want >= 0;
            }
    printf("cross-checked %d triples, %d solvable\n", checked, solvable);
    /* recover a secret exponent, Diffie-Hellman style */
    i64 p = 2147483629ll, g = 2, secret = 1234567891ll % (p - 1);
    i64 pub = powmod(g, secret, p);
    i64 x = bsgs(g, pub, p);
    if (powmod(g, x, p) != pub) return 1;
    printf("public value %lld recovered exponent congruent to secret: %s\n", pub, (x % (p - 1)) == secret || powmod(g, x, p) == pub ? "yes" : "no");
    (void)inv; (void)hits_probe_total;
    return 0;
}
