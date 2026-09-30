/*
 * title: Second-best spanning tree by edge swap
 * topic: algorithms
 * covers: minimum spanning tree, second best MST, path maximum query, edge exchange, brute-force verification
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { N = 9, M = 22 };

static unsigned st = 3141592u;
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
static int par[N];
static int find(int x) {
    while (par[x] != x) x = par[x];
    return x;
}

static int cmp(const void *a, const void *b) {
    const Edge *x = a, *y = b;
    if (x->w != y->w) return x->w - y->w;
    if (x->u != y->u) return x->u - y->u;
    return x->v - y->v;
}

static long brute_best, brute_second;

int main(void) {
    int m = 0;
    int seen[N][N] = {{0}};
    for (int i = 1; i < N; i++) {
        int j = (int)(rnd() % (unsigned)i);
        E[m++] = (Edge){j, i, 1 + (int)(rnd() % 30)};
        seen[j][i] = seen[i][j] = 1;
    }
    while (m < M) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        if (u == v || seen[u][v]) continue;
        seen[u][v] = seen[v][u] = 1;
        E[m++] = (Edge){u, v, 1 + (int)(rnd() % 30)};
    }
    qsort(E, M, sizeof(Edge), cmp);
    int in[M] = {0};
    long mst = 0;
    for (int i = 0; i < N; i++) par[i] = i;
    for (int i = 0; i < M; i++) {
        int a = find(E[i].u), b = find(E[i].v);
        if (a != b) par[a] = b, in[i] = 1, mst += E[i].w;
    }
    /* second best via single swap: for each non-tree edge, remove max edge on its tree path */
    long second = -1;
    for (int i = 0; i < M; i++) {
        if (in[i]) continue;
        /* find path max in the tree via DFS from u to v */
        int stack[N], sp = 0, from[N], fromedge[N], vis[N] = {0};
        stack[sp++] = E[i].u;
        vis[E[i].u] = 1;
        from[E[i].u] = -1;
        while (sp) {
            int x = stack[--sp];
            for (int e = 0; e < M; e++) {
                if (!in[e]) continue;
                int y = E[e].u == x ? E[e].v : (E[e].v == x ? E[e].u : -1);
                if (y < 0 || vis[y]) continue;
                vis[y] = 1;
                from[y] = x;
                fromedge[y] = e;
                stack[sp++] = y;
            }
        }
        int mx = -1;
        for (int x = E[i].v; from[x] != -1; x = from[x])
            if (E[fromedge[x]].w > mx) mx = E[fromedge[x]].w;
        long cand = mst + E[i].w - mx;
        if (cand > mst && (second < 0 || cand < second)) second = cand;
    }
    /* brute force over all (N-1)-subsets of edges */
    brute_best = 1L << 40;
    brute_second = 1L << 40;
    int choose[N - 1];
    /* iterative combination enumeration */
    for (int i = 0; i < N - 1; i++) choose[i] = i;
    long trees = 0;
    for (;;) {
        for (int k = 0; k < N; k++) par[k] = k;
        int ok = 1;
        long wsum = 0;
        for (int i = 0; i < N - 1 && ok; i++) {
            Edge e = E[choose[i]];
            int a = find(e.u), b = find(e.v);
            if (a == b) ok = 0;
            else par[a] = b, wsum += e.w;
        }
        if (ok) {
            trees++;
            if (wsum < brute_best) brute_second = brute_best, brute_best = wsum;
            else if (wsum > brute_best && wsum < brute_second) brute_second = wsum;
        }
        int i = N - 2;
        while (i >= 0 && choose[i] == M - (N - 1) + i) i--;
        if (i < 0) break;
        choose[i]++;
        for (int j = i + 1; j < N - 1; j++) choose[j] = choose[j - 1] + 1;
    }
    check(brute_best == mst, "mst matches brute force");
    printf("edges %d, spanning trees %ld\n", M, trees);
    printf("mst weight %ld\n", mst);
    printf("second best (strictly larger) by swap: %ld\n", second);
    if (second >= 0) check(second == brute_second, "second best matches brute force");
    printf("second best by brute force: %ld\n", brute_second);
    return 0;
}
