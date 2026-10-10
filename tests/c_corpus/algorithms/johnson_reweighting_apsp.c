/*
 * title: Johnson all-pairs shortest paths
 * topic: algorithms
 * covers: Johnson's algorithm, Bellman-Ford potentials, edge reweighting, Dijkstra from each source, Floyd cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { N = 40, M = 160, INF = 1 << 28 };

static unsigned st = 909090u;
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
static Edge E[M + N];
static int hidden[N];

int main(void) {
    for (int i = 0; i < N; i++) hidden[i] = (int)(rnd() % 25);
    for (int i = 0; i < M; i++) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        E[i] = (Edge){u, v, (int)(rnd() % 12) + hidden[v] - hidden[u]};
    }
    int neg = 0;
    for (int i = 0; i < M; i++) neg += E[i].w < 0;
    /* virtual source N with zero edges to all */
    int h[N + 1];
    for (int i = 0; i <= N; i++) h[i] = INF;
    h[N] = 0;
    for (int i = 0; i < N; i++) E[M + i] = (Edge){N, i, 0};
    int passes = 0;
    for (int again = 1; again; passes++) {
        again = 0;
        check(passes <= N + 1, "no negative cycle");
        for (int i = 0; i < M + N; i++)
            if (h[E[i].u] < INF && h[E[i].u] + E[i].w < h[E[i].v]) h[E[i].v] = h[E[i].u] + E[i].w, again = 1;
    }
    for (int i = 0; i < M; i++) {
        E[i].w += h[E[i].u] - h[E[i].v];
        check(E[i].w >= 0, "reweighted edge nonnegative");
    }
    static int dist[N][N], fw[N][N];
    for (int s = 0; s < N; s++) {
        int done[N];
        for (int i = 0; i < N; i++) dist[s][i] = INF, done[i] = 0;
        dist[s][s] = 0;
        for (int it = 0; it < N; it++) {
            int u = -1;
            for (int i = 0; i < N; i++)
                if (!done[i] && (u < 0 || dist[s][i] < dist[s][u])) u = i;
            if (dist[s][u] >= INF) break;
            done[u] = 1;
            for (int e = 0; e < M; e++)
                if (E[e].u == u && dist[s][u] + E[e].w < dist[s][E[e].v]) dist[s][E[e].v] = dist[s][u] + E[e].w;
        }
        /* undo reweighting */
        for (int t = 0; t < N; t++)
            if (dist[s][t] < INF) dist[s][t] += h[t] - h[s];
    }
    /* Floyd reference on original weights */
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) fw[i][j] = i == j ? 0 : INF;
    for (int e = 0; e < M; e++) {
        int w0 = E[e].w - h[E[e].u] + h[E[e].v];
        if (w0 < fw[E[e].u][E[e].v]) fw[E[e].u][E[e].v] = w0;
    }
    for (int k = 0; k < N; k++)
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++)
                if (fw[i][k] < INF && fw[k][j] < INF && fw[i][k] + fw[k][j] < fw[i][j]) fw[i][j] = fw[i][k] + fw[k][j];
    long sum = 0;
    int reach = 0, mn = 0, mx = 0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            check(dist[i][j] == fw[i][j], "johnson equals floyd");
            if (dist[i][j] < INF) {
                reach++;
                sum += dist[i][j];
                if (dist[i][j] < mn) mn = dist[i][j];
                if (dist[i][j] > mx) mx = dist[i][j];
            }
        }
    printf("edges %d, negative %d, bellman-ford passes %d\n", M, neg, passes);
    printf("reachable pairs %d, sum %ld, min %d, max %d\n", reach, sum, mn, mx);
    printf("potentials:");
    for (int i = 0; i < 10; i++) printf(" %d", h[i]);
    printf("\n");
    return 0;
}
