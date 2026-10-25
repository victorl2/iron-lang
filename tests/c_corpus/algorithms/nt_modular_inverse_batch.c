/*
 * title: Modular inverses four ways
 * topic: algorithms
 * covers: extended Euclid inverse, Fermat inverse, linear inverse table, batch inversion with prefix products
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef long long i64;
typedef unsigned long long u64;

#define P 998244353ll

static i64 modpow(i64 b, i64 e, i64 m) {
    i64 r = 1;
    b %= m;
    while (e) {
        if (e & 1) r = r * b % m;
        b = b * b % m;
        e >>= 1;
    }
    return r;
}

/* returns inverse or -1 when gcd != 1 */
static i64 inv_egcd(i64 a, i64 m) {
    i64 g = m, x = 0, y = 1, aa = ((a % m) + m) % m;
    i64 r0 = m, r1 = aa, s0 = 0, s1 = 1;
    (void)g; (void)x; (void)y;
    while (r1) {
        i64 q = r0 / r1;
        i64 t = r0 - q * r1; r0 = r1; r1 = t;
        t = s0 - q * s1; s0 = s1; s1 = t;
    }
    if (r0 != 1) return -1;
    return ((s0 % m) + m) % m;
}

static i64 inv_fermat(i64 a, i64 p) { return modpow(a, p - 2, p); }

static u64 st = 88172645463325252ull;
static u64 rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }

int main(void) {
    /* linear-time table of 1..n inverses: inv[i] = -(p/i) * inv[p%i] */
    enum { N = 100000 };
    static i64 tab[N + 1];
    tab[1] = 1;
    for (int i = 2; i <= N; i++) tab[i] = (P - (P / i) * tab[P % i] % P) % P;
    for (int i = 1; i <= N; i += 997) {
        if (tab[i] * i % P != 1) { fprintf(stderr, "table wrong\n"); return 1; }
    }
    printf("inv(2)=%lld inv(3)=%lld inv(10)=%lld inv(100000)=%lld\n", tab[2], tab[3], tab[10], tab[N]);

    /* Compare the three single-shot methods on random values */
    i64 xor_all = 0;
    for (int i = 0; i < 3000; i++) {
        i64 a = (i64)(rnd() % (u64)(P - 1)) + 1;
        i64 x = inv_egcd(a, P), y = inv_fermat(a, P);
        if (x != y || x * a % P != 1) { fprintf(stderr, "inverse mismatch\n"); return 1; }
        xor_all ^= x;
    }
    printf("xor of 3000 inverses: %lld\n", xor_all);

    /* batch inversion: one modular inverse for the whole array */
    enum { B = 5000 };
    static i64 a[B], pre[B + 1], out[B];
    for (int i = 0; i < B; i++) a[i] = (i64)(rnd() % (u64)(P - 1)) + 1;
    pre[0] = 1;
    for (int i = 0; i < B; i++) pre[i + 1] = pre[i] * a[i] % P;
    i64 inv_all = modpow(pre[B], P - 2, P);
    for (int i = B - 1; i >= 0; i--) {
        out[i] = inv_all * pre[i] % P;
        inv_all = inv_all * a[i] % P;
    }
    i64 sum = 0;
    for (int i = 0; i < B; i++) {
        if (out[i] * a[i] % P != 1) { fprintf(stderr, "batch wrong at %d\n", i); return 1; }
        sum = (sum + out[i]) % P;
    }
    printf("batch inverse sum over %d values: %lld\n", B, sum);

    /* composite modulus: inverse exists only for coprime residues */
    i64 m = 3600;
    int have = 0, none = 0;
    for (i64 a2 = 0; a2 < m; a2++) {
        i64 x = inv_egcd(a2, m);
        if (x < 0) { none++; continue; }
        if (a2 * x % m != 1) { fprintf(stderr, "composite inverse wrong\n"); return 1; }
        have++;
    }
    printf("mod %lld: %d invertible residues, %d not (phi(3600)=960)\n", m, have, none);
    printf("inv(7) mod 3600 = %lld, inv(3600) mod 7 = %lld, inv(6) mod 9 = %lld\n",
           inv_egcd(7, 3600), inv_egcd(3600, 7), inv_egcd(6, 9));
    /* negative input normalization */
    printf("inv(-5) mod 13 = %lld\n", inv_egcd(-5, 13));
    return 0;
}
