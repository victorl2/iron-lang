/*
 * title: Hopcroft-Karp bipartite matching
 * topic: algorithms
 * covers: Hopcroft-Karp, BFS layering, DFS augmenting paths, phase counts, comparison with simple augmenting matching
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { L = 120, R = 120, MAXDEG = 8, INF = 1 << 28 };

static unsigned st = 271828u;
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

static int adj[L][MAXDEG], deg[L];
static int ml[L], mr[R], dist[L];

static int bfs(void) {
    int q[L], qh = 0, qt = 0, found = 0;
    for (int u = 0; u < L; u++) {
        if (ml[u] < 0) dist[u] = 0, q[qt++] = u;
        else dist[u] = INF;
    }
    while (qh < qt) {
        int u = q[qh++];
        for (int i = 0; i < deg[u]; i++) {
            int w = mr[adj[u][i]];
            if (w < 0) found = 1;
            else if (dist[w] == INF) dist[w] = dist[u] + 1, q[qt++] = w;
        }
    }
    return found;
}
static int dfs(int u) {
    for (int i = 0; i < deg[u]; i++) {
        int v = adj[u][i], w = mr[v];
        if (w < 0 || (dist[w] == dist[u] + 1 && dfs(w))) {
            ml[u] = v;
            mr[v] = u;
            return 1;
        }
    }
    dist[u] = INF;
    return 0;
}

static int vis[R];
static int kuhn(int u) {
    for (int i = 0; i < deg[u]; i++) {
        int v = adj[u][i];
        if (vis[v]) continue;
        vis[v] = 1;
        if (mr[v] < 0 || kuhn(mr[v])) {
            ml[u] = v;
            mr[v] = u;
            return 1;
        }
    }
    return 0;
}

int main(void) {
    for (int trial = 0; trial < 4; trial++) {
        int d = 1 + trial; /* out-degree per left vertex */
        int edges = 0;
        for (int u = 0; u < L; u++) {
            deg[u] = 0;
            for (int k = 0; k < d; k++) {
                int v = (int)(rnd() % R), dup = 0;
                for (int j = 0; j < deg[u]; j++) dup |= adj[u][j] == v;
                if (!dup) adj[u][deg[u]++] = v, edges++;
            }
        }
        memset(ml, -1, sizeof ml);
        memset(mr, -1, sizeof mr);
        int size = 0, phases = 0;
        while (bfs()) {
            phases++;
            for (int u = 0; u < L; u++)
                if (ml[u] < 0 && dfs(u)) size++;
        }
        for (int u = 0; u < L; u++)
            if (ml[u] >= 0) check(mr[ml[u]] == u, "matching consistent");
        /* Kuhn reference */
        int hk_ml[L], hk_mr[R];
        memcpy(hk_ml, ml, sizeof ml);
        memcpy(hk_mr, mr, sizeof mr);
        memset(ml, -1, sizeof ml);
        memset(mr, -1, sizeof mr);
        int ksize = 0;
        for (int u = 0; u < L; u++) {
            memset(vis, 0, sizeof vis);
            ksize += kuhn(u);
        }
        check(ksize == size, "hopcroft-karp equals kuhn");
        int unmatched = 0;
        for (int u = 0; u < L; u++) unmatched += hk_ml[u] < 0;
        check(unmatched == L - size, "unmatched count");
        (void)hk_mr;
        printf("degree %d: edges %3d, matching %3d, phases %d, unmatched left %d\n", d, edges, size, phases, unmatched);
    }
    return 0;
}
