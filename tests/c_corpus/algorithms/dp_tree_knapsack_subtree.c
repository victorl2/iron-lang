/*
 * title: Tree knapsack: choose k nodes forming a connected subtree from the root
 * topic: algorithms
 * covers: dynamic programming, tree DP, merging child tables, min-plus/max-plus convolution, subtree size bound, brute force over subsets
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

#define N 14
#define NEG (-1000000)

static int par[N], val[N], size[N];
static int f[N][N + 1]; /* f[v][k]: best sum choosing k nodes in subtree of v, with v chosen and connected */

int main(void) {
    for (int t = 0; t < 8; t++) {
        int n = 5 + rr(N - 4);
        par[0] = -1;
        for (int i = 1; i < n; i++) par[i] = rr(i);
        for (int i = 0; i < n; i++) val[i] = rr(41) - 15; /* negative values matter */
        for (int v = 0; v < n; v++) { size[v] = 1; for (int k = 0; k <= N; k++) f[v][k] = NEG; f[v][1] = val[v]; }
        for (int v = n - 1; v >= 1; v--) {
            int p = par[v];
            int g[N + 1];
            for (int k = 0; k <= N; k++) g[k] = f[p][k];
            for (int a = 1; a <= size[p]; a++) {
                if (f[p][a] == NEG) continue;
                for (int b = 1; b <= size[v] && a + b <= N; b++)
                    if (f[v][b] != NEG && f[p][a] + f[v][b] > g[a + b]) g[a + b] = f[p][a] + f[v][b];
            }
            for (int k = 0; k <= N; k++) f[p][k] = g[k];
            size[p] += size[v];
        }
        printf("case %d: n=%d best by k:", t, n);
        for (int k = 1; k <= n; k++) {
            /* brute force: subsets of size k that contain root and are connected */
            int bb = NEG;
            for (unsigned m = 1; m < (1u << n); m += 2) {
                if (__builtin_popcount(m) != k) continue;
                int ok = 1, s = 0;
                for (int v = 0; v < n && ok; v++) {
                    if (!(m >> v & 1)) continue;
                    s += val[v];
                    if (v > 0 && !(m >> par[v] & 1)) ok = 0;
                }
                if (ok && s > bb) bb = s;
            }
            CHECK(bb == f[0][k]);
            printf(" %d", f[0][k]);
        }
        printf("\n");
    }
    return 0;
}
