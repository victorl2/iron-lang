/*
 * title: Bridges and articulation points by lowlink
 * topic: algorithms
 * covers: DFS lowlink, bridges, articulation points, parallel edges by edge id, removal brute-force check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 30, M = 38 };

static unsigned st = 2357u;
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

static int eu[M], ev[M];
static int adjv[N][M], adje[N][M], deg[N];
static int disc[N], low[N], timer_;
static int is_bridge[M], is_art[N];

static void dfs(int u, int pe) {
    disc[u] = low[u] = ++timer_;
    int children = 0;
    for (int i = 0; i < deg[u]; i++) {
        int v = adjv[u][i], e = adje[u][i];
        if (e == pe) continue;
        if (disc[v]) {
            if (disc[v] < low[u]) low[u] = disc[v];
        } else {
            children++;
            dfs(v, e);
            if (low[v] < low[u]) low[u] = low[v];
            if (low[v] > disc[u]) is_bridge[e] = 1;
            if (pe >= 0 && low[v] >= disc[u]) is_art[u] = 1;
        }
    }
    if (pe < 0 && children > 1) is_art[u] = 1;
}

static int components(int skip_edge, int skip_vertex) {
    int comp[N], c = 0;
    memset(comp, -1, sizeof comp);
    for (int s = 0; s < N; s++) {
        if (s == skip_vertex || comp[s] >= 0) continue;
        int stack[N], sp = 0;
        stack[sp++] = s;
        comp[s] = c;
        while (sp) {
            int u = stack[--sp];
            for (int i = 0; i < deg[u]; i++) {
                int v = adjv[u][i];
                if (adje[u][i] == skip_edge || v == skip_vertex || comp[v] >= 0) continue;
                comp[v] = c;
                stack[sp++] = v;
            }
        }
        c++;
    }
    return c;
}

int main(void) {
    int m = 0;
    while (m < M) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        if (u == v) continue;
        /* parallel edges are allowed and are never bridges */
        eu[m] = u, ev[m] = v;
        adjv[u][deg[u]] = v, adje[u][deg[u]++] = m;
        adjv[v][deg[v]] = u, adje[v][deg[v]++] = m;
        m++;
    }
    for (int u = 0; u < N; u++)
        if (!disc[u]) dfs(u, -1);
    int base = components(-1, -1);
    int nb = 0, na = 0;
    for (int e = 0; e < M; e++) {
        int more = components(e, -1) > base;
        check(more == is_bridge[e], "bridge matches removal test");
        nb += is_bridge[e];
    }
    for (int v = 0; v < N; v++) {
        /* removing v drops its own component slot: compare against base minus isolated case */
        int iso = 1;
        for (int i = 0; i < deg[v]; i++) iso = 0;
        int after = components(-1, v);
        int expect_more = after > base - (iso ? 1 : 0);
        check(expect_more == is_art[v], "articulation matches removal test");
        na += is_art[v];
    }
    printf("vertices %d, edges %d, connected components %d\n", N, M, base);
    printf("bridges (%d):", nb);
    for (int e = 0; e < M; e++)
        if (is_bridge[e]) printf(" %d-%d", eu[e] < ev[e] ? eu[e] : ev[e], eu[e] < ev[e] ? ev[e] : eu[e]);
    printf("\narticulation points (%d):", na);
    for (int v = 0; v < N; v++)
        if (is_art[v]) printf(" %d", v);
    printf("\n");
    return 0;
}
