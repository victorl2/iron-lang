/*
 * title: Gomory-Hu cut tree by Gusfield's algorithm
 * topic: data_structures
 * covers: gomory-hu tree, gusfield, max-flow min-cut, edmonds-karp, all-pairs min cut, subset enumeration brute force
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 12

static int n;
static int w[MAXN][MAXN]; /* undirected capacities */

static unsigned long long rs = 0x0123456789ULL * 7919;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *m) { if (!c) { fprintf(stderr, "check failed: %s\n", m); exit(1); } }

/* Edmonds-Karp on a capacity matrix; fills side[] with the source side of a min cut */
static int maxflow(int s, int t, int *side) {
    int cap[MAXN][MAXN];
    memcpy(cap, w, sizeof cap);
    int flow = 0;
    for (;;) {
        int prev[MAXN], q[MAXN], h = 0, tl = 0;
        for (int i = 0; i < n; i++) prev[i] = -1;
        prev[s] = s; q[tl++] = s;
        while (h < tl && prev[t] < 0) {
            int u = q[h++];
            for (int v = 0; v < n; v++) if (prev[v] < 0 && cap[u][v] > 0) { prev[v] = u; q[tl++] = v; }
        }
        if (prev[t] < 0) {
            for (int i = 0; i < n; i++) side[i] = prev[i] >= 0;
            return flow;
        }
        int b = 1 << 30;
        for (int v = t; v != s; v = prev[v]) if (cap[prev[v]][v] < b) b = cap[prev[v]][v];
        for (int v = t; v != s; v = prev[v]) { cap[prev[v]][v] -= b; cap[v][prev[v]] += b; }
        flow += b;
    }
}

static int par[MAXN], fl[MAXN];
static void gusfield(void) {
    for (int i = 0; i < n; i++) { par[i] = 0; fl[i] = 0; }
    for (int s = 1; s < n; s++) {
        int t = par[s], side[MAXN];
        int f = maxflow(s, t, side);
        fl[s] = f;
        for (int i = 0; i < n; i++) if (i != s && side[i] && par[i] == t) par[i] = s;
        if (side[par[t]] && t != 0) { par[s] = par[t]; par[t] = s; fl[s] = fl[t]; fl[t] = f; }
    }
}
/* min edge on the tree path between a and b */
static int tree_query(int a, int b) {
    int depth[MAXN], root_seen = 0;
    (void)root_seen;
    /* the tree is rooted at whatever par[] gives; find root (par[x]==x or 0 self) by climbing */
    for (int i = 0; i < n; i++) { int d = 0, x = i; while (par[x] != x && d <= n) { x = par[x]; d++; } depth[i] = d; }
    int best = 1 << 30;
    while (a != b) {
        if (depth[a] >= depth[b]) { if (fl[a] < best) best = fl[a]; a = par[a]; }
        else { if (fl[b] < best) best = fl[b]; b = par[b]; }
    }
    return best;
}
static int cut_capacity(unsigned mask) {
    int c = 0;
    for (int u = 0; u < n; u++) for (int v = u + 1; v < n; v++) if (((mask >> u) & 1) != ((mask >> v) & 1)) c += w[u][v];
    return c;
}

int main(void) {
    int total_edges = 0;
    for (int g = 0; g < 12; g++) {
        n = 5 + (g % 8);
        memset(w, 0, sizeof w);
        int m = n + (int)(rnd() % (unsigned)(n + 4));
        if (g == 0) { /* CLRS-style small example */
            n = 6;
            int e[][3] = { {0,1,7}, {0,2,4}, {1,2,2}, {1,3,4}, {2,4,5}, {3,4,4}, {3,5,3}, {4,5,7} };
            for (unsigned i = 0; i < sizeof e / sizeof e[0]; i++) w[e[i][0]][e[i][1]] = w[e[i][1]][e[i][0]] = e[i][2];
        } else {
            for (int i = 0; i < m; i++) {
                int a = (int)(rnd() % (unsigned)n), b = (int)(rnd() % (unsigned)n);
                if (a != b) { int c = 1 + (int)(rnd() % 9); w[a][b] += c; w[b][a] += c; }
            }
        }
        for (int u = 0; u < n; u++) for (int v = u + 1; v < n; v++) if (w[u][v]) total_edges++;
        gusfield();
        par[0] = 0; /* root marker for climbing */
        /* brute force: enumerate all vertex subsets, best[s][t] = min cut over subsets separating s from t */
        int best[MAXN][MAXN];
        for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) best[i][j] = 1 << 30;
        for (unsigned mask = 1; mask < (1u << n) - 1; mask++) {
            int c = cut_capacity(mask);
            for (int s = 0; s < n; s++) if ((mask >> s) & 1) for (int t = 0; t < n; t++) if (!((mask >> t) & 1) && c < best[s][t]) best[s][t] = best[t][s] = c;
        }
        long sum = 0;
        for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) {
            int q = tree_query(a, b);
            check(q == best[a][b], "tree path minimum equals min cut");
            int side[MAXN];
            check(maxflow(a, b, side) == q, "max-flow equals tree query");
            sum += q;
        }
        /* cut-tree property: removing tree edge (s,par[s]) splits vertices into two sets whose cut equals fl[s] */
        int tree_edges = 0;
        for (int s = 1; s < n; s++) {
            int p = par[s];
            if (p == s) continue;
            /* vertices in s's component after removing that edge: those whose climb to root passes through s (before p) */
            unsigned mask = 0;
            for (int v = 0; v < n; v++) { int x = v, hit = 0, d = 0; while (d++ <= n) { if (x == s) { hit = 1; break; } if (par[x] == x) break; x = par[x]; } if (hit) mask |= 1u << v; }
            check(cut_capacity(mask) == fl[s], "tree edge defines a min cut");
            tree_edges++;
        }
        printf("graph %2d: n=%2d edges=%2d tree edges=%d sum of all-pairs mincut=%ld  tree:", g, n, m, tree_edges, sum);
        for (int s = 1; s < n; s++) printf(" %d-%d(%d)", par[s], s, fl[s]);
        printf("\n");
    }
    printf("total undirected edges %d\n", total_edges);
    return 0;
}
