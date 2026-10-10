/*
 * title: Bellman-Ford with negative cycle extraction
 * topic: algorithms
 * covers: Bellman-Ford, negative weights, negative cycle detection, cycle recovery via predecessors
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 30, M = 90, INF = 1 << 29 };

static unsigned st = 24680u;
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
static Edge E[M + 8];

/* returns -1 if no negative cycle reachable from s, else a vertex on one; fills cycle */
static int bellman_ford(int n, int m, int s, long *dist, int *par) {
    for (int i = 0; i < n; i++) dist[i] = INF, par[i] = -1;
    dist[s] = 0;
    int last = -1;
    for (int pass = 0; pass < n; pass++) {
        last = -1;
        for (int i = 0; i < m; i++) {
            if (dist[E[i].u] >= INF) continue;
            if (dist[E[i].u] + E[i].w < dist[E[i].v]) {
                dist[E[i].v] = dist[E[i].u] + E[i].w;
                par[E[i].v] = E[i].u;
                last = E[i].v;
            }
        }
        if (last < 0) return -1;
    }
    for (int i = 0; i < n; i++) last = par[last];
    return last;
}

int main(void) {
    long dist[N];
    int par[N];
    /* phase 1: potentials guarantee no negative cycle although edges are negative */
    int pot[N];
    for (int i = 0; i < N; i++) pot[i] = (int)(rnd() % 40);
    int m = 0;
    while (m < M) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        if (u == v) continue;
        int w = 1 + (int)(rnd() % 20);
        E[m++] = (Edge){u, v, w + pot[u] - pot[v]}; /* reduced weights, cycles keep positive sum */
    }
    int neg = 0;
    for (int i = 0; i < m; i++) neg += E[i].w < 0;
    int cyc = bellman_ford(N, m, 0, dist, par);
    check(cyc == -1, "no negative cycle with potentials");
    printf("edges %d, negative edges %d, no negative cycle\n", m, neg);
    int reach = 0;
    long sum = 0;
    for (int i = 0; i < N; i++)
        if (dist[i] < INF) reach++, sum += dist[i];
    printf("reachable %d, distance sum %ld, min distance ", reach, sum);
    long mn = 0;
    for (int i = 0; i < N; i++)
        if (dist[i] < INF && dist[i] < mn) mn = dist[i];
    printf("%ld\n", mn);
    /* check optimality condition */
    for (int i = 0; i < m; i++)
        if (dist[E[i].u] < INF) check(dist[E[i].u] + E[i].w >= dist[E[i].v], "triangle inequality");

    /* phase 2: plant a negative cycle among reachable vertices */
    int a = -1, b = -1, c = -1;
    for (int i = 1; i < N && c < 0; i++)
        if (dist[i] < INF) {
            if (a < 0) a = i;
            else if (b < 0) b = i;
            else c = i;
        }
    check(c >= 0, "three reachable vertices");
    E[m++] = (Edge){a, b, 2};
    E[m++] = (Edge){b, c, 2};
    E[m++] = (Edge){c, a, -1000};
    cyc = bellman_ford(N, m, 0, dist, par);
    check(cyc >= 0, "planted negative cycle found");
    int cycle[N + 1], len = 0, v = cyc;
    do {
        cycle[len++] = v;
        v = par[v];
    } while (v != cyc && len <= N);
    check(len <= N, "cycle closed");
    /* reverse to forward order and total weight */
    long total = 0;
    printf("negative cycle length %d:", len);
    for (int i = len - 1; i >= 0; i--) {
        printf(" %d", cycle[i]);
        int from = cycle[i], to = cycle[(i + len - 1) % len];
        long best = INF;
        for (int j = 0; j < m; j++)
            if (E[j].u == from && E[j].v == to && E[j].w < best) best = E[j].w;
        check(best < INF, "cycle edge exists");
        total += best;
    }
    printf("\nnegative cycle weight negative: %s\n", total < 0 ? "yes" : "no");
    check(total < 0, "cycle is negative");
    return 0;
}
