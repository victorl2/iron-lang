/*
 * title: Prim MST dense array versus heap
 * topic: algorithms
 * covers: Prim, minimum spanning tree, adjacency matrix, indexed priority queue, decrease-key, total weight agreement
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { N = 80, INF = 1 << 28 };

static unsigned st = 9182736u;
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

static int w[N][N];

/* indexed min-heap keyed by key[v] with tie-break on vertex id */
static int key[N], pos[N], hp[N], hn;
static long decs;
static int lt(int a, int b) { return key[a] < key[b] || (key[a] == key[b] && a < b); }
static void swap_h(int i, int j) {
    int t = hp[i];
    hp[i] = hp[j];
    hp[j] = t;
    pos[hp[i]] = i;
    pos[hp[j]] = j;
}
static void up(int i) {
    while (i > 0 && lt(hp[i], hp[(i - 1) / 2])) swap_h(i, (i - 1) / 2), i = (i - 1) / 2;
}
static void down(int i) {
    for (;;) {
        int c = 2 * i + 1;
        if (c >= hn) return;
        if (c + 1 < hn && lt(hp[c + 1], hp[c])) c++;
        if (!lt(hp[c], hp[i])) return;
        swap_h(i, c);
        i = c;
    }
}

int main(void) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) w[i][j] = INF;
    for (int i = 1; i < N; i++) {
        int j = (int)(rnd() % (unsigned)i), c = 1 + (int)(rnd() % 500);
        w[i][j] = w[j][i] = c;
    }
    for (int k = 0; k < 500; k++) {
        int i = (int)(rnd() % N), j = (int)(rnd() % N);
        if (i == j) continue;
        int c = 1 + (int)(rnd() % 500);
        if (c < w[i][j]) w[i][j] = w[j][i] = c;
    }
    /* dense O(n^2) */
    int best[N], done[N], from[N];
    long dense = 0;
    for (int i = 0; i < N; i++) best[i] = INF, done[i] = 0, from[i] = -1;
    best[0] = 0;
    for (int it = 0; it < N; it++) {
        int u = -1;
        for (int i = 0; i < N; i++)
            if (!done[i] && (u < 0 || best[i] < best[u])) u = i;
        done[u] = 1;
        dense += best[u];
        for (int v = 0; v < N; v++)
            if (!done[v] && w[u][v] < best[v]) best[v] = w[u][v], from[v] = u;
    }
    /* heap version */
    int in_tree[N] = {0};
    long heaped = 0;
    hn = N;
    for (int i = 0; i < N; i++) key[i] = i == 0 ? 0 : INF, hp[i] = i, pos[i] = i;
    while (hn > 0) {
        int u = hp[0];
        swap_h(0, --hn);
        down(0);
        in_tree[u] = 1;
        heaped += key[u];
        for (int v = 0; v < N; v++)
            if (!in_tree[v] && w[u][v] < key[v]) {
                key[v] = w[u][v];
                decs++;
                up(pos[v]);
            }
    }
    check(dense == heaped, "dense and heap Prim agree");
    /* verify via Kruskal on the matrix */
    int comp[N];
    for (int i = 0; i < N; i++) comp[i] = i;
    long kr = 0;
    for (int wt = 1; wt <= 500; wt++)
        for (int i = 0; i < N; i++)
            for (int j = i + 1; j < N; j++)
                if (w[i][j] == wt && comp[i] != comp[j]) {
                    int a = comp[i], b = comp[j];
                    for (int x = 0; x < N; x++)
                        if (comp[x] == b) comp[x] = a;
                    kr += wt;
                }
    check(kr == dense, "Prim equals Kruskal");
    int edges = 0;
    for (int i = 0; i < N; i++)
        for (int j = i + 1; j < N; j++) edges += w[i][j] < INF;
    printf("vertices %d, edges %d\n", N, edges);
    printf("mst weight %ld\n", dense);
    printf("decrease-key operations %ld\n", decs);
    printf("first tree edges:");
    for (int v = 1; v <= 8; v++) printf(" %d-%d(%d)", from[v], v, w[from[v]][v]);
    printf("\n");
    return 0;
}
