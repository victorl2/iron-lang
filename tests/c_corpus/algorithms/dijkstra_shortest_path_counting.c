/*
 * title: Dijkstra with shortest path counting and DAG
 * topic: algorithms
 * covers: Dijkstra, path counting, predecessor sets, shortest-path DAG, modular counts
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { R = 8, C = 8, N = R * C, INF = 1 << 29 };
#define MOD 1000000007ull

static unsigned st = 8675309u;
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

static int cost[R][C];

/* Node weight graph: entering a cell costs its value. */
int main(void) {
    for (int r = 0; r < R; r++)
        for (int c = 0; c < C; c++) cost[r][c] = 1 + (int)(rnd() % 3);
    int dist[N], done[N];
    unsigned long long ways[N];
    for (int i = 0; i < N; i++) dist[i] = INF, done[i] = 0, ways[i] = 0;
    dist[0] = 0;
    ways[0] = 1;
    const int dr[4] = {-1, 0, 1, 0}, dc[4] = {0, 1, 0, -1};
    int order[N], no = 0;
    for (int it = 0; it < N; it++) {
        int u = -1;
        for (int i = 0; i < N; i++)
            if (!done[i] && dist[i] < INF && (u < 0 || dist[i] < dist[u])) u = i;
        if (u < 0) break;
        done[u] = 1;
        order[no++] = u;
        for (int d = 0; d < 4; d++) {
            int nr = u / C + dr[d], nc = u % C + dc[d];
            if (nr < 0 || nr >= R || nc < 0 || nc >= C) continue;
            int v = nr * C + nc, nd = dist[u] + cost[nr][nc];
            if (nd < dist[v]) {
                dist[v] = nd;
                ways[v] = ways[u];
            } else if (nd == dist[v]) {
                ways[v] = (ways[v] + ways[u]) % MOD;
            }
        }
    }
    check(no == N, "all cells settled");
    /* Verify by counting through the shortest-path DAG in dist order (edges have positive weight). */
    unsigned long long w2[N];
    memset(w2, 0, sizeof w2);
    w2[0] = 1;
    for (int k = 0; k < N; k++) {
        int u = order[k];
        for (int d = 0; d < 4; d++) {
            int nr = u / C + dr[d], nc = u % C + dc[d];
            if (nr < 0 || nr >= R || nc < 0 || nc >= C) continue;
            int v = nr * C + nc;
            if (dist[u] + cost[nr][nc] == dist[v]) w2[v] = (w2[v] + w2[u]) % MOD;
        }
    }
    int dag_edges = 0;
    for (int u = 0; u < N; u++) {
        check(w2[u] == ways[u] % MOD, "path counts agree");
        for (int d = 0; d < 4; d++) {
            int nr = u / C + dr[d], nc = u % C + dc[d];
            if (nr < 0 || nr >= R || nc < 0 || nc >= C) continue;
            dag_edges += dist[u] + cost[nr][nc] == dist[nr * C + nc];
        }
    }
    for (int r = 0; r < R; r++) {
        for (int c = 0; c < C; c++) printf("%d", cost[r][c]);
        printf("\n");
    }
    printf("cost to far corner: %d\n", dist[N - 1]);
    printf("shortest paths to far corner: %llu\n", ways[N - 1] % MOD);
    printf("shortest-path DAG edges: %d\n", dag_edges);
    for (int r = 0; r < R; r += 2) {
        for (int c = 0; c < C; c++) printf("%4llu", ways[r * C + c] % MOD);
        printf("\n");
    }
    return 0;
}
