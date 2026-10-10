/*
 * title: Parity union-find for online bipartiteness
 * topic: data_structures
 * covers: DSU with parity, odd cycle detection, online edge insertion, path halving with parity, BFS 2-coloring oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 50

static unsigned long long rs = 0xB1BA27171EULL * 31;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int par[N], sz[N], rel[N]; /* rel[x]: parity of x relative to par[x] */

static int find(int x, int *parity) {
    int p = 0, r = x;
    while (par[r] != r) { p ^= rel[r]; r = par[r]; }
    /* compress: rewrite every node on the path to point at r with its own parity */
    int cur = x, cp = p;
    while (par[cur] != r && cur != r) {
        int nx = par[cur], np = cp ^ rel[cur];
        par[cur] = r; rel[cur] = cp;
        cur = nx; cp = np;
    }
    *parity = p;
    return r;
}
/* add edge (u,v); returns 1 if it keeps the graph bipartite, 0 if it closes an odd cycle */
static int add_edge(int u, int v) {
    int pu, pv, ru = find(u, &pu), rv = find(v, &pv);
    if (ru == rv) return pu != pv;
    if (sz[ru] < sz[rv]) { int t = ru; ru = rv; rv = t; }
    par[rv] = ru; rel[rv] = pu ^ pv ^ 1; sz[ru] += sz[rv];
    return 1;
}

static int eu[600], ev[600], ne;
static int oracle_bipartite(void) {
    int col[N]; memset(col, -1, sizeof col);
    int q[N];
    for (int s = 0; s < N; s++) {
        if (col[s] >= 0) continue;
        int h = 0, t = 0; q[t++] = s; col[s] = 0;
        while (h < t) {
            int u = q[h++];
            for (int i = 0; i < ne; i++) {
                int v = -1;
                if (eu[i] == u) v = ev[i]; else if (ev[i] == u) v = eu[i];
                if (v < 0) continue;
                if (col[v] < 0) { col[v] = col[u] ^ 1; q[t++] = v; }
                else if (col[v] == col[u]) return 0;
            }
        }
    }
    return 1;
}

int main(void) {
    int trials[4] = {30, 45, 60, 90};
    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < N; i++) { par[i] = i; sz[i] = 1; rel[i] = 0; }
        ne = 0;
        int first_odd = -1, added = 0;
        int target = trials[t];
        /* bias half the trials to bipartite by forcing edges across a hidden partition */
        int hidden = (t % 2 == 0);
        int side[N]; for (int i = 0; i < N; i++) side[i] = (int)(rnd() & 1u);
        for (int i = 0; i < target; i++) {
            int u = (int)(rnd() % N), v = (int)(rnd() % N);
            if (u == v) continue;
            if (hidden && side[u] == side[v] && i < target - 3) continue;
            eu[ne] = u; ev[ne] = v; ne++;
            int ok = add_edge(u, v); added++;
            int want = oracle_bipartite();
            if (first_odd < 0) {
                check(ok == 1 || want == 0, "odd cycle report agrees with oracle");
                if (!ok) first_odd = added;
                else check(want == 1, "bipartite so far");
            } else break;
            if (!ok) break;
        }
        int comps = 0, big = 0;
        for (int i = 0; i < N; i++) { int p; if (find(i, &p) == i) { comps++; if (sz[i] > big) big = sz[i]; } }
        printf("trial %d: edges=%d bipartite=%s first_odd_edge=%d comps=%d largest=%d\n",
               t, added, first_odd < 0 ? "yes" : "no", first_odd, comps, big);
    }
    /* pairs: same-side queries after building a bipartite forest */
    for (int i = 0; i < N; i++) { par[i] = i; sz[i] = 1; rel[i] = 0; }
    ne = 0;
    for (int i = 1; i < N; i++) { int u = i, v = (int)(rnd() % (unsigned)i); eu[ne] = u; ev[ne] = v; ne++; check(add_edge(u, v), "tree is bipartite"); }
    int same = 0;
    for (int u = 0; u < N; u++) for (int v = u + 1; v < N; v++) { int pu, pv; find(u, &pu); find(v, &pv); same += pu == pv; }
    printf("tree pairs on same side: %d of %d\n", same, N * (N - 1) / 2);
    return 0;
}
