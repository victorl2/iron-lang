/*
 * title: Floyd-Warshall with next-hop path recovery
 * topic: algorithms
 * covers: Floyd-Warshall, all-pairs shortest paths, next matrix, path reconstruction, diameter and center
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { N = 24, INF = 1 << 28 };

static unsigned st = 65537u;
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

static int w[N][N], d[N][N], nxt[N][N];

int main(void) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) w[i][j] = i == j ? 0 : INF;
    /* ring guarantees strong connectivity, chords add variety */
    for (int i = 0; i < N; i++) w[i][(i + 1) % N] = 5 + (int)(rnd() % 20);
    for (int k = 0; k < 40; k++) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        if (u != v) w[u][v] = 1 + (int)(rnd() % 30);
    }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            d[i][j] = w[i][j];
            nxt[i][j] = w[i][j] < INF ? j : -1;
        }
    for (int k = 0; k < N; k++)
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++)
                if (d[i][k] + d[k][j] < d[i][j]) {
                    d[i][j] = d[i][k] + d[k][j];
                    nxt[i][j] = nxt[i][k];
                }
    /* verify against single-source O(n^2) Dijkstra from every vertex */
    for (int s = 0; s < N; s++) {
        int dist[N], done[N];
        for (int i = 0; i < N; i++) dist[i] = INF, done[i] = 0;
        dist[s] = 0;
        for (int it = 0; it < N; it++) {
            int u = -1;
            for (int i = 0; i < N; i++)
                if (!done[i] && (u < 0 || dist[i] < dist[u])) u = i;
            done[u] = 1;
            for (int v = 0; v < N; v++)
                if (w[u][v] < INF && dist[u] + w[u][v] < dist[v]) dist[v] = dist[u] + w[u][v];
        }
        for (int t = 0; t < N; t++) check(dist[t] == d[s][t], "floyd equals dijkstra");
    }
    /* path recovery, verify each path's length equals d */
    long path_hops = 0;
    for (int s = 0; s < N; s++)
        for (int t = 0; t < N; t++) {
            int len = 0, u = s, hops = 0;
            while (u != t) {
                int v = nxt[u][t];
                check(v >= 0, "next defined");
                len += w[u][v];
                u = v;
                check(++hops <= N, "no loops");
            }
            check(len == d[s][t], "path length equals distance");
            path_hops += hops;
        }
    int diam = 0, da = 0, db = 0, rad = INF, center = 0;
    for (int i = 0; i < N; i++) {
        int ecc = 0;
        for (int j = 0; j < N; j++) {
            if (d[i][j] > ecc) ecc = d[i][j];
            if (d[i][j] > diam) diam = d[i][j], da = i, db = j;
        }
        if (ecc < rad) rad = ecc, center = i;
    }
    printf("diameter %d between %d and %d\n", diam, da, db);
    printf("radius %d at vertex %d\n", rad, center);
    printf("total hops over all pairs: %ld\n", path_hops);
    printf("path %d -> %d:", da, db);
    for (int u = da; u != db; u = nxt[u][db]) printf(" %d", u);
    printf(" %d\n", db);
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 12; j++) printf("%4d", d[i][j]);
        printf("\n");
    }
    return 0;
}
