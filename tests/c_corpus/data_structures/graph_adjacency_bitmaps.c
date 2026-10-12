/*
 * title: Adjacency bitmaps for triangles and k-cliques
 * topic: data_structures
 * covers: adjacency bitmap rows, 64-bit words, popcount by table, triangle counting, k-clique counting, greedy clique, brute-force oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define N 100
#define W 2 /* words per row */

static unsigned long long rs = 0xB17B17ULL * 65537;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { uint64_t w[W]; } Row;
static Row adj[N];
static unsigned char adjm[N][N];

static int pop64(uint64_t x) {
    int c = 0;
    while (x) { x &= x - 1; c++; }
    return c;
}
static int pop(const Row *r) { int c = 0; for (int i = 0; i < W; i++) c += pop64(r->w[i]); return c; }
static Row and_row(const Row *a, const Row *b) { Row r; for (int i = 0; i < W; i++) r.w[i] = a->w[i] & b->w[i]; return r; }
static int test(const Row *r, int v) { return (int)((r->w[v >> 6] >> (v & 63)) & 1u); }
static int lowest(const Row *r) {
    for (int i = 0; i < W; i++) if (r->w[i]) { uint64_t x = r->w[i]; int b = 0; while (!(x & 1u)) { x >>= 1; b++; } return i * 64 + b; }
    return -1;
}
static void clear_bit(Row *r, int v) { r->w[v >> 6] &= ~((uint64_t)1 << (v & 63)); }

/* count k-cliques whose vertices are all in `cand` and > previous, by recursive candidate intersection */
static long kcliques(Row cand, int k) {
    if (k == 0) return 1;
    long total = 0;
    Row c = cand;
    for (int v = lowest(&c); v >= 0; v = lowest(&c)) {
        clear_bit(&c, v);
        if (k == 1) { total++; continue; }
        Row nc = and_row(&c, &adj[v]);   /* only larger-than-v candidates remain in c */
        total += kcliques(nc, k - 1);
    }
    return total;
}
static long brute_k(int k) {
    long total = 0;
    int idx[6];
    /* enumerate increasing tuples with matrix checks */
    int depth = 0; idx[0] = -1;
    while (depth >= 0) {
        idx[depth]++;
        if (idx[depth] >= N) { depth--; continue; }
        int ok = 1;
        for (int i = 0; i < depth; i++) if (!adjm[idx[i]][idx[depth]]) { ok = 0; break; }
        if (!ok) continue;
        if (depth + 1 == k) { total++; continue; }
        depth++; idx[depth] = idx[depth - 1];
    }
    return total;
}

int main(void) {
    int pcts[3] = {8, 20, 45};
    for (int t = 0; t < 3; t++) {
        memset(adj, 0, sizeof adj); memset(adjm, 0, sizeof adjm);
        int edges = 0;
        for (int u = 0; u < N; u++) for (int v = u + 1; v < N; v++) {
            unsigned r = rnd() % 100;
            if ((int)r < pcts[t]) {
                adj[u].w[v >> 6] |= (uint64_t)1 << (v & 63); adj[v].w[u >> 6] |= (uint64_t)1 << (u & 63);
                adjm[u][v] = adjm[v][u] = 1; edges++;
            }
        }
        int degsum = 0;
        for (int u = 0; u < N; u++) { degsum += pop(&adj[u]); check(!test(&adj[u], u), "no self loop"); }
        check(degsum == 2 * edges, "degree sum");
        /* triangles via common-neighbour popcounts, each counted 3 times over ordered-edge scan */
        long tri3 = 0;
        for (int u = 0; u < N; u++) for (int v = u + 1; v < N; v++) if (test(&adj[u], v)) { Row c = and_row(&adj[u], &adj[v]); tri3 += pop(&c); }
        check(tri3 % 3 == 0, "triangle multiple of 3");
        long kc[5];
        for (int k = 2; k <= 4; k++) {
            Row all; memset(&all, 0, sizeof all);
            for (int v = 0; v < N; v++) all.w[v >> 6] |= (uint64_t)1 << (v & 63);
            kc[k] = kcliques(all, k);
            check(kc[k] == brute_k(k), "k-clique vs brute force");
        }
        check(kc[2] == edges && kc[3] == tri3 / 3, "k=2 edges and k=3 triangles");
        /* greedy clique: repeatedly take the candidate with max remaining candidate-degree */
        Row cand; memset(&cand, 0, sizeof cand);
        for (int v = 0; v < N; v++) cand.w[v >> 6] |= (uint64_t)1 << (v & 63);
        int size = 0, members[N];
        while (pop(&cand)) {
            int best = -1, bd = -1;
            for (int v = 0; v < N; v++) if (test(&cand, v)) { Row x = and_row(&cand, &adj[v]); int d = pop(&x); if (d > bd) { bd = d; best = v; } }
            members[size++] = best; cand = and_row(&cand, &adj[best]);
        }
        for (int i = 0; i < size; i++) for (int j = i + 1; j < size; j++) check(adjm[members[i]][members[j]], "greedy is clique");
        printf("p=%2d%% edges=%4d triangles=%5ld k4=%6ld greedy_clique=%d\n", pcts[t], edges, kc[3], kc[4], size);
    }
    return 0;
}
