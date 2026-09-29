/*
 * title: Bitmask DP assignment problem
 * topic: algorithms
 * covers: dynamic programming, bitmask over assigned jobs, popcount as row index, permutation brute force, count of optimal assignments
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

#define N 9

static int cost[N][N];

static int popcount(unsigned x) { int c = 0; while (x) { x &= x - 1; c++; } return c; }

static int best_perm, n_opt;
static void perm(int row, unsigned used, int acc, int n) {
    if (acc > best_perm) return;
    if (row == n) {
        if (acc < best_perm) { best_perm = acc; n_opt = 1; } else n_opt++;
        return;
    }
    for (int j = 0; j < n; j++)
        if (!(used >> j & 1)) perm(row + 1, used | 1u << j, acc + cost[row][j], n);
}

int main(void) {
    for (int t = 0; t < 6; t++) {
        int n = 4 + t, range = t % 2 ? 6 : 50;
        for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) cost[i][j] = 1 + rr(range);
        int full = (1 << n) - 1;
        static int dp[1 << N], ways[1 << N];
        for (int m = 0; m <= full; m++) { dp[m] = 1 << 28; ways[m] = 0; }
        dp[0] = 0; ways[0] = 1;
        for (int m = 0; m < full; m++) {
            if (dp[m] >= 1 << 28) continue;
            int row = popcount((unsigned)m);
            for (int j = 0; j < n; j++) {
                if (m >> j & 1) continue;
                int nm = m | 1 << j, c = dp[m] + cost[row][j];
                if (c < dp[nm]) { dp[nm] = c; ways[nm] = ways[m]; }
                else if (c == dp[nm]) ways[nm] += ways[m];
            }
        }
        /* reconstruct one optimal assignment by walking back */
        int assign[N], m = full;
        for (int row = n - 1; row >= 0; row--) {
            for (int j = 0; j < n; j++) {
                if (!(m >> j & 1)) continue;
                int pm = m ^ (1 << j);
                if (dp[pm] + cost[row][j] == dp[m]) { assign[row] = j; m = pm; break; }
            }
        }
        best_perm = 1 << 28; n_opt = 0;
        perm(0, 0, 0, n);
        CHECK(best_perm == dp[full] && n_opt == ways[full]);
        int chk = 0;
        for (int i = 0; i < n; i++) chk += cost[i][assign[i]];
        CHECK(chk == dp[full]);
        printf("n=%d optimum=%d optimal_assignments=%d pick=", n, dp[full], ways[full]);
        for (int i = 0; i < n; i++) printf("%d", assign[i]);
        printf("\n");
    }
    return 0;
}
