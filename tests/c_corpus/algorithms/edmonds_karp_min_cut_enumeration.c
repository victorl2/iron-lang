/*
 * title: Edmonds-Karp on a matrix with brute-force min cut
 * topic: algorithms
 * covers: Edmonds-Karp, residual matrix, shortest augmenting paths, cut enumeration over subsets, max-flow min-cut theorem
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 14 };

static unsigned st = 5040u;
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

static int cap[N][N], res[N][N];

static int bits(unsigned x) {
    int c = 0;
    for (; x; x &= x - 1) c++;
    return c;
}

static int edmonds_karp(int s, int t, int *augs, int *path_hops) {
    int flow = 0;
    memcpy(res, cap, sizeof res);
    *augs = *path_hops = 0;
    for (;;) {
        int par[N], q[N], qh = 0, qt = 0;
        for (int i = 0; i < N; i++) par[i] = -1;
        par[s] = s;
        q[qt++] = s;
        while (qh < qt && par[t] < 0) {
            int u = q[qh++];
            for (int v = 0; v < N; v++)
                if (par[v] < 0 && res[u][v] > 0) par[v] = u, q[qt++] = v;
        }
        if (par[t] < 0) break;
        int bott = 1 << 28, hops = 0;
        for (int v = t; v != s; v = par[v]) {
            if (res[par[v]][v] < bott) bott = res[par[v]][v];
            hops++;
        }
        for (int v = t; v != s; v = par[v]) res[par[v]][v] -= bott, res[v][par[v]] += bott;
        flow += bott;
        (*augs)++;
        *path_hops += hops;
    }
    return flow;
}

int main(void) {
    int total_edges = 0;
    for (int trial = 0; trial < 6; trial++) {
        memset(cap, 0, sizeof cap);
        int density = 15 + trial * 8, edges = 0;
        for (int u = 0; u < N; u++)
            for (int v = 0; v < N; v++)
                if (u != v && rnd() % 100u < (unsigned)density) cap[u][v] = 1 + (int)(rnd() % 20), edges++;
        int s = 0, t = N - 1, augs, hops;
        int flow = edmonds_karp(s, t, &augs, &hops);
        /* enumerate all cuts: subsets of inner vertices join the source side */
        int best = 1 << 28, best_mask = 0;
        for (unsigned mask = 0; mask < (1u << (N - 2)); mask++) {
            unsigned side = 1u | (mask << 1); /* vertex 0 plus inner vertices 1..N-2 */
            int c = 0;
            for (int u = 0; u < N; u++)
                if (side >> u & 1u)
                    for (int v = 0; v < N; v++)
                        if (!(side >> v & 1u)) c += cap[u][v];
            if (c < best) best = c, best_mask = (int)side;
        }
        check(flow == best, "flow equals brute-force min cut");
        /* residual reachability gives a minimum cut too */
        int reach[N] = {0}, stack[N], sp = 0;
        reach[s] = 1;
        stack[sp++] = s;
        while (sp) {
            int u = stack[--sp];
            for (int v = 0; v < N; v++)
                if (!reach[v] && res[u][v] > 0) reach[v] = 1, stack[sp++] = v;
        }
        int rc = 0, rs = 0;
        for (int u = 0; u < N; u++) {
            rs += reach[u];
            for (int v = 0; v < N; v++)
                if (reach[u] && !reach[v]) rc += cap[u][v];
        }
        check(rc == flow, "residual cut equals flow");
        printf("trial %d: edges %3d flow %3d augmentations %2d avg hops %d.%d source side %d (brute side %d)\n", trial, edges, flow,
               augs, augs ? hops / augs : 0, augs ? hops * 10 / augs % 10 : 0, rs, bits((unsigned)best_mask));
        total_edges += edges;
    }
    printf("total edges %d\n", total_edges);
    return 0;
}
