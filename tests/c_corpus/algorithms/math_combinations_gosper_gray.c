/*
 * title: Combination generation with Gosper's hack and revolving door order
 * topic: algorithms
 * covers: bitmask combinations, Gosper's hack, lexicographic index sets, combinadic rank, revolving-door Gray order, popcount
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;

static u64 binom[40][40];

static int popcount(unsigned x) { int c = 0; while (x) { x &= x - 1; c++; } return c; }

/* next integer with the same popcount */
static unsigned gosper(unsigned x) {
    unsigned c = x & (0u - x);
    unsigned r = x + c;
    return (((r ^ x) >> 2) / c) | r;
}

static void show_mask(unsigned m, int n) {
    for (int i = 0; i < n; i++) putchar((m >> i) & 1 ? '1' : '0');
}

/* combinadic rank of mask among all k-subsets in Gosper (colex) order */
static u64 colex_rank(unsigned m) {
    u64 r = 0;
    int j = 0;
    for (int i = 0; i < 32; i++) if ((m >> i) & 1) { j++; r += binom[i][j]; }
    return r;
}

static unsigned colex_unrank(u64 r, int k) {
    unsigned m = 0;
    for (int j = k; j >= 1; j--) {
        int i = j - 1;
        while (binom[i + 1][j] <= r) i++;
        m |= 1u << i;
        r -= binom[i][j];
    }
    return m;
}

/* revolving door: Gray code for combinations, i-th subset is derived from Gray code of colex order */
static unsigned revolving(u64 idx, int k, int n) {
    /* recursive definition: R(n,k) = R(n-1,k), then reverse(R(n-1,k-1)) with n-1 appended */
    (void)n;
    unsigned m = 0;
    int nn = n, kk = k;
    int rev = 0;
    while (kk > 0 && nn > 0) {
        u64 first = binom[nn - 1][kk];
        if (kk == nn) { for (int i = 0; i < kk; i++) m |= 1u << i; break; }
        if (idx < first && !rev) { nn--; }
        else if (rev && idx >= binom[nn - 1][kk - 1]) { idx -= binom[nn - 1][kk - 1]; nn--; }
        else if (!rev) { idx -= first; m |= 1u << (nn - 1); nn--; kk--; rev = !rev; idx = binom[nn][kk] - 1 - idx; rev = !rev; }
        else { m |= 1u << (nn - 1); nn--; kk--; idx = binom[nn][kk] - 1 - idx; }
    }
    return m;
}

int main(void) {
    for (int i = 0; i < 40; i++) {
        binom[i][0] = 1;
        for (int j = 1; j <= i; j++) binom[i][j] = binom[i - 1][j - 1] + (j <= i - 1 ? binom[i - 1][j] : 0);
    }
    int n = 6, k = 3;
    printf("C(6,3) by Gosper (bit 0 leftmost):");
    unsigned limit = 1u << n;
    int cnt = 0;
    unsigned x = (1u << k) - 1;
    while (x < limit) {
        printf(" "); show_mask(x, n);
        if (colex_rank(x) != (u64)cnt) { fprintf(stderr, "rank mismatch\n"); return 1; }
        if (colex_unrank((u64)cnt, k) != x) { fprintf(stderr, "unrank mismatch\n"); return 1; }
        x = gosper(x);
        cnt++;
    }
    printf("\ncount %d\n", cnt);
    if ((u64)cnt != binom[6][3]) return 1;

    /* all k for n=12: counts match binomials and popcount is preserved */
    n = 12;
    limit = 1u << n;
    u64 total = 0;
    for (k = 0; k <= n; k++) {
        unsigned y = (1u << k) - 1;
        u64 c = 0;
        if (k == 0) c = 1;
        else while (y < limit) { if (popcount(y) != k) return 1; c++; y = gosper(y); }
        if (c != binom[n][k]) { fprintf(stderr, "count wrong k=%d\n", k); return 1; }
        total += c;
    }
    printf("sum over k of C(12,k) = %llu = 2^12\n", total);
    if (total != 4096) return 1;

    /* sum of all 5-subsets of 0..11 elements: each element appears C(11,4) times */
    long occ[12] = {0};
    for (unsigned y = (1u << 5) - 1; y < limit; y = gosper(y))
        for (int i = 0; i < 12; i++) occ[i] += (y >> i) & 1;
    printf("element occurrences in 5-subsets of 12: %ld each (C(11,4)=%llu)\n", occ[0], binom[11][4]);
    for (int i = 0; i < 12; i++) if (occ[i] != (long)binom[11][4]) return 1;

    /* colex rank of large masks with n up to 30 */
    unsigned big = 0x2AAAAAAu;
    printf("mask %u (popcount %d) has colex rank %llu; unrank gives %u\n", big, popcount(big), colex_rank(big),
           colex_unrank(colex_rank(big), popcount(big)));
    if (colex_unrank(colex_rank(big), popcount(big)) != big) return 1;

    /* revolving door: each successive subset differs by exactly one swap */
    n = 7; k = 3;
    printf("revolving door C(7,3):");
    unsigned prev = 0;
    int ok = 1;
    unsigned char seen[128] = {0};
    for (u64 i = 0; i < binom[n][k]; i++) {
        unsigned m = revolving(i, k, n);
        if (popcount(m) != k || m >= 128 || seen[m]) ok = 0;
        else seen[m] = 1;
        if (i) { unsigned d = m ^ prev; if (popcount(d) != 2) ok = 0; }
        if (i < 8) { printf(" "); show_mask(m, n); }
        prev = m;
    }
    printf(" ...\nrevolving door single-swap and all-distinct properties hold: %s\n", ok ? "yes" : "no");
    return ok ? 0 : 1;
}
