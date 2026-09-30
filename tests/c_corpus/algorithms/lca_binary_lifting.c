/*
 * title: LCA by binary lifting and k-th ancestor
 * topic: algorithms
 * covers: lowest common ancestor, binary lifting, k-th ancestor, tree distance, naive parent-walk verification
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { N = 500, LOG = 9 };

static unsigned st = 1234567u;
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

static int up[LOG][N], depth[N], parent[N];

static int kth(int v, int k) {
    for (int b = 0; b < LOG; b++)
        if (k >> b & 1) v = up[b][v];
    return v;
}
static int lca(int a, int b) {
    if (depth[a] < depth[b]) {
        int t = a;
        a = b;
        b = t;
    }
    a = kth(a, depth[a] - depth[b]);
    if (a == b) return a;
    for (int j = LOG - 1; j >= 0; j--)
        if (up[j][a] != up[j][b]) a = up[j][a], b = up[j][b];
    return up[0][a];
}
static int naive_lca(int a, int b) {
    while (depth[a] > depth[b]) a = parent[a];
    while (depth[b] > depth[a]) b = parent[b];
    while (a != b) a = parent[a], b = parent[b];
    return a;
}

int main(void) {
    /* parents chosen from a window of recent vertices to get deep trees */
    parent[0] = 0;
    depth[0] = 0;
    for (int v = 1; v < N; v++) {
        int window = v < 6 ? v : 6;
        parent[v] = v - 1 - (int)(rnd() % (unsigned)window);
        depth[v] = depth[parent[v]] + 1;
    }
    for (int v = 0; v < N; v++) up[0][v] = parent[v];
    for (int j = 1; j < LOG; j++)
        for (int v = 0; v < N; v++) up[j][v] = up[j - 1][up[j - 1][v]];
    int maxd = 0;
    for (int v = 0; v < N; v++)
        if (depth[v] > maxd) maxd = depth[v];
    check(maxd < (1 << LOG), "depth fits lifting table");
    long dsum = 0;
    int qcount = 0, root_hits = 0, self_hits = 0;
    for (int q = 0; q < 20000; q++) {
        int a = (int)(rnd() % N), b = (int)(rnd() % N);
        int l = lca(a, b);
        check(l == naive_lca(a, b), "lifting equals naive");
        int dist = depth[a] + depth[b] - 2 * depth[l];
        dsum += dist;
        root_hits += l == 0;
        self_hits += l == a || l == b;
        qcount++;
    }
    for (int q = 0; q < 2000; q++) {
        int v = (int)(rnd() % N), k = (int)(rnd() % (unsigned)(depth[v] + 1));
        int u = kth(v, k), w = v;
        for (int i = 0; i < k; i++) w = parent[w];
        check(u == w, "kth ancestor");
        check(depth[u] == depth[v] - k, "kth ancestor depth");
    }
    printf("nodes %d, max depth %d\n", N, maxd);
    printf("queries %d, distance sum %ld\n", qcount, dsum);
    printf("lca is root: %d, one endpoint is lca: %d\n", root_hits, self_hits);
    printf("lca(499,498)=%d lca(400,17)=%d lca(250,251)=%d\n", lca(499, 498), lca(400, 17), lca(250, 251));
    return 0;
}
