/*
 * title: Min-cost max-flow by successive shortest paths
 * topic: algorithms
 * covers: min-cost flow, successive shortest paths, Bellman-Ford residual costs, negative reverse edges, transportation problem
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAXN = 40, MAXE = 400, INF = 1 << 28 };

static unsigned st = 4711u;
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
    int to, cap, cost;
} Arc;
static Arc arcs[MAXE];
static int nxt[MAXE], head[MAXN], na, nv;

static void add_edge(int u, int v, int cap, int cost) {
    arcs[na] = (Arc){v, cap, cost};
    nxt[na] = head[u];
    head[u] = na++;
    arcs[na] = (Arc){u, 0, -cost};
    nxt[na] = head[v];
    head[v] = na++;
}

static long mcmf(int s, int t, long *flow_out, int *paths) {
    long cost = 0, flow = 0;
    *paths = 0;
    for (;;) {
        int dist[MAXN], pe[MAXN], inq[MAXN], q[MAXN + 1], qh = 0, qt = 0;
        for (int i = 0; i < nv; i++) dist[i] = INF, pe[i] = -1, inq[i] = 0;
        dist[s] = 0;
        q[qt++] = s;
        while (qh != qt) {
            int u = q[qh];
            qh = (qh + 1) % (MAXN + 1);
            inq[u] = 0;
            for (int e = head[u]; e != -1; e = nxt[e])
                if (arcs[e].cap > 0 && dist[u] + arcs[e].cost < dist[arcs[e].to]) {
                    dist[arcs[e].to] = dist[u] + arcs[e].cost;
                    pe[arcs[e].to] = e;
                    if (!inq[arcs[e].to]) inq[arcs[e].to] = 1, q[qt] = arcs[e].to, qt = (qt + 1) % (MAXN + 1);
                }
        }
        if (dist[t] >= INF) break;
        int f = INF;
        for (int v = t; v != s; v = arcs[pe[v] ^ 1].to)
            if (arcs[pe[v]].cap < f) f = arcs[pe[v]].cap;
        for (int v = t; v != s; v = arcs[pe[v] ^ 1].to) arcs[pe[v]].cap -= f, arcs[pe[v] ^ 1].cap += f;
        flow += f;
        cost += (long)f * dist[t];
        (*paths)++;
    }
    *flow_out = flow;
    return cost;
}

int main(void) {
    /* transportation: S suppliers, D consumers */
    enum { S = 6, D = 7 };
    int supply[S], demand[D], c[S][D];
    int tot_s = 0, tot_d = 0;
    for (int i = 0; i < S; i++) supply[i] = 5 + (int)(rnd() % 10), tot_s += supply[i];
    for (int j = 0; j < D; j++) demand[j] = 4 + (int)(rnd() % 9), tot_d += demand[j];
    for (int i = 0; i < S; i++)
        for (int j = 0; j < D; j++) c[i][j] = 1 + (int)(rnd() % 30);
    memset(head, -1, sizeof head);
    nv = S + D + 2;
    int src = S + D, snk = S + D + 1;
    for (int i = 0; i < S; i++) add_edge(src, i, supply[i], 0);
    for (int j = 0; j < D; j++) add_edge(S + j, snk, demand[j], 0);
    int first_mid = na;
    for (int i = 0; i < S; i++)
        for (int j = 0; j < D; j++) add_edge(i, S + j, INF, c[i][j]);
    long flow;
    int paths;
    long cost = mcmf(src, snk, &flow, &paths);
    check(flow == (tot_s < tot_d ? tot_s : tot_d), "flow saturates smaller side");
    long recomputed = 0;
    int shipped[S][D], out[S] = {0}, in[D] = {0};
    for (int i = 0; i < S; i++)
        for (int j = 0; j < D; j++) {
            int e = first_mid + 2 * (i * D + j);
            shipped[i][j] = arcs[e ^ 1].cap;
            recomputed += (long)shipped[i][j] * c[i][j];
            out[i] += shipped[i][j];
            in[j] += shipped[i][j];
        }
    check(recomputed == cost, "cost equals sum of shipments times unit cost");
    for (int i = 0; i < S; i++) check(out[i] <= supply[i], "supply respected");
    for (int j = 0; j < D; j++) check(in[j] <= demand[j], "demand respected");
    /* optimality: no negative cycle in residual graph of the mid edges (check via Bellman-Ford) */
    int dist[MAXN];
    for (int i = 0; i < nv; i++) dist[i] = 0;
    for (int pass = 0; pass <= nv; pass++) {
        int changed = 0;
        for (int u = 0; u < nv; u++)
            for (int e = head[u]; e != -1; e = nxt[e])
                if (arcs[e].cap > 0 && dist[u] + arcs[e].cost < dist[arcs[e].to]) dist[arcs[e].to] = dist[u] + arcs[e].cost, changed = 1;
        if (!changed) break;
        check(pass < nv, "no negative residual cycle (optimal)");
    }
    printf("supply %d, demand %d, shipped %ld, cost %ld, augmenting paths %d\n", tot_s, tot_d, flow, cost, paths);
    for (int i = 0; i < S; i++) {
        for (int j = 0; j < D; j++) printf("%3d", shipped[i][j]);
        printf("  | %2d/%2d\n", out[i], supply[i]);
    }
    return 0;
}
