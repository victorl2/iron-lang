/*
 * title: Held-Karp TSP with tour reconstruction
 * topic: algorithms
 * covers: traveling salesman, Held-Karp, bitmask DP, parent tables, tour reconstruction, permutation brute force
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAXN = 13, INF = 1 << 28 };

static unsigned st = 5050u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void check(int c, const char *m) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", m);
        exit(1);
    }
}

static int W[MAXN][MAXN];
static int dp[1 << MAXN][MAXN], parent_[1 << MAXN][MAXN];

static long held_karp(int n, int *tour) {
    unsigned full = (1u << n) - 1;
    for (unsigned m = 0; m <= full; m++)
        for (int j = 0; j < n; j++) dp[m][j] = INF, parent_[m][j] = -1;
    dp[1][0] = 0;
    for (unsigned m = 1; m <= full; m += 2) /* masks containing city 0 */
        for (int j = 0; j < n; j++) {
            if (dp[m][j] >= INF || !(m >> j & 1u)) continue;
            for (int k = 1; k < n; k++) {
                if (m >> k & 1u) continue;
                unsigned nm = m | 1u << k;
                if (dp[m][j] + W[j][k] < dp[nm][k]) dp[nm][k] = dp[m][j] + W[j][k], parent_[nm][k] = j;
            }
        }
    int best = INF, last = -1;
    for (int j = 1; j < n; j++)
        if (dp[full][j] + W[j][0] < best) best = dp[full][j] + W[j][0], last = j;
    unsigned m = full;
    for (int i = n - 1; i >= 1; i--) {
        tour[i] = last;
        int p = parent_[m][last];
        m &= ~(1u << last);
        last = p;
    }
    tour[0] = 0;
    return best;
}

static long brute_best;
static void permute(int n, int *perm, int k, long acc) {
    if (acc >= brute_best) return;
    if (k == n) {
        if (acc + W[perm[n - 1]][0] < brute_best) brute_best = acc + W[perm[n - 1]][0];
        return;
    }
    for (int i = k; i < n; i++) {
        int t = perm[k];
        perm[k] = perm[i];
        perm[i] = t;
        permute(n, perm, k + 1, acc + W[perm[k - 1]][perm[k]]);
        t = perm[k];
        perm[k] = perm[i];
        perm[i] = t;
    }
}

int main(void) {
    int sizes[5] = {5, 8, 9, 10, 13};
    for (int s = 0; s < 5; s++) {
        int n = sizes[s];
        int px[MAXN], py[MAXN];
        for (int i = 0; i < n; i++) px[i] = (int)(rnd() % 100), py[i] = (int)(rnd() % 100);
        /* asymmetric-ish weights: Manhattan distance plus a directional penalty */
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) W[i][j] = abs(px[i] - px[j]) + abs(py[i] - py[j]) + (px[j] > px[i] ? 3 : 0);
        int tour[MAXN];
        long best = held_karp(n, tour);
        long len = 0;
        unsigned seen = 0;
        for (int i = 0; i < n; i++) {
            check(!(seen >> tour[i] & 1u), "city visited once");
            seen |= 1u << tour[i];
            len += W[tour[i]][tour[(i + 1) % n]];
        }
        check(len == best, "reconstructed tour has DP length");
        if (n <= 10) {
            int perm[MAXN];
            for (int i = 0; i < n; i++) perm[i] = i;
            brute_best = 1L << 40;
            permute(n, perm, 1, 0);
            check(brute_best == best, "Held-Karp equals brute force");
        }
        printf("n=%2d: optimal tour length %ld:", n, best);
        for (int i = 0; i < n; i++) printf(" %d", tour[i]);
        printf("\n");
    }
    return 0;
}
