/*
 * title: Bipartite test with odd cycle witness
 * topic: algorithms
 * covers: two-coloring, BFS layers, odd cycle extraction, bipartite graph generation, brute-force coloring check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 14, MAXD = N };

static unsigned st = 999983u;
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

static int adj[N][MAXD], deg[N];

/* returns 1 if bipartite; otherwise fills cycle with odd cycle vertices, *clen its length */
static int two_color(int *color, int *cycle, int *clen) {
    int par[N], dep[N];
    memset(color, -1, sizeof(int) * N);
    for (int s = 0; s < N; s++) {
        if (color[s] >= 0) continue;
        int q[N], qh = 0, qt = 0;
        color[s] = 0;
        par[s] = -1;
        dep[s] = 0;
        q[qt++] = s;
        while (qh < qt) {
            int u = q[qh++];
            for (int i = 0; i < deg[u]; i++) {
                int v = adj[u][i];
                if (color[v] < 0) {
                    color[v] = color[u] ^ 1;
                    par[v] = u;
                    dep[v] = dep[u] + 1;
                    q[qt++] = v;
                } else if (color[v] == color[u]) {
                    /* climb both to the common ancestor */
                    int a = u, b = v, left[N], right[N], nl = 0, nr = 0;
                    while (a != b) {
                        if (dep[a] >= dep[b]) left[nl++] = a, a = par[a];
                        else right[nr++] = b, b = par[b];
                    }
                    int n = 0;
                    for (int k = 0; k < nl; k++) cycle[n++] = left[k];
                    cycle[n++] = a;
                    for (int k = nr - 1; k >= 0; k--) cycle[n++] = right[k];
                    *clen = n;
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int has_edge(int a, int b) {
    for (int i = 0; i < deg[a]; i++)
        if (adj[a][i] == b) return 1;
    return 0;
}

static int brute_bipartite(void) {
    for (unsigned m = 0; m < (1u << N); m++) {
        int ok = 1;
        for (int u = 0; u < N && ok; u++)
            for (int i = 0; i < deg[u]; i++)
                if (((m >> u) & 1u) == ((m >> adj[u][i]) & 1u)) ok = 0;
        if (ok) return 1;
    }
    return 0;
}

int main(void) {
    int bip_count = 0;
    for (int trial = 0; trial < 12; trial++) {
        memset(deg, 0, sizeof deg);
        int edges = 0, planted = trial % 3 == 0; /* every third graph gets a random same-side edge */
        int side[N];
        for (int v = 0; v < N; v++) side[v] = (int)(rnd() & 1);
        for (int k = 0; k < 24 + 2 * trial; k++) {
            int u = (int)(rnd() % N), v = (int)(rnd() % N);
            if (u == v || side[u] == side[v] || has_edge(u, v)) continue;
            adj[u][deg[u]++] = v;
            adj[v][deg[v]++] = u;
            edges++;
        }
        if (planted) { /* a triangle guarantees an odd cycle */
            int t[3];
            t[0] = (int)(rnd() % N);
            do t[1] = (int)(rnd() % N); while (t[1] == t[0]);
            do t[2] = (int)(rnd() % N); while (t[2] == t[0] || t[2] == t[1]);
            for (int i = 0; i < 3; i++) {
                int u = t[i], v = t[(i + 1) % 3];
                if (has_edge(u, v)) continue;
                adj[u][deg[u]++] = v;
                adj[v][deg[v]++] = u;
                edges++;
            }
        }
        int color[N], cycle[N], clen = 0;
        int ok = two_color(color, cycle, &clen);
        check(ok == brute_bipartite(), "verdict matches brute force");
        if (ok) {
            for (int u = 0; u < N; u++)
                for (int i = 0; i < deg[u]; i++) check(color[u] != color[adj[u][i]], "proper coloring");
            bip_count++;
            printf("graph %2d: %2d edges, bipartite\n", trial, edges);
        } else {
            check(clen % 2 == 1 && clen >= 3, "odd cycle length");
            for (int i = 0; i < clen; i++) check(has_edge(cycle[i], cycle[(i + 1) % clen]), "cycle edges exist");
            printf("graph %2d: %2d edges, odd cycle of length %d:", trial, edges, clen);
            for (int i = 0; i < clen; i++) printf(" %d", cycle[i]);
            printf("\n");
        }
    }
    printf("bipartite graphs: %d of 12\n", bip_count);
    return 0;
}
