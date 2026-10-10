/*
 * title: Permutation ranking with Lehmer codes and factorial base
 * topic: algorithms
 * covers: Lehmer code, factorial number system, rank and unrank, inversion table, permutation composition and inverse, cycle count
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;

static u64 fact[21];

static u64 rank_perm(const int *p, int n) {
    u64 r = 0;
    for (int i = 0; i < n; i++) {
        int smaller = 0;
        for (int j = i + 1; j < n; j++) smaller += p[j] < p[i];
        r += (u64)smaller * fact[n - 1 - i];
    }
    return r;
}

static void unrank_perm(u64 r, int n, int *p) {
    int pool[20];
    for (int i = 0; i < n; i++) pool[i] = i;
    int left = n;
    for (int i = 0; i < n; i++) {
        u64 f = fact[n - 1 - i];
        int k = (int)(r / f);
        r %= f;
        p[i] = pool[k];
        memmove(pool + k, pool + k + 1, (size_t)(left - k - 1) * sizeof *pool);
        left--;
    }
}

static void lehmer(const int *p, int n, int *code) {
    for (int i = 0; i < n; i++) {
        code[i] = 0;
        for (int j = i + 1; j < n; j++) code[i] += p[j] < p[i];
    }
}

static void inverse(const int *p, int n, int *q) { for (int i = 0; i < n; i++) q[p[i]] = i; }
static void compose(const int *a, const int *b, int n, int *r) { for (int i = 0; i < n; i++) r[i] = a[b[i]]; }

static int cycles(const int *p, int n) {
    int seen[20] = {0}, c = 0;
    for (int i = 0; i < n; i++) {
        if (seen[i]) continue;
        c++;
        for (int j = i; !seen[j]; j = p[j]) seen[j] = 1;
    }
    return c;
}

static int inversions(const int *code, int n) { int s = 0; for (int i = 0; i < n; i++) s += code[i]; return s; }

static u64 st = 424242;
static u64 rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }

int main(void) {
    fact[0] = 1;
    for (int i = 1; i <= 20; i++) fact[i] = fact[i - 1] * (u64)i;
    int p[20], q[20], code[20];
    printf("rank table for n=4:\n");
    for (u64 r = 0; r < 24; r += 5) {
        unrank_perm(r, 4, p);
        lehmer(p, 4, code);
        printf("  rank %2llu -> %d%d%d%d lehmer %d%d%d%d\n", r, p[0], p[1], p[2], p[3], code[0], code[1], code[2], code[3]);
    }
    /* full round trip for n=7, and unrank order must be strictly lexicographic */
    int prev[20];
    for (u64 r = 0; r < fact[7]; r++) {
        unrank_perm(r, 7, p);
        if (rank_perm(p, 7) != r) { fprintf(stderr, "roundtrip fails at %llu\n", r); return 1; }
        if (r) {
            int lt = 0;
            for (int i = 0; i < 7; i++) if (prev[i] != p[i]) { lt = prev[i] < p[i]; break; }
            if (!lt) { fprintf(stderr, "order fails at %llu\n", r); return 1; }
        }
        memcpy(prev, p, sizeof prev);
    }
    printf("n=7: all %llu ranks round-trip in lexicographic order\n", fact[7]);
    /* n=20 random ranks */
    u64 acc = 0;
    for (int t = 0; t < 200; t++) {
        u64 r = rnd() % fact[20];
        unrank_perm(r, 20, p);
        if (rank_perm(p, 20) != r) { fprintf(stderr, "n=20 fails\n"); return 1; }
        acc ^= r;
    }
    printf("n=20 200 random ranks OK, xor %llu\n", acc);
    unrank_perm(fact[20] - 1, 20, p);
    printf("last permutation of 20 starts %d %d %d ... ends %d\n", p[0], p[1], p[2], p[19]);
    /* group laws on random permutations of 10 */
    int cyc_hist[11] = {0};
    for (int t = 0; t < 2000; t++) {
        int a[10], b[10], ia[10], id[10], ab[10];
        unrank_perm(rnd() % fact[10], 10, a);
        unrank_perm(rnd() % fact[10], 10, b);
        inverse(a, 10, ia);
        compose(a, ia, 10, id);
        for (int i = 0; i < 10; i++) if (id[i] != i) { fprintf(stderr, "inverse law\n"); return 1; }
        compose(a, b, 10, ab);
        int iab[10], ib[10], ibia[10];
        inverse(ab, 10, iab);
        inverse(b, 10, ib);
        compose(ib, ia, 10, ibia);
        if (memcmp(iab, ibia, sizeof iab) != 0) { fprintf(stderr, "anti-hom fails\n"); return 1; }
        cyc_hist[cycles(a, 10)]++;
        /* Lehmer code inversion total equals pair count and is preserved by inverse */
        int c1[10], c2[10];
        lehmer(a, 10, c1);
        lehmer(ia, 10, c2);
        if (inversions(c1, 10) != inversions(c2, 10)) { fprintf(stderr, "inversion count differs\n"); return 1; }
    }
    printf("cycle count histogram over 2000 random perms of 10:");
    for (int i = 1; i <= 10; i++) printf(" %d", cyc_hist[i]);
    printf("\n");
    (void)q;
    return 0;
}
