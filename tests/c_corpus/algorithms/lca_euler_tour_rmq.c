/*
 * title: LCA via Euler tour and sparse table
 * topic: algorithms
 * covers: lowest common ancestor, Euler tour, range minimum query, sparse table, first occurrence index, adjacency lists
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 300, TOUR = 2 * N, LOGT = 10 };

static unsigned st = 7654321u;
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

static int child[N][N], nchild[N], parent[N], depth[N];
static int tour[TOUR], tlen, first[N];
static int sparse[LOGT][TOUR], lg[TOUR + 1];

static void euler(int u) {
    first[u] = tlen;
    tour[tlen++] = u;
    for (int i = 0; i < nchild[u]; i++) {
        euler(child[u][i]);
        tour[tlen++] = u;
    }
}

static int shallower(int a, int b) { return depth[a] < depth[b] || (depth[a] == depth[b] && a < b) ? a : b; }

static int lca(int a, int b) {
    int l = first[a], r = first[b];
    if (l > r) {
        int t = l;
        l = r;
        r = t;
    }
    int k = lg[r - l + 1];
    return shallower(sparse[k][l], sparse[k][r - (1 << k) + 1]);
}

int main(void) {
    parent[0] = -1;
    for (int v = 1; v < N; v++) {
        parent[v] = (int)(rnd() % (unsigned)v);
        depth[v] = depth[parent[v]] + 1;
        child[parent[v]][nchild[parent[v]]++] = v;
    }
    euler(0);
    check(tlen == 2 * N - 1, "tour length");
    lg[1] = 0;
    for (int i = 2; i <= tlen; i++) lg[i] = lg[i / 2] + 1;
    for (int i = 0; i < tlen; i++) sparse[0][i] = tour[i];
    for (int k = 1; (1 << k) <= tlen; k++)
        for (int i = 0; i + (1 << k) <= tlen; i++) sparse[k][i] = shallower(sparse[k - 1][i], sparse[k - 1][i + (1 << (k - 1))]);
    int maxd = 0, leaves = 0;
    for (int v = 0; v < N; v++) {
        if (depth[v] > maxd) maxd = depth[v];
        leaves += nchild[v] == 0;
    }
    long dsum = 0;
    for (int q = 0; q < 30000; q++) {
        int a = (int)(rnd() % N), b = (int)(rnd() % N);
        int l = lca(a, b);
        int x = a, y = b;
        while (depth[x] > depth[y]) x = parent[x];
        while (depth[y] > depth[x]) y = parent[y];
        while (x != y) x = parent[x], y = parent[y];
        check(l == x, "rmq lca equals climbing lca");
        dsum += depth[a] + depth[b] - 2 * depth[l];
    }
    /* subtree membership via tour: b in subtree(a) iff lca(a,b)==a */
    int sub[N];
    for (int v = 0; v < N; v++) {
        sub[v] = 0;
        for (int u = 0; u < N; u++) sub[v] += lca(v, u) == v;
    }
    check(sub[0] == N, "root subtree is everything");
    int biggest = 0, bv = 1;
    for (int v = 1; v < N; v++)
        if (sub[v] > biggest) biggest = sub[v], bv = v;
    printf("nodes %d, leaves %d, max depth %d, tour length %d\n", N, leaves, maxd, tlen);
    printf("sparse table levels: %d\n", lg[tlen] + 1);
    printf("distance sum over 30000 queries: %ld\n", dsum);
    printf("largest non-root subtree: node %d with %d nodes\n", bv, biggest);
    return 0;
}
