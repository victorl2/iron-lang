/*
 * title: Tree diameter, centers and rerooting sums
 * topic: algorithms
 * covers: tree diameter, double BFS, tree centers, rerooting DP, sum of distances, subtree sizes
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 200 };

static unsigned st = 1000003u;
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

static int adj[N][N], deg[N];

static void bfs(int s, int *dist, int *par) {
    int q[N], qh = 0, qt = 0;
    for (int i = 0; i < N; i++) dist[i] = -1;
    dist[s] = 0;
    par[s] = -1;
    q[qt++] = s;
    while (qh < qt) {
        int u = q[qh++];
        for (int i = 0; i < deg[u]; i++) {
            int v = adj[u][i];
            if (dist[v] < 0) dist[v] = dist[u] + 1, par[v] = u, q[qt++] = v;
        }
    }
}

static int size[N];
static long down[N], all[N];
static int order[N], no, parent_[N];

int main(void) {
    for (int v = 1; v < N; v++) {
        int p = v < 4 ? 0 : v - 1 - (int)(rnd() % 4u); /* fairly deep tree */
        if (p < 0) p = 0;
        adj[v][deg[v]++] = p;
        adj[p][deg[p]++] = v;
    }
    int d0[N], p0[N], d1[N], p1[N], d2[N], p2[N];
    bfs(0, d0, p0);
    int a = 0;
    for (int v = 0; v < N; v++)
        if (d0[v] > d0[a]) a = v;
    bfs(a, d1, p1);
    int b = a;
    for (int v = 0; v < N; v++)
        if (d1[v] > d1[b]) b = v;
    int diam = d1[b];
    bfs(b, d2, p2);
    /* brute force diameter via all-pairs BFS */
    int best = 0;
    for (int s = 0; s < N; s++) {
        int d[N], p[N];
        bfs(s, d, p);
        for (int v = 0; v < N; v++)
            if (d[v] > best) best = d[v];
    }
    check(best == diam, "double BFS diameter");
    /* centers: middle of the diameter path */
    int path[N], pl = 0;
    for (int v = b; v != -1; v = p1[v]) path[pl++] = v;
    check(pl == diam + 1, "path length");
    int c1 = path[diam / 2], c2 = path[(diam + 1) / 2];
    for (int v = 0; v < N; v++) {
        int ecc = d1[v] > d2[v] ? d1[v] : d2[v];
        int expect = (diam + 1) / 2;
        int is_center = ecc == expect;
        check(is_center == (v == c1 || v == c2) || diam % 2 == 1, "center by eccentricity");
    }
    /* rerooting: sum of distances from each vertex */
    parent_[0] = -1;
    order[no++] = 0;
    for (int i = 0; i < no; i++) {
        int u = order[i];
        for (int k = 0; k < deg[u]; k++)
            if (adj[u][k] != parent_[u]) parent_[adj[u][k]] = u, order[no++] = adj[u][k];
    }
    for (int i = N - 1; i >= 0; i--) {
        int u = order[i];
        size[u] += 1;
        down[u] += 0;
        if (parent_[u] >= 0) size[parent_[u]] += size[u], down[parent_[u]] += down[u] + size[u];
    }
    all[0] = down[0];
    for (int i = 1; i < N; i++) {
        int u = order[i], p = parent_[u];
        all[u] = all[p] - size[u] + (N - size[u]);
    }
    long total = 0, minsum = all[0];
    int med = 0;
    for (int v = 0; v < N; v++) {
        int d[N], p[N];
        bfs(v, d, p);
        long s = 0;
        for (int u = 0; u < N; u++) s += d[u];
        check(s == all[v], "rerooted sum equals BFS sum");
        total += s;
        if (s < minsum) minsum = s, med = v;
    }
    printf("diameter %d between %d and %d\n", diam, a, b);
    printf("center(s): %d%s", c1, c1 == c2 ? "" : " ");
    if (c1 != c2) printf("%d", c2);
    printf("\n");
    printf("tree median (min distance sum): vertex %d sum %ld\n", med, minsum);
    printf("sum of all pairwise distances (ordered): %ld\n", total);
    printf("root subtree sizes of first children:");
    for (int k = 0; k < deg[0] && k < 5; k++) printf(" %d", size[adj[0][k]]);
    printf("\n");
    return 0;
}
