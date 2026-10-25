/*
 * title: Edge deletions answered by reversing time with union-find
 * topic: data_structures
 * covers: offline deletion, reverse processing, DSU, component count timeline, largest component, brute-force recomputation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 70
#define M 160

static unsigned long long rs = 0x0FF11EDE1ULL * 7;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int par[N], sz[N], comps, biggest;
static int find(int x) { while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; } return x; }
static void init(void) { for (int i = 0; i < N; i++) { par[i] = i; sz[i] = 1; } comps = N; biggest = 1; }
static void unite(int a, int b) {
    a = find(a); b = find(b);
    if (a == b) return;
    if (sz[a] < sz[b]) { int t = a; a = b; b = t; }
    par[b] = a; sz[a] += sz[b]; comps--;
    if (sz[a] > biggest) biggest = sz[a];
}

static int eu[M], ev[M], order[M], alive[M];

/* brute force: DFS over alive edges */
static void brute(int *cc, int *big) {
    int seen[N] = {0}, st[N * 4];
    *cc = 0; *big = 0;
    for (int s = 0; s < N; s++) {
        if (seen[s]) continue;
        int sp = 0, cnt = 0; st[sp++] = s; seen[s] = 1; (*cc)++;
        while (sp) {
            int u = st[--sp]; cnt++;
            for (int e = 0; e < M; e++) {
                if (!alive[e]) continue;
                int v = -1;
                if (eu[e] == u) v = ev[e]; else if (ev[e] == u) v = eu[e];
                if (v >= 0 && !seen[v]) { seen[v] = 1; st[sp++] = v; }
            }
        }
        if (cnt > *big) *big = cnt;
    }
}

int main(void) {
    for (int e = 0; e < M; e++) {
        eu[e] = (int)(rnd() % N); ev[e] = (int)(rnd() % N); order[e] = e;
    }
    for (int i = M - 1; i > 0; i--) { int j = (int)(rnd() % (unsigned)(i + 1)); int t = order[i]; order[i] = order[j]; order[j] = t; }
    /* delete edges in `order`; ans[k] = state after k deletions */
    int ansc[M + 1], ansb[M + 1];
    init();
    /* reverse: start with all edges deleted -> state after M deletions is no edges, then re-add backwards */
    ansc[M] = comps; ansb[M] = biggest;
    for (int k = M - 1; k >= 0; k--) {
        int e = order[k];
        unite(eu[e], ev[e]);
        ansc[k] = comps; ansb[k] = biggest;
    }
    /* verify by brute force after each forward deletion */
    for (int e = 0; e < M; e++) alive[e] = 1;
    int splits = 0, prev = ansc[0];
    for (int k = 0; k <= M; k++) {
        int cc, big;
        brute(&cc, &big);
        check(cc == ansc[k] && big == ansb[k], "reverse DSU matches brute force");
        if (ansc[k] > prev) splits++;
        prev = ansc[k];
        if (k < M) alive[order[k]] = 0;
    }
    printf("edges=%d vertices=%d initial_comps=%d initial_largest=%d\n", M, N, ansc[0], ansb[0]);
    for (int k = 0; k <= M; k += 20) printf("after %3d deletions: comps=%2d largest=%2d\n", k, ansc[k], ansb[k]);
    /* the first deletion that disconnects the giant component */
    int first = -1;
    for (int k = 1; k <= M; k++) if (ansb[k] < ansb[k - 1]) { first = k; break; }
    printf("component-count increases: %d, first drop of giant component at deletion %d (%d -> %d)\n",
           splits, first, first > 0 ? ansb[first - 1] : 0, first > 0 ? ansb[first] : 0);
    return 0;
}
