/*
 * title: Boruvka MST rounds
 * topic: algorithms
 * covers: Boruvka, cheapest edge per component, component halving, tie-breaking by edge index, Kruskal check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { N = 128, M = 700 };

static unsigned st = 616161u;
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
    while (par[x] != x) par[x] = par[par[x]], x = par[x];
    return x;
}
/* total order on edges: weight then index */
static int better(int a, int b) {
    if (b < 0) return 1;
    if (E[a].w != E[b].w) return E[a].w < E[b].w;
    return a < b;
}
static int cmp(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return better(x, y) ? -1 : 1;
}

int main(void) {
    int m = 0;
    for (int i = 1; i < N; i++) E[m++] = (Edge){(int)(rnd() % (unsigned)i), i, 1 + (int)(rnd() % 100)};
    while (m < M) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        if (u != v) E[m++] = (Edge){u, v, 1 + (int)(rnd() % 100)}; /* many equal weights */
    }
    for (int i = 0; i < N; i++) par[i] = i;
    int comps = N, round = 0;
    long total = 0;
    int used[M] = {0}, taken = 0;
    while (comps > 1) {
        int cheap[N];
        for (int i = 0; i < N; i++) cheap[i] = -1;
        for (int i = 0; i < M; i++) {
            int a = find(E[i].u), b = find(E[i].v);
            if (a == b) continue;
            if (better(i, cheap[a])) cheap[a] = i;
            if (better(i, cheap[b])) cheap[b] = i;
        }
        int merged = 0;
        long round_w = 0;
        for (int c = 0; c < N; c++) {
            if (cheap[c] < 0) continue;
            int e = cheap[c], a = find(E[e].u), b = find(E[e].v);
            if (a == b) continue;
            par[a] = b;
            comps--;
            merged++;
            total += E[e].w;
            round_w += E[e].w;
            used[e] = 1;
            taken++;
        }
        round++;
        printf("round %d: merged %d, components left %d, added weight %ld\n", round, merged, comps, round_w);
        check(merged > 0, "progress each round");
    }
    check(taken == N - 1, "n-1 edges");
    /* Kruskal reference using the same total order */
    int idx[M];
    for (int i = 0; i < M; i++) idx[i] = i;
    qsort(idx, M, sizeof(int), cmp);
    for (int i = 0; i < N; i++) par[i] = i;
    long kw = 0;
    int kused[M] = {0};
    for (int k = 0; k < M; k++) {
        int e = idx[k], a = find(E[e].u), b = find(E[e].v);
        if (a != b) par[a] = b, kw += E[e].w, kused[e] = 1;
    }
    check(kw == total, "same weight as Kruskal");
    for (int i = 0; i < M; i++) check(used[i] == kused[i], "same edge set with strict tie-break");
    check(round <= 8, "at most log2(n)+1 rounds");
    printf("total weight %ld in %d rounds\n", total, round);
    return 0;
}
