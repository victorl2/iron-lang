/*
 * title: Lattice path DP: Dyck paths, Catalan, ballot and Motzkin numbers
 * topic: algorithms
 * covers: dynamic programming, lattice paths, Dyck paths, reflection principle, Catalan recurrence, Motzkin numbers, Narayana refinement, path enumeration cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 88172645463325252ULL;
static unsigned long long rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static int rr(int n) { return (int)(rnd() % (unsigned long long)n); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); exit(1); } } while (0)

typedef unsigned long long u64;
#define N 30

static u64 binom(int n, int k) {
    if (k < 0 || k > n) return 0;
    u64 r = 1;
    for (int i = 1; i <= k; i++) r = r * (u64)(n - k + i) / (u64)i;
    return r;
}

static long enumerate_dyck(int len, int pos, int height) {
    if (height < 0) return 0;
    if (pos == len) return height == 0;
    if (height > len - pos) return 0;
    return enumerate_dyck(len, pos + 1, height + 1) + enumerate_dyck(len, pos + 1, height - 1);
}

int main(void) {
    (void)rr;
    /* paths of 2n steps staying at height >= 0: ways[step][height] */
    static u64 ways[2 * N + 1][N + 2];
    ways[0][0] = 1;
    for (int s = 0; s < 2 * N; s++)
        for (int h = 0; h <= N; h++) {
            if (!ways[s][h]) continue;
            ways[s + 1][h + 1] += ways[s][h];
            if (h > 0) ways[s + 1][h - 1] += ways[s][h];
        }
    u64 cat[N + 1];
    cat[0] = 1;
    for (int n = 1; n <= N; n++) { cat[n] = 0; for (int i = 0; i < n; i++) cat[n] += cat[i] * cat[n - 1 - i]; }
    printf("Catalan:");
    for (int n = 0; n <= 15; n++) {
        CHECK(ways[2 * n][0] == cat[n]);
        CHECK(cat[n] == binom(2 * n, n) - binom(2 * n, n + 1)); /* reflection principle */
        if (n <= 10) CHECK((u64)enumerate_dyck(2 * n, 0, 0) == cat[n]);
        printf(" %llu", cat[n]);
    }
    printf("\nC(30)=%llu\n", cat[30]);
    /* ballot numbers: paths from 0 to height h in s steps never below 0 = (h+1)/(s+1) * C(s+1, (s-h)/2) */
    printf("ballot (steps=12):");
    for (int h = 0; h <= 12; h += 2) {
        u64 closed = binom(13, (12 - h) / 2) * (u64)(h + 1) / 13;
        CHECK(ways[12][h] == closed);
        printf(" h%d=%llu", h, ways[12][h]);
    }
    printf("\n");
    /* Motzkin numbers: allow flat steps */
    static u64 mot[N + 1][N + 2];
    mot[0][0] = 1;
    for (int s = 0; s < N; s++)
        for (int h = 0; h <= N; h++) {
            if (!mot[s][h]) continue;
            mot[s + 1][h + 1] += mot[s][h];
            mot[s + 1][h] += mot[s][h];
            if (h > 0) mot[s + 1][h - 1] += mot[s][h];
        }
    printf("Motzkin:");
    for (int n = 0; n <= 20; n++) printf(" %llu", mot[n][0]);
    printf("\n");
    /* Narayana: Dyck paths of semilength n with exactly k peaks = C(n,k)C(n,k-1)/n; rows sum to Catalan */
    for (int n = 1; n <= 12; n++) {
        u64 s = 0;
        for (int k = 1; k <= n; k++) s += binom(n, k) * binom(n, k - 1) / (u64)n;
        CHECK(s == cat[n]);
    }
    printf("Narayana row 6:");
    for (int k = 1; k <= 6; k++) printf(" %llu", binom(6, k) * binom(6, k - 1) / 6);
    printf("\n");
    return 0;
}
