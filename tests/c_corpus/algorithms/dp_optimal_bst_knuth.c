/*
 * title: Optimal binary search tree with Knuth optimization
 * topic: algorithms
 * covers: dynamic programming, interval DP, Knuth optimization, root monotonicity opt[i][j-1] <= opt[i][j] <= opt[i+1][j], cubic vs quadratic step counts, tree shape output
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

#define N 40
static long w[N + 2][N + 2], ce[N + 2][N + 2], ck[N + 2][N + 2];
static int ro[N + 2][N + 2], rk[N + 2][N + 2];
static long pre[N + 2];

static void shape(int i, int j, char *b, int *pos) {
    if (i > j) { b[(*pos)++] = '.'; return; }
    int r = rk[i][j];
    b[(*pos)++] = '(';
    shape(i, r - 1, b, pos);
    *pos += snprintf(b + *pos, 16, "%d", r);
    shape(r + 1, j, b, pos);
    b[(*pos)++] = ')';
    b[*pos] = 0;
}

int main(void) {
    for (int t = 0; t < 6; t++) {
        int n = t == 0 ? 5 : 8 + rr(N - 8);
        long p[N + 2];
        pre[0] = 0;
        for (int i = 1; i <= n; i++) { p[i] = t == 0 ? (long[]){0, 15, 10, 5, 10, 20}[i] : 1 + rr(100); pre[i] = pre[i - 1] + p[i]; }
        long cubic_steps = 0, knuth_steps = 0;
        /* cost[i][j] for keys i..j; empty range cost 0. cost = sum(p) + min_r(cost[i][r-1] + cost[r+1][j]) */
        for (int i = 1; i <= n + 1; i++) { ce[i][i - 1] = 0; ck[i][i - 1] = 0; }
        for (int len = 1; len <= n; len++)
            for (int i = 1; i + len - 1 <= n; i++) {
                int j = i + len - 1;
                long best = -1;
                for (int r = i; r <= j; r++) {
                    cubic_steps++;
                    long c = ce[i][r - 1] + ce[r + 1][j];
                    if (best < 0 || c < best) { best = c; ro[i][j] = r; }
                }
                ce[i][j] = best + pre[j] - pre[i - 1];
            }
        for (int len = 1; len <= n; len++)
            for (int i = 1; i + len - 1 <= n; i++) {
                int j = i + len - 1;
                int lo = len == 1 ? i : rk[i][j - 1], hi = len == 1 ? j : rk[i + 1][j];
                long best = -1;
                for (int r = lo; r <= hi; r++) {
                    knuth_steps++;
                    long c = ck[i][r - 1] + ck[r + 1][j];
                    if (best < 0 || c < best) { best = c; rk[i][j] = r; }
                }
                ck[i][j] = best + pre[j] - pre[i - 1];
            }
        CHECK(ce[1][n] == ck[1][n]);
        printf("case %d: n=%d cost=%ld cubic_steps=%ld knuth_steps=%ld", t, n, ck[1][n], cubic_steps, knuth_steps);
        if (t == 0) {
            char b[256];
            int pos = 0;
            shape(1, n, b, &pos);
            printf(" shape=%s", b);
        }
        printf("\n");
        CHECK(knuth_steps <= cubic_steps);
    }
    (void)w; (void)ro;
    return 0;
}
