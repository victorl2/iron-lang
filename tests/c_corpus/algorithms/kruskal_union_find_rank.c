/*
 * title: Kruskal MST with union-find and cut property checks
 * topic: algorithms
 * covers: Kruskal, disjoint sets, union by rank, path compression, unique weights, MST verification
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { N = 60, M = 300 };

static unsigned st = 424242u;
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

typedef struct {
    int u, v, w;
} Edge;
static Edge E[M];

static int parent[N], rnk[N];
static long finds, hops;

static int find(int x) {
    finds++;
    int r = x;
    while (parent[r] != r) r = parent[r], hops++;
    while (parent[x] != r) {
        int nx = parent[x];
        parent[x] = r;
        x = nx;
    }
    return r;
}
static int unite(int a, int b) {
    a = find(a);
    b = find(b);
    if (a == b) return 0;
    if (rnk[a] < rnk[b]) {
        int t = a;
        a = b;
        b = t;
    }
    parent[b] = a;
    if (rnk[a] == rnk[b]) rnk[a]++;
    return 1;
}

static int cmp(const void *a, const void *b) {
    const Edge *x = a, *y = b;
    if (x->w != y->w) return x->w < y->w ? -1 : 1;
    if (x->u != y->u) return x->u - y->u;
    return x->v - y->v;
}

int main(void) {
    int m = 0;
    /* spanning path to guarantee connectivity, then random edges */
    for (int i = 1; i < N; i++) E[m++] = (Edge){(int)(rnd() % (unsigned)i), i, 1 + (int)(rnd() % 1000)};
    while (m < M) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        if (u == v) continue;
        E[m++] = (Edge){u < v ? u : v, u < v ? v : u, 1 + (int)(rnd() % 1000)};
    }
    qsort(E, M, sizeof(Edge), cmp);
    for (int i = 0; i < N; i++) parent[i] = i, rnk[i] = 0;
    int inmst[M] = {0}, taken = 0;
    long total = 0;
    for (int i = 0; i < M && taken < N - 1; i++)
        if (unite(E[i].u, E[i].v)) inmst[i] = 1, taken++, total += E[i].w;
    check(taken == N - 1, "spanning tree has n-1 edges");
    /* cycle property: each non-tree edge is at least as heavy as the max edge on the tree path */
    int adj[N][N];
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) adj[i][j] = 0;
    for (int i = 0; i < M; i++)
        if (inmst[i]) adj[E[i].u][E[i].v] = adj[E[i].v][E[i].u] = E[i].w;
    int maxpath[N][N];
    for (int s = 0; s < N; s++) {
        int q[N], qh = 0, qt = 0, seen[N] = {0};
        q[qt++] = s;
        seen[s] = 1;
        maxpath[s][s] = 0;
        while (qh < qt) {
            int u = q[qh++];
            for (int v = 0; v < N; v++)
                if (adj[u][v] && !seen[v]) {
                    seen[v] = 1;
                    maxpath[s][v] = maxpath[s][u] > adj[u][v] ? maxpath[s][u] : adj[u][v];
                    q[qt++] = v;
                }
        }
    }
    int heavier = 0;
    for (int i = 0; i < M; i++)
        if (!inmst[i]) {
            check(E[i].w >= maxpath[E[i].u][E[i].v], "cycle property");
            heavier += E[i].w > maxpath[E[i].u][E[i].v];
        }
    int maxw = 0, heavy_end = 0;
    for (int i = 0; i < M; i++)
        if (inmst[i] && E[i].w > maxw) maxw = E[i].w, heavy_end = i;
    printf("mst weight %ld over %d edges\n", total, taken);
    printf("heaviest tree edge: %d-%d weight %d\n", E[heavy_end].u, E[heavy_end].v, maxw);
    printf("non-tree edges strictly heavier than path max: %d of %d\n", heavier, M - taken);
    printf("union-find finds %ld, hops %ld\n", finds, hops);
    int deg[N] = {0}, maxdeg = 0;
    for (int i = 0; i < M; i++)
        if (inmst[i]) deg[E[i].u]++, deg[E[i].v]++;
    for (int i = 0; i < N; i++)
        if (deg[i] > maxdeg) maxdeg = deg[i];
    printf("max tree degree %d\n", maxdeg);
    return 0;
}
