/*
 * title: Capacity scaling max flow
 * topic: algorithms
 * covers: Ford-Fulkerson, capacity scaling, delta phases, DFS augmentation, comparison with unit-BFS augmentation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 30 };

static unsigned st = 7331u;
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

static int cap[N][N], res[N][N], seen[N];

static int dfs(int u, int t, int delta, int f) {
    if (u == t) return f;
    seen[u] = 1;
    for (int v = 0; v < N; v++)
        if (!seen[v] && res[u][v] >= delta) {
            int d = dfs(v, t, delta, f < res[u][v] ? f : res[u][v]);
            if (d > 0) {
                res[u][v] -= d;
                res[v][u] += d;
                return d;
            }
        }
    return 0;
}

static int simple_flow(int s, int t) {
    static int r[N][N];
    memcpy(r, cap, sizeof r);
    int flow = 0;
    for (;;) {
        int par[N], q[N], qh = 0, qt = 0;
        memset(par, -1, sizeof par);
        par[s] = s;
        q[qt++] = s;
        while (qh < qt) {
            int u = q[qh++];
            for (int v = 0; v < N; v++)
                if (par[v] < 0 && r[u][v] > 0) par[v] = u, q[qt++] = v;
        }
        if (par[t] < 0) return flow;
        int b = 1 << 30;
        for (int v = t; v != s; v = par[v])
            if (r[par[v]][v] < b) b = r[par[v]][v];
        for (int v = t; v != s; v = par[v]) r[par[v]][v] -= b, r[v][par[v]] += b;
        flow += b;
    }
}

int main(void) {
    for (int trial = 0; trial < 4; trial++) {
        memset(cap, 0, sizeof cap);
        int big = trial % 2 ? 1000000 : 1000; /* huge capacities show why scaling helps */
        for (int u = 0; u < N; u++)
            for (int v = 0; v < N; v++)
                if (u != v && rnd() % 100 < 14) cap[u][v] = 1 + (int)(rnd() % (unsigned)big);
        memcpy(res, cap, sizeof res);
        int maxcap = 0;
        for (int u = 0; u < N; u++)
            for (int v = 0; v < N; v++)
                if (cap[u][v] > maxcap) maxcap = cap[u][v];
        int delta = 1;
        while (delta * 2 <= maxcap) delta *= 2;
        long flow = 0;
        int augs = 0, phases = 0;
        for (; delta >= 1; delta /= 2) {
            phases++;
            for (;;) {
                memset(seen, 0, sizeof seen);
                int f = dfs(0, N - 1, delta, 1 << 30);
                if (f <= 0) break;
                flow += f;
                augs++;
            }
        }
        long ref = simple_flow(0, N - 1);
        check(flow == ref, "scaling equals BFS augmentation");
        printf("trial %d: max capacity %7d, phases %2d, augmentations %3d, flow %ld\n", trial, maxcap, phases, augs, flow);
    }
    return 0;
}
