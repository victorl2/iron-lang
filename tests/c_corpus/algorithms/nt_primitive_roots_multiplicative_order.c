/*
 * title: Primitive roots and multiplicative order
 * topic: algorithms
 * covers: multiplicative order, primitive root search, existence n=1,2,4,p^k,2p^k, index tables, Carmichael lambda
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef long long i64;

static i64 gcd(i64 a, i64 b) { while (b) { i64 t = a % b; a = b; b = t; } return a; }
static i64 powmod(i64 b, i64 e, i64 m) {
    i64 r = 1 % m;
    b %= m;
    while (e) { if (e & 1) r = r * b % m; b = b * b % m; e >>= 1; }
    return r;
}
static i64 phi(i64 n) {
    i64 r = n;
    for (i64 p = 2; p * p <= n; p++) if (n % p == 0) { while (n % p == 0) n /= p; r -= r / p; }
    if (n > 1) r -= r / n;
    return r;
}
static int prime_factors(i64 n, i64 *ps) {
    int k = 0;
    for (i64 p = 2; p * p <= n; p++) if (n % p == 0) { ps[k++] = p; while (n % p == 0) n /= p; }
    if (n > 1) ps[k++] = n;
    return k;
}
/* order of a mod n via dividing out prime factors of phi(n) */
static i64 order(i64 a, i64 n) {
    if (gcd(a, n) != 1) return 0;
    i64 o = phi(n), ps[16];
    int k = prime_factors(o, ps);
    for (int i = 0; i < k; i++) while (o % ps[i] == 0 && powmod(a, o / ps[i], n) == 1) o /= ps[i];
    return o;
}
static i64 order_brute(i64 a, i64 n) {
    if (gcd(a, n) != 1) return 0;
    i64 v = a % n;
    for (i64 k = 1; k <= n; k++) { if (v == 1 % n) return k; v = v * a % n; }
    return 0;
}
static i64 primitive_root(i64 n) {
    if (n == 1) return 0;
    i64 ph = phi(n), ps[16];
    int k = prime_factors(ph, ps);
    for (i64 g = 1; g < n; g++) {
        if (gcd(g, n) != 1) continue;
        int ok = 1;
        for (int i = 0; i < k && ok; i++) if (powmod(g, ph / ps[i], n) == 1) ok = 0;
        if (ok) return g;
    }
    return -1;
}
static int should_have_root(i64 n) {
    if (n == 1 || n == 2 || n == 4) return 1;
    if (n % 4 == 0) return 0;
    if (n % 2 == 0) n /= 2;
    i64 ps[16];
    return prime_factors(n, ps) == 1;
}

int main(void) {
    for (i64 n = 2; n <= 400; n++) {
        for (i64 a = 1; a < n && a < 40; a++)
            if (order(a, n) != order_brute(a, n)) { fprintf(stderr, "order mismatch %lld %lld\n", a, n); return 1; }
        i64 g = primitive_root(n);
        if ((g > 0) != should_have_root(n)) { fprintf(stderr, "existence mismatch %lld\n", n); return 1; }
    }
    printf("primitive roots exist exactly for 1,2,4,p^k,2p^k (n<=400 verified)\n");
    printf("least primitive roots:");
    i64 ns[] = {3, 5, 7, 11, 13, 14, 17, 18, 19, 23, 25, 27, 41, 71, 97, 998244353};
    for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) printf(" %lld:%lld", ns[i], primitive_root(ns[i]));
    printf("\n");
    /* number of primitive roots is phi(phi(p)) */
    i64 ps[] = {7, 13, 31, 101, 257};
    for (int i = 0; i < 5; i++) {
        i64 p = ps[i], cnt = 0;
        for (i64 g = 1; g < p; g++) if (order(g, p) == p - 1) cnt++;
        printf("p=%lld primitive roots: %lld = phi(phi(p)) = %lld\n", p, cnt, phi(phi(p)));
        if (cnt != phi(phi(p))) return 1;
    }
    /* order distribution in (Z/91)* : group is not cyclic */
    i64 hist[100] = {0};
    for (i64 a = 1; a < 91; a++) if (gcd(a, 91) == 1) hist[order(a, 91)]++;
    printf("orders in Z/91*:");
    for (int k = 1; k < 100; k++) if (hist[k]) printf(" ord%d x%lld", k, hist[k]);
    printf("\n");
    /* index (discrete log) table w.r.t. generator 3 mod 17 */
    i64 idx[17], v = 1;
    for (i64 e = 0; e < 16; e++) { idx[v] = e; v = v * 3 % 17; }
    printf("ind_3 mod 17:");
    for (int a = 1; a < 17; a++) printf(" %d", (int)idx[a]);
    printf("\n");
    /* Carmichael lambda(n): max order; equals phi(n) iff cyclic */
    i64 cyc = 0;
    for (i64 n = 3; n <= 100; n++) {
        i64 lam = 0;
        for (i64 a = 1; a < n; a++) if (gcd(a, n) == 1 && order(a, n) > lam) lam = order(a, n);
        if (lam == phi(n)) cyc++;
        if (n == 15 || n == 24 || n == 91 || n == 100) printf("lambda(%lld)=%lld phi=%lld\n", n, lam, phi(n));
    }
    printf("cyclic unit groups among n in 3..100: %lld\n", cyc);
    return 0;
}
