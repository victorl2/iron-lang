/*
 * title: Modular square roots with Tonelli-Shanks
 * topic: algorithms
 * covers: Legendre symbol, Euler criterion, Tonelli-Shanks, quadratic residues, p = 3 mod 4 shortcut, Cipolla-free cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef long long i64;

static i64 powmod(i64 b, i64 e, i64 m) {
    i64 r = 1;
    b %= m; if (b < 0) b += m;
    while (e) { if (e & 1) r = r * b % m; b = b * b % m; e >>= 1; }
    return r;
}
static int legendre(i64 a, i64 p) {
    i64 r = powmod(a, (p - 1) / 2, p);
    return r == 0 ? 0 : (r == 1 ? 1 : -1);
}

/* returns a root r (the smaller of r, p-r) or -1 if none */
static i64 tonelli(i64 a, i64 p, int *loops) {
    a %= p; if (a < 0) a += p;
    if (a == 0) return 0;
    if (p == 2) return a;
    if (legendre(a, p) != 1) return -1;
    i64 q = p - 1;
    int s = 0;
    while (!(q & 1)) { q >>= 1; s++; }
    if (s == 1) { i64 r = powmod(a, (p + 1) / 4, p); return r < p - r ? r : p - r; }
    i64 z = 2;
    while (legendre(z, p) != -1) z++;
    i64 c = powmod(z, q, p), t = powmod(a, q, p), r = powmod(a, (q + 1) / 2, p);
    int m = s;
    while (t != 1) {
        int i = 0;
        i64 tt = t;
        while (tt != 1) { tt = tt * tt % p; i++; }
        i64 b = c;
        for (int j = 0; j < m - i - 1; j++) b = b * b % p;
        m = i;
        c = b * b % p;
        t = t * c % p;
        r = r * b % p;
        (*loops)++;
    }
    return r < p - r ? r : p - r;
}

int main(void) {
    i64 primes[] = {3, 5, 7, 13, 17, 41, 97, 113, 257, 7681, 65537, 998244353, 1000000007};
    int loops = 0;
    for (size_t k = 0; k < sizeof primes / sizeof primes[0]; k++) {
        i64 p = primes[k];
        int s = 0;
        for (i64 q = p - 1; !(q & 1); q >>= 1) s++;
        if (p < 300) {
            int residues = 0;
            for (i64 a = 0; a < p; a++) {
                i64 r = tonelli(a, p, &loops);
                int brute = 0;
                for (i64 x = 0; x < p; x++) if (x * x % p == a) { brute = 1; break; }
                if ((r >= 0) != brute) { fprintf(stderr, "existence mismatch p=%lld a=%lld\n", p, a); return 1; }
                if (r >= 0) { if (r * r % p != a) { fprintf(stderr, "bad root\n"); return 1; } residues++; }
            }
            printf("p=%lld (2-adic valuation of p-1 = %d): %d square classes incl. 0\n", p, s, residues);
        } else {
            i64 a[] = {2, 3, 5, 10, 123456, 99999989};
            printf("p=%lld (s=%d):", p, s);
            for (int i = 0; i < 6; i++) {
                i64 r = tonelli(a[i], p, &loops);
                if (r >= 0 && r * r % p != a[i] % p) { fprintf(stderr, "bad big root\n"); return 1; }
                if (r >= 0) printf(" sqrt(%lld)=%lld", a[i], r); else printf(" %lld:nonresidue", a[i]);
            }
            printf("\n");
        }
    }
    printf("tonelli inner iterations recorded: %s\n", loops > 0 ? "some" : "none");
    /* density of quadratic residues among 1..p-1 is exactly one half */
    i64 p = 10007;
    int res = 0;
    for (i64 a = 1; a < p; a++) res += legendre(a, p) == 1;
    printf("quadratic residues mod %lld: %d of %lld\n", p, res, p - 1);
    /* quadratic reciprocity check over small odd primes */
    int checked = 0;
    i64 sp[] = {3, 5, 7, 11, 13, 17, 19, 23, 29, 31};
    for (int i = 0; i < 10; i++) for (int j = i + 1; j < 10; j++) {
        int a = legendre(sp[i], sp[j]), b = legendre(sp[j], sp[i]);
        int sign = ((sp[i] - 1) / 2 * ((sp[j] - 1) / 2)) & 1 ? -1 : 1;
        if (a * b != sign) { fprintf(stderr, "reciprocity fails\n"); return 1; }
        checked++;
    }
    printf("quadratic reciprocity verified for %d prime pairs\n", checked);
    return 0;
}
