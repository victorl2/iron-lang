/*
 * title: Burnside counting of necklaces and bracelets
 * topic: algorithms
 * covers: Burnside lemma, cyclic and dihedral group actions, totient sums, canonical form brute force, orbit enumeration, Lyndon word counts
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;

static u64 gcd(u64 a, u64 b) { while (b) { u64 t = a % b; a = b; b = t; } return a; }
static u64 ipow(u64 b, u64 e) { u64 r = 1; while (e--) r *= b; return r; }
static u64 phi(u64 n) {
    u64 r = n;
    for (u64 p = 2; p * p <= n; p++) if (n % p == 0) { while (n % p == 0) n /= p; r -= r / p; }
    if (n > 1) r -= r / n;
    return r;
}
static int mobius(u64 n) {
    int r = 1;
    for (u64 p = 2; p * p <= n; p++) {
        if (n % p) continue;
        n /= p;
        if (n % p == 0) return 0;
        r = -r;
    }
    return n > 1 ? -r : r;
}

/* Burnside with the cyclic group: (1/n) sum_{d|n} phi(n/d) k^d */
static u64 necklaces(int n, int k) {
    u64 s = 0;
    for (int d = 1; d <= n; d++) if (n % d == 0) s += phi((u64)(n / d)) * ipow((u64)k, (u64)d);
    return s / (u64)n;
}
static u64 necklaces_gcd_form(int n, int k) {
    u64 s = 0;
    for (int i = 0; i < n; i++) s += ipow((u64)k, gcd((u64)i, (u64)n));
    return s / (u64)n;
}
/* dihedral group of order 2n: bracelets */
static u64 bracelets(int n, int k) {
    u64 s = necklaces(n, k) * (u64)n; /* rotations */
    u64 refl = 0;
    if (n & 1) refl = (u64)n * ipow((u64)k, (u64)(n + 1) / 2);
    else refl = (u64)(n / 2) * (ipow((u64)k, (u64)n / 2 + 1) + ipow((u64)k, (u64)n / 2));
    return (s + refl) / (2 * (u64)n);
}
/* Lyndon words = aperiodic necklaces = (1/n) sum mu(d) k^(n/d) */
static u64 lyndon(int n, int k) {
    long long s = 0;
    for (int d = 1; d <= n; d++) if (n % d == 0) s += mobius((u64)d) * (long long)ipow((u64)k, (u64)(n / d));
    return (u64)s / (u64)n;
}

/* brute force: canonical minimal rotation (and reflection) over all k^n strings */
static u64 brute(int n, int k, int allow_reflect) {
    u64 total = ipow((u64)k, (u64)n), classes = 0;
    int a[16];
    for (u64 code = 0; code < total; code++) {
        u64 c = code;
        for (int i = 0; i < n; i++) { a[i] = (int)(c % (u64)k); c /= (u64)k; }
        int canonical = 1;
        for (int r = 0; r < n && canonical; r++) {
            for (int refl = 0; refl <= allow_reflect && canonical; refl++) {
                int cmp = 0;
                for (int i = 0; i < n; i++) {
                    int idx = refl ? (r - i + 2 * n) % n : (r + i) % n;
                    if (a[idx] != a[i]) { cmp = a[idx] < a[i] ? -1 : 1; break; }
                }
                if (cmp < 0) canonical = 0;
            }
        }
        classes += (u64)canonical;
    }
    return classes;
}

int main(void) {
    printf("necklaces N(n,k):\n     ");
    for (int k = 1; k <= 5; k++) printf("%9d", k);
    printf("\n");
    for (int n = 1; n <= 10; n++) {
        printf("n=%2d ", n);
        for (int k = 1; k <= 5; k++) {
            u64 a = necklaces(n, k), b = necklaces_gcd_form(n, k);
            if (a != b) { fprintf(stderr, "burnside forms differ\n"); return 1; }
            printf("%9llu", a);
        }
        printf("\n");
    }
    for (int n = 1; n <= 9; n++)
        for (int k = 1; k <= 3; k++) {
            if (ipow((u64)k, (u64)n) > 20000) continue;
            if (brute(n, k, 0) != necklaces(n, k)) { fprintf(stderr, "necklace brute mismatch n=%d k=%d\n", n, k); return 1; }
            if (brute(n, k, 1) != bracelets(n, k)) { fprintf(stderr, "bracelet brute mismatch n=%d k=%d\n", n, k); return 1; }
        }
    printf("brute-force orbit counts verified for k^n <= 20000\n");
    printf("bracelets B(n,2), n=1..16:");
    for (int n = 1; n <= 16; n++) printf(" %llu", bracelets(n, 2));
    printf("\nbracelets B(n,3), n=1..10:");
    for (int n = 1; n <= 10; n++) printf(" %llu", bracelets(n, 3));
    printf("\nLyndon words L(n,2), n=1..16:");
    for (int n = 1; n <= 16; n++) printf(" %llu", lyndon(n, 2));
    printf("\n");
    /* sum over d|n of L(d,k) = N(n,k)? No: N(n,k) = sum_{d|n} L(d,k) */
    for (int n = 1; n <= 30; n++) {
        u64 s = 0;
        for (int d = 1; d <= n; d++) if (n % d == 0) s += lyndon(d, 3);
        if (s != necklaces(n, 3)) { fprintf(stderr, "lyndon sum mismatch %d\n", n); return 1; }
    }
    printf("N(n,3) = sum of L(d,3) over d|n verified for n<=30\n");
    printf("2-colour necklaces with 20 beads: %llu, bracelets: %llu\n", necklaces(20, 2), bracelets(20, 2));
    printf("4 beads, 6 colours: necklaces: %llu\n", necklaces(4, 6));
    return 0;
}
