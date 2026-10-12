/*
 * title: Tree DP: maximum weight independent set and vertex cover
 * topic: algorithms
 * covers: dynamic programming, tree DP, take/skip states, post-order via explicit stack, reconstruction, bitmask brute force
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

#define N 18

static int par[N], w[N], nchild[N], child[N][N];

int main(void) {
    for (int t = 0; t < 8; t++) {
        int n = 6 + rr(N - 6);
        par[0] = -1;
        for (int i = 0; i < N; i++) nchild[i] = 0;
        for (int i = 1; i < n; i++) {
            par[i] = rr(i);
            child[par[i]][nchild[par[i]]++] = i;
        }
        for (int i = 0; i < n; i++) w[i] = 1 + rr(20);
        /* parents always have smaller index, so reverse index order is a valid post-order */
        int take[N], skip[N];
        for (int v = n - 1; v >= 0; v--) {
            take[v] = w[v]; skip[v] = 0;
            for (int c = 0; c < nchild[v]; c++) {
                int u = child[v][c];
                take[v] += skip[u];
                skip[v] += take[u] > skip[u] ? take[u] : skip[u];
            }
        }
        int best = take[0] > skip[0] ? take[0] : skip[0];
        /* reconstruct top-down */
        int chosen[N] = {0}, state[N];
        state[0] = take[0] > skip[0];
        for (int v = 0; v < n; v++) {
            if (v > 0) state[v] = state[par[v]] ? 0 : (take[v] > skip[v]);
            chosen[v] = state[v];
        }
        int sum = 0, tot = 0;
        for (int v = 0; v < n; v++) {
            tot += w[v];
            if (chosen[v]) { sum += w[v]; CHECK(v == 0 || !chosen[par[v]]); }
        }
        CHECK(sum == best);
        int bb = 0;
        for (unsigned m = 0; m < (1u << n); m++) {
            int ok = 1, s = 0;
            for (int v = 0; v < n && ok; v++) {
                if (!(m >> v & 1)) continue;
                s += w[v];
                if (v > 0 && (m >> par[v] & 1)) ok = 0;
            }
            if (ok && s > bb) bb = s;
        }
        CHECK(bb == best);
        printf("case %d: n=%d total=%d max_independent=%d min_vertex_cover=%d picked=", t, n, tot, best, tot - best);
        for (int v = 0; v < n; v++) if (chosen[v]) printf("%d,", v);
        printf("\n");
    }
    return 0;
}
