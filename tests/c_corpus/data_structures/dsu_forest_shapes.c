/*
 * title: Disjoint-set forest shapes under union and find heuristics
 * topic: data_structures
 * covers: disjoint-set forest, union by size and rank, path compression, halving, splitting, forest height statistics, component cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 4096

typedef enum { NAIVE, COMPRESS, HALVE, SPLIT } FindMode;
typedef enum { LINK_ARBITRARY, LINK_SIZE, LINK_RANK } LinkMode;

static int par[N], sz[N], rk[N];
static long hops;

static unsigned long long rs = 0xD5A7F04E57ULL * 8209;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static void init(void) { for (int i = 0; i < N; i++) { par[i] = i; sz[i] = 1; rk[i] = 0; } hops = 0; }
static int find(int x, FindMode m) {
    switch (m) {
    case NAIVE: while (par[x] != x) { x = par[x]; hops++; } return x;
    case COMPRESS: {
        int r = x;
        while (par[r] != r) { r = par[r]; hops++; }
        while (par[x] != r) { int n = par[x]; par[x] = r; x = n; }
        return r;
    }
    case HALVE: while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; hops++; } return x;
    default: while (par[x] != x) { int n = par[x]; par[x] = par[n]; x = n; hops++; } return x;
    }
}
static int unite(int a, int b, FindMode fm, LinkMode lm) {
    a = find(a, fm); b = find(b, fm);
    if (a == b) return 0;
    if (lm == LINK_SIZE && sz[a] < sz[b]) { int t = a; a = b; b = t; }
    else if (lm == LINK_RANK) { if (rk[a] < rk[b]) { int t = a; a = b; b = t; } if (rk[a] == rk[b]) rk[a]++; }
    else if (lm == LINK_ARBITRARY && (a > b)) { int t = a; a = b; b = t; } /* always attach larger id under smaller: deterministic, ignores size */
    par[b] = a; sz[a] += sz[b];
    return 1;
}
static int depth_of(int x) { int d = 0; while (par[x] != x) { x = par[x]; d++; } return d; }

/* reference: components by repeated relabelling over the edge list */
static int lab[N];
static void ref_components(int (*edges)[2], int m) {
    for (int i = 0; i < N; i++) lab[i] = i;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int e = 0; e < m; e++) {
            int a = lab[edges[e][0]], b = lab[edges[e][1]];
            if (a < b) { lab[edges[e][1]] = a; changed = 1; }
            else if (b < a) { lab[edges[e][0]] = b; changed = 1; }
        }
    }
}

int main(void) {
    static int edges[3 * N][2];
    int m = 0;
    /* a sparse random graph plus a long chain to provoke degenerate trees */
    for (int i = 0; i + 1 < 600; i++) { edges[m][0] = i; edges[m][1] = i + 1; m++; }
    for (int i = 0; i < 2200; i++) { edges[m][0] = 600 + (int)(rnd() % (N - 600)); edges[m][1] = 600 + (int)(rnd() % (N - 600)); m++; }
    ref_components(edges, m);
    int comps = 0; for (int i = 0; i < N; i++) comps += lab[i] == i;
    printf("graph: %d vertices, %d edges, %d components (reference relabelling)\n", N, m, comps);
    const char *fn[] = { "naive", "compress", "halving", "splitting" };
    const char *ln[] = { "arbitrary", "by-size", "by-rank" };
    for (int lm = 0; lm < 3; lm++) for (int fm = 0; fm < 4; fm++) {
        init();
        int merges = 0;
        for (int e = 0; e < m; e++) merges += unite(edges[e][0], edges[e][1], (FindMode)fm, (LinkMode)lm);
        check(merges == N - comps, "successful unions = n - components");
        /* same partition as the reference */
        for (int i = 0; i < N; i++) for (int t = 0; t < 2; t++) {
            int j = (int)(rnd() % N);
            check((find(i, NAIVE) == find(j, NAIVE)) == (lab[i] == lab[j]), "partition matches reference");
        }
        int maxd = 0; long sumd = 0;
        for (int i = 0; i < N; i++) { int d = depth_of(i); if (d > maxd) maxd = d; sumd += d; }
        int trees = 0; for (int i = 0; i < N; i++) trees += par[i] == i;
        check(trees == comps, "roots equal components");
        /* sizes at roots add up */
        int total = 0; for (int i = 0; i < N; i++) if (par[i] == i) total += sz[i];
        check(total == N, "sizes sum to n");
        printf("link=%-9s find=%-9s: height %3d, mean depth %d.%02d, pointer hops during build %ld\n",
               ln[lm], fn[fm], maxd, (int)(sumd / N), (int)(sumd * 100 / N % 100), hops);
        if (lm == LINK_RANK) {
            /* rank bound: a tree of rank r has at least 2^r nodes; height <= log2 n without compression */
            for (int i = 0; i < N; i++) if (par[i] == i) check((1 << rk[i]) <= sz[i], "rank bound 2^rank <= size");
        }
    }
    /* draw a tiny forest as a tree */
    init();
    int ex[][2] = { {1,2},{3,4},{1,3},{5,6},{7,8},{5,7},{1,5},{9,10} };
    for (unsigned i = 0; i < sizeof ex / sizeof ex[0]; i++) unite(ex[i][0], ex[i][1], NAIVE, LINK_SIZE);
    printf("small forest (by size, no compression), parent of 1..10:");
    for (int i = 1; i <= 10; i++) printf(" %d", par[i]);
    printf("\n");
    for (int i = 1; i <= 10; i++) find(i, COMPRESS);
    printf("after compressing every find:                        ");
    for (int i = 1; i <= 10; i++) printf(" %d", par[i]);
    printf("\n");
    return 0;
}
