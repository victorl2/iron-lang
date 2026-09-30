/*
 * title: DP on grids: obstacle path counting, min-cost paths, and cherry pickup
 * topic: algorithms
 * covers: dynamic programming, grid DP, modular path counts, blocked cells, min cost path reconstruction, two-walker simultaneous DP
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

#define R 7
#define C 8

static int blocked[R][C], gain[R][C];

int main(void) {
    for (int t = 0; t < 5; t++) {
        for (int i = 0; i < R; i++)
            for (int j = 0; j < C; j++) {
                blocked[i][j] = rr(100) < 18 && !(i == 0 && j == 0) && !(i == R - 1 && j == C - 1);
                gain[i][j] = blocked[i][j] ? 0 : rr(10);
            }
        long paths[R][C];
        int cost[R][C];
        for (int i = 0; i < R; i++)
            for (int j = 0; j < C; j++) {
                if (blocked[i][j]) { paths[i][j] = 0; cost[i][j] = -1; continue; }
                paths[i][j] = (i == 0 && j == 0) ? 1 : 0;
                cost[i][j] = (i == 0 && j == 0) ? gain[0][0] : -1;
                if (i > 0) {
                    paths[i][j] += paths[i - 1][j];
                    if (cost[i - 1][j] >= 0 && (cost[i][j] < 0 || cost[i - 1][j] + gain[i][j] < cost[i][j]))
                        cost[i][j] = cost[i - 1][j] + gain[i][j];
                }
                if (j > 0) {
                    paths[i][j] += paths[i][j - 1];
                    if (cost[i][j - 1] >= 0 && (cost[i][j] < 0 || cost[i][j - 1] + gain[i][j] < cost[i][j]))
                        cost[i][j] = cost[i][j - 1] + gain[i][j];
                }
            }
        /* brute-force count by DFS */
        long dfs_cnt = 0;
        int ci[R + C], cj[R + C], depth = 0, stack_dir[R + C];
        ci[0] = 0; cj[0] = 0; stack_dir[0] = 0;
        if (!blocked[0][0]) {
            while (depth >= 0) {
                int i = ci[depth], j = cj[depth];
                if (i == R - 1 && j == C - 1) { dfs_cnt++; depth--; continue; }
                int d = stack_dir[depth]++;
                if (d >= 2) { depth--; continue; }
                int ni = i + (d == 0), nj = j + (d == 1);
                if (ni < R && nj < C && !blocked[ni][nj]) {
                    depth++; ci[depth] = ni; cj[depth] = nj; stack_dir[depth] = 0;
                }
            }
        }
        CHECK(dfs_cnt == paths[R - 1][C - 1]);
        /* two walkers: max total gain, both go corner to corner, shared cells counted once */
        static int cp[R + C - 1][R][R];
        int diag = R + C - 2, best2 = -1;
        for (int d = 0; d <= diag; d++) for (int a = 0; a < R; a++) for (int b = 0; b < R; b++) cp[d][a][b] = -1;
        cp[0][0][0] = gain[0][0];
        for (int d = 1; d <= diag; d++)
            for (int a = 0; a < R; a++)
                for (int b = 0; b < R; b++) {
                    int ja = d - a, jb = d - b;
                    if (ja < 0 || ja >= C || jb < 0 || jb >= C || blocked[a][ja] || blocked[b][jb]) continue;
                    int m = -1;
                    for (int da = 0; da < 2; da++)
                        for (int db = 0; db < 2; db++) {
                            int pa = a - da, pb = b - db;
                            if (pa < 0 || pb < 0) continue;
                            if (cp[d - 1][pa][pb] > m) m = cp[d - 1][pa][pb];
                        }
                    if (m < 0) continue;
                    cp[d][a][b] = m + gain[a][ja] + (a != b ? gain[b][jb] : 0);
                }
        best2 = cp[diag][R - 1][R - 1];
        printf("grid %d: paths=%ld mincost=%d two_walkers=%d\n", t, paths[R - 1][C - 1], cost[R - 1][C - 1], best2);
    }
    return 0;
}
