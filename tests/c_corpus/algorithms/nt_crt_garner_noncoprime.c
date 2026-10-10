/*
 * title: Chinese remainder theorem, Garner and non-coprime merging
 * topic: algorithms
 * covers: CRT, Garner mixed radix, congruence merging with gcd, inconsistency detection, lcm growth
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef long long i64;

static i64 egcd(i64 a, i64 b, i64 *x, i64 *y) {
    if (!b) { *x = 1; *y = 0; return a; }
    i64 x1, y1, g = egcd(b, a % b, &x1, &y1);
    *x = y1; *y = x1 - (a / b) * y1;
    return g;
}
static i64 mod(i64 a, i64 m) { a %= m; return a < 0 ? a + m : a; }
static i64 mulmod(i64 a, i64 b, i64 m) {
    i64 r = 0;
    a = mod(a, m); b = mod(b, m);
    while (b) { if (b & 1) r = (r + a) % m; a = (a + a) % m; b >>= 1; }
    return r;
}

/* merge x = r1 mod m1 and x = r2 mod m2; result (r, lcm) or fails */
static int merge(i64 r1, i64 m1, i64 r2, i64 m2, i64 *r, i64 *m) {
    i64 p, q, g = egcd(m1, m2, &p, &q);
    if ((r2 - r1) % g) return 0;
    i64 l = m1 / g * m2;
    i64 t = mulmod((r2 - r1) / g, p, m2 / g);
    *r = mod(r1 + mulmod(t, m1, l), l);
    *m = l;
    return 1;
}

/* Garner: pairwise-coprime moduli, returns value mod prod as mixed radix digits */
static i64 garner(const i64 *r, const i64 *m, int n, i64 *digits) {
    for (int i = 0; i < n; i++) {
        i64 x = r[i], mi = m[i];
        for (int j = 0; j < i; j++) {
            /* x = (x - d_j) * inv(m_j) mod m_i */
            i64 p, q;
            egcd(m[j] % mi, mi, &p, &q);
            x = mulmod(x - digits[j], p, mi);
        }
        digits[i] = mod(x, mi);
    }
    i64 v = 0, mult = 1;
    for (int i = 0; i < n; i++) { v += digits[i] * mult; mult *= m[i]; }
    return v;
}

int main(void) {
    /* classic Sunzi problem */
    i64 r[] = {2, 3, 2}, m[] = {3, 5, 7}, dg[3];
    printf("Sunzi: x = %lld (mod 105)\n", garner(r, m, 3, dg));
    printf("mixed radix digits: %lld %lld %lld\n", dg[0], dg[1], dg[2]);

    /* recover known numbers from residues with 4 coprime moduli */
    i64 mm[] = {1009, 1013, 1019, 1021};
    i64 prod = 1;
    for (int i = 0; i < 4; i++) prod *= mm[i];
    i64 xs[] = {0, 1, 123456789, 999999999999ll % prod, prod - 1};
    for (int t = 0; t < 5; t++) {
        i64 rr[4], d4[4];
        for (int i = 0; i < 4; i++) rr[i] = xs[t] % mm[i];
        i64 v = garner(rr, mm, 4, d4);
        if (v != xs[t] % prod) { fprintf(stderr, "garner mismatch\n"); return 1; }
        printf("x=%lld -> residues %lld %lld %lld %lld -> %lld\n", xs[t] % prod, rr[0], rr[1], rr[2], rr[3], v);
    }

    /* fold non-coprime congruences */
    struct { i64 r, m; } sys1[] = {{2, 6}, {5, 9}, {8, 15}};
    i64 R = 0, M = 1;
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        if (i == 0) { R = sys1[0].r; M = sys1[0].m; continue; }
        if (!merge(R, M, sys1[i].r, sys1[i].m, &R, &M)) { ok = 0; break; }
    }
    printf("system A: %s", ok ? "solvable" : "inconsistent");
    if (ok) printf(" x = %lld mod %lld", R, M);
    printf("\n");
    if (ok) for (int i = 0; i < 3; i++) if (mod(R, sys1[i].m) != sys1[i].r) return 1;

    struct { i64 r, m; } sys2[] = {{1, 4}, {2, 6}};
    ok = merge(sys2[0].r, sys2[0].m, sys2[1].r, sys2[1].m, &R, &M);
    printf("system B: %s\n", ok ? "solvable" : "inconsistent (parity clash)");

    /* brute-force cross-check with random systems */
    unsigned s = 12345;
    int solvable = 0, unsolvable = 0;
    for (int t = 0; t < 300; t++) {
        s = s * 1103515245u + 12345u;
        i64 m1 = (s >> 16) % 30 + 1;
        s = s * 1103515245u + 12345u;
        i64 m2 = (s >> 16) % 30 + 1;
        s = s * 1103515245u + 12345u;
        i64 r1 = (s >> 16) % m1;
        s = s * 1103515245u + 12345u;
        i64 r2 = (s >> 16) % m2;
        i64 rr = -1, ll = 0, bf = -1;
        for (i64 x = 0; x < m1 * m2; x++) if (x % m1 == r1 && x % m2 == r2) { bf = x; break; }
        int got = merge(r1, m1, r2, m2, &rr, &ll);
        if (got != (bf >= 0) || (got && rr != bf)) { fprintf(stderr, "random merge mismatch\n"); return 1; }
        got ? solvable++ : unsolvable++;
    }
    printf("random pairs: %d solvable, %d inconsistent\n", solvable, unsolvable);
    /* lcm growth for moduli 1..20 */
    i64 L = 1;
    for (i64 k = 2; k <= 20; k++) { i64 p, q, g = egcd(L, k, &p, &q); L = L / g * k; }
    printf("lcm(1..20) = %lld\n", L);
    return 0;
}
