/*
 * title: Tarjan strongly connected components
 * topic: algorithms
 * covers: Tarjan SCC, lowlink, explicit stack, reachability matrix cross-check, component sizes
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 40, M = 70 };

static unsigned st = 31337u;
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
static int idx[N], low[N], onstack[N], comp[N], stack_[N], sp, counter, ncomp;

static void strong(int u) {
    idx[u] = low[u] = ++counter;
    stack_[sp++] = u;
    onstack[u] = 1;
    for (int i = 0; i < deg[u]; i++) {
        int v = adj[u][i];
        if (!idx[v]) {
            strong(v);
            if (low[v] < low[u]) low[u] = low[v];
        } else if (onstack[v] && idx[v] < low[u])
            low[u] = idx[v];
    }
    if (low[u] == idx[u]) {
        int v;
        do {
            v = stack_[--sp];
            onstack[v] = 0;
            comp[v] = ncomp;
        } while (v != u);
        ncomp++;
    }
}

int main(void) {
    unsigned char reach[N][N];
    memset(reach, 0, sizeof reach);
    for (int i = 0; i < N; i++) reach[i][i] = 1;
    for (int i = 0; i < M; i++) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        adj[u][deg[u]++] = v;
        reach[u][v] = 1;
    }
    for (int u = 0; u < N; u++)
        if (!idx[u]) strong(u);
    for (int k = 0; k < N; k++)
        for (int i = 0; i < N; i++)
            if (reach[i][k])
                for (int j = 0; j < N; j++) reach[i][j] |= reach[k][j];
    for (int a = 0; a < N; a++)
        for (int b = 0; b < N; b++)
            check((comp[a] == comp[b]) == (reach[a][b] && reach[b][a]), "scc equals mutual reachability");
    /* Tarjan emits components in reverse topological order: edges go to lower or equal ids */
    for (int u = 0; u < N; u++)
        for (int i = 0; i < deg[u]; i++) check(comp[adj[u][i]] <= comp[u], "reverse topological ids");
    int size[N] = {0};
    for (int v = 0; v < N; v++) size[comp[v]]++;
    int biggest = 0, singles = 0;
    for (int c = 0; c < ncomp; c++) {
        if (size[c] > biggest) biggest = size[c];
        singles += size[c] == 1;
    }
    printf("vertices %d edges %d\n", N, M);
    printf("components: %d, singletons: %d, largest: %d\n", ncomp, singles, biggest);
    for (int c = 0; c < ncomp; c++) {
        if (size[c] < 2) continue;
        printf("component %d:", c);
        for (int v = 0; v < N; v++)
            if (comp[v] == c) printf(" %d", v);
        printf("\n");
    }
    return 0;
}
