/*
 * title: Kosaraju SCC and condensation DAG
 * topic: algorithms
 * covers: Kosaraju, transpose graph, finish-time order, condensation, source and sink components
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 36, M = 60 };

static unsigned st = 2718281u;
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

static int fwd[N][N], nf[N], bwd[N][N], nb[N];
static int seen[N], order[N], no, comp[N];

static void dfs1(int u) {
    seen[u] = 1;
    for (int i = 0; i < nf[u]; i++)
        if (!seen[fwd[u][i]]) dfs1(fwd[u][i]);
    order[no++] = u;
}
static void dfs2(int u, int c) {
    comp[u] = c;
    for (int i = 0; i < nb[u]; i++)
        if (comp[bwd[u][i]] < 0) dfs2(bwd[u][i], c);
}

int main(void) {
    for (int i = 0; i < M; i++) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        if (u == v) continue;
        fwd[u][nf[u]++] = v;
        bwd[v][nb[v]++] = u;
    }
    for (int u = 0; u < N; u++)
        if (!seen[u]) dfs1(u);
    memset(comp, -1, sizeof comp);
    int nc = 0;
    for (int i = N - 1; i >= 0; i--)
        if (comp[order[i]] < 0) dfs2(order[i], nc++);
    /* Kosaraju numbers components in topological order of the condensation */
    static unsigned char cedge[N][N];
    int cedges = 0;
    for (int u = 0; u < N; u++)
        for (int i = 0; i < nf[u]; i++) {
            int a = comp[u], b = comp[fwd[u][i]];
            if (a == b) continue;
            check(a < b, "components numbered topologically");
            if (!cedge[a][b]) cedge[a][b] = 1, cedges++;
        }
    int indeg[N] = {0}, outdeg[N] = {0}, size[N] = {0};
    for (int a = 0; a < nc; a++)
        for (int b = 0; b < nc; b++)
            if (cedge[a][b]) outdeg[a]++, indeg[b]++;
    for (int v = 0; v < N; v++) size[comp[v]]++;
    printf("components: %d, condensation edges: %d\n", nc, cedges);
    printf("source components:");
    for (int c = 0; c < nc; c++)
        if (!indeg[c]) printf(" %d(size %d)", c, size[c]);
    printf("\nsink components:");
    for (int c = 0; c < nc; c++)
        if (!outdeg[c]) printf(" %d(size %d)", c, size[c]);
    printf("\n");
    /* longest chain in the condensation via DP in numbering order */
    int longest[N];
    int best = 0;
    for (int c = 0; c < nc; c++) longest[c] = 1;
    for (int a = 0; a < nc; a++) {
        for (int b = a + 1; b < nc; b++)
            if (cedge[a][b] && longest[a] + 1 > longest[b]) longest[b] = longest[a] + 1;
        if (longest[a] > best) best = longest[a];
    }
    printf("longest chain of components: %d\n", best);
    int minadd = 0, srcs = 0, sinks = 0;
    for (int c = 0; c < nc; c++) srcs += !indeg[c], sinks += !outdeg[c];
    minadd = nc == 1 ? 0 : (srcs > sinks ? srcs : sinks);
    printf("edges needed to make strongly connected: %d\n", minadd);
    return 0;
}
