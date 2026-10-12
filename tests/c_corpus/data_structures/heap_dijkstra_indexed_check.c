/*
 * title: Dijkstra with an indexed heap versus lazy insertion
 * topic: data_structures
 * covers: indexed priority queue, decrease-key, lazy insertion variant, predecessor paths, Floyd-Warshall cross-check, operation counting
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0xD135ull;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

enum { N = 120, INF = 1 << 28 };

typedef struct {
    int to, w;
} Edge;

static Edge adj[N][12];
static int deg[N];
static int dense[N][N];

typedef struct {
    int heap[N], pos[N], n;
    const int *key;
    long decreases, sifts;
} Idx;

static void ip_swap(Idx *q, int i, int j) {
    int t = q->heap[i];
    q->heap[i] = q->heap[j];
    q->heap[j] = t;
    q->pos[q->heap[i]] = i;
    q->pos[q->heap[j]] = j;
    q->sifts++;
}

static void ip_up(Idx *q, int i) {
    while (i > 0 && q->key[q->heap[i]] < q->key[q->heap[(i - 1) / 2]]) {
        ip_swap(q, i, (i - 1) / 2);
        i = (i - 1) / 2;
    }
}

static void ip_down(Idx *q, int i) {
    for (;;) {
        int c = 2 * i + 1;
        if (c >= q->n)
            break;
        if (c + 1 < q->n && q->key[q->heap[c + 1]] < q->key[q->heap[c]])
            c++;
        if (q->key[q->heap[c]] >= q->key[q->heap[i]])
            break;
        ip_swap(q, i, c);
        i = c;
    }
}

static void dijkstra_indexed(int src, int *dist, int *prev, long *dec, long *pops) {
    Idx q;
    q.n = 0;
    q.key = dist;
    q.decreases = q.sifts = 0;
    for (int i = 0; i < N; i++) {
        dist[i] = INF;
        prev[i] = -1;
        q.pos[i] = -1;
    }
    dist[src] = 0;
    q.heap[q.n] = src;
    q.pos[src] = q.n++;
    *pops = 0;
    while (q.n > 0) {
        int u = q.heap[0];
        ip_swap(&q, 0, q.n - 1);
        q.n--;
        q.pos[u] = -2; /* settled */
        ip_down(&q, 0);
        (*pops)++;
        for (int k = 0; k < deg[u]; k++) {
            int v = adj[u][k].to, nd = dist[u] + adj[u][k].w;
            if (q.pos[v] == -2 || nd >= dist[v])
                continue;
            dist[v] = nd;
            prev[v] = u;
            if (q.pos[v] == -1) {
                q.heap[q.n] = v;
                q.pos[v] = q.n++;
            } else
                q.decreases++;
            ip_up(&q, q.pos[v]);
        }
    }
    *dec = q.decreases;
}

typedef struct {
    int d, v;
} Ent;

static void dijkstra_lazy(int src, int *dist, long *pushes, long *stale) {
    static Ent h[N * 12 + 1];
    int n = 0;
    for (int i = 0; i < N; i++)
        dist[i] = INF;
    dist[src] = 0;
    h[n++] = (Ent){0, src};
    *pushes = 1;
    *stale = 0;
    while (n > 0) {
        Ent top = h[0], x = h[--n];
        int i = 0;
        for (;;) {
            int c = 2 * i + 1;
            if (c >= n)
                break;
            if (c + 1 < n && h[c + 1].d < h[c].d)
                c++;
            if (h[c].d >= x.d)
                break;
            h[i] = h[c];
            i = c;
        }
        if (n > 0)
            h[i] = x;
        if (top.d > dist[top.v]) {
            (*stale)++;
            continue;
        }
        for (int k = 0; k < deg[top.v]; k++) {
            int v = adj[top.v][k].to, nd = top.d + adj[top.v][k].w;
            if (nd < dist[v]) {
                dist[v] = nd;
                int j = n++;
                while (j > 0 && nd < h[(j - 1) / 2].d) {
                    h[j] = h[(j - 1) / 2];
                    j = (j - 1) / 2;
                }
                h[j] = (Ent){nd, v};
                (*pushes)++;
            }
        }
    }
}

int main(void) {
    for (int graph = 0; graph < 3; graph++) {
        memset(deg, 0, sizeof deg);
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++)
                dense[i][j] = i == j ? 0 : INF;
        int per_node = graph == 0 ? 3 : graph == 1 ? 6 : 11;
        for (int u = 0; u < N; u++)
            for (int k = 0; k < per_node; k++) {
                int v = (int)(rng() % N), w = 1 + (int)(rng() % 50);
                if (v == u || dense[u][v] < INF)
                    continue;
                dense[u][v] = w;
                adj[u][deg[u]].to = v;
                adj[u][deg[u]].w = w;
                deg[u]++;
            }
        /* Floyd-Warshall reference */
        static int fw[N][N];
        memcpy(fw, dense, sizeof fw);
        for (int k = 0; k < N; k++)
            for (int i = 0; i < N; i++)
                for (int j = 0; j < N; j++)
                    if (fw[i][k] < INF && fw[k][j] < INF && fw[i][k] + fw[k][j] < fw[i][j])
                        fw[i][j] = fw[i][k] + fw[k][j];
        long total_dec = 0, total_pops = 0, total_push = 0, total_stale = 0, reach = 0, distsum = 0;
        for (int src = 0; src < N; src += 7) {
            int d1[N], d2[N], prev[N];
            long dec, pops, pushes, stale;
            dijkstra_indexed(src, d1, prev, &dec, &pops);
            dijkstra_lazy(src, d2, &pushes, &stale);
            for (int v = 0; v < N; v++) {
                check(d1[v] == fw[src][v], "indexed Dijkstra equals Floyd-Warshall");
                check(d2[v] == fw[src][v], "lazy Dijkstra equals Floyd-Warshall");
                if (d1[v] < INF) {
                    reach++;
                    distsum += d1[v];
                    /* walk the predecessor chain and re-add the edge weights */
                    int len = 0, u = v;
                    while (u != src) {
                        int p = prev[u];
                        check(p >= 0, "predecessor exists for reachable node");
                        len += dense[p][u];
                        u = p;
                    }
                    check(len == d1[v], "path weight equals distance");
                }
            }
            total_dec += dec;
            total_pops += pops;
            total_push += pushes;
            total_stale += stale;
        }
        printf("graph %d (out-degree ~%d): reachable=%ld distance_sum=%ld\n", graph, per_node, reach, distsum);
        printf("  indexed: pops=%ld decrease_keys=%ld   lazy: pushes=%ld stale_pops=%ld\n", total_pops, total_dec, total_push, total_stale);
        check(total_push - total_stale == total_pops, "lazy heap: pushes minus stale pops equals settled nodes");
    }
    return 0;
}
