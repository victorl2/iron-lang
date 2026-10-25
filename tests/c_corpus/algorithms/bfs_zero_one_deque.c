/*
 * title: 0-1 BFS with a deque versus Dijkstra
 * topic: algorithms
 * covers: 0-1 BFS, circular deque, edge weights 0 and 1, quadratic Dijkstra cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { N = 200, M = 900, INF = 1 << 29 };

static unsigned st = 123456789u;
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
    int to, w, next;
} Edge;
static Edge edges[M];
static int first[N], ne;

static void add_edge(int u, int v, int w) {
    edges[ne] = (Edge){v, w, first[u]};
    first[u] = ne++;
}

/* circular deque of capacity 2*M+N, ring indices */
enum { CAP = 4096 };
static int dq[CAP], dh, dt, dn;
static void push_front(int x) {
    dh = (dh + CAP - 1) % CAP;
    dq[dh] = x;
    dn++;
}
static void push_back(int x) {
    dq[dt] = x;
    dt = (dt + 1) % CAP;
    dn++;
}
static int pop_front(void) {
    int x = dq[dh];
    dh = (dh + 1) % CAP;
    dn--;
    return x;
}

int main(void) {
    for (int i = 0; i < N; i++) first[i] = -1;
    for (int i = 0; i < M; i++) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        add_edge(u, v, (int)(rnd() % 100 < 45 ? 0 : 1));
    }
    int dist[N];
    for (int i = 0; i < N; i++) dist[i] = INF;
    dist[0] = 0;
    push_back(0);
    long pops = 0;
    while (dn > 0) {
        int u = pop_front();
        pops++;
        for (int e = first[u]; e != -1; e = edges[e].next) {
            int v = edges[e].to, w = edges[e].w;
            if (dist[u] + w < dist[v]) {
                dist[v] = dist[u] + w;
                if (w == 0)
                    push_front(v);
                else
                    push_back(v);
            }
        }
    }
    int ref[N], done[N];
    for (int i = 0; i < N; i++) ref[i] = INF, done[i] = 0;
    ref[0] = 0;
    for (int it = 0; it < N; it++) {
        int u = -1;
        for (int i = 0; i < N; i++)
            if (!done[i] && (u < 0 || ref[i] < ref[u])) u = i;
        if (ref[u] >= INF) break;
        done[u] = 1;
        for (int e = first[u]; e != -1; e = edges[e].next)
            if (ref[u] + edges[e].w < ref[edges[e].to]) ref[edges[e].to] = ref[u] + edges[e].w;
    }
    int reach = 0, far = 0, hist[16] = {0};
    long sum = 0;
    for (int i = 0; i < N; i++) {
        check(dist[i] == ref[i], "0-1 BFS equals Dijkstra");
        if (dist[i] < INF) {
            reach++;
            sum += dist[i];
            if (dist[i] > far) far = dist[i];
            hist[dist[i] < 15 ? dist[i] : 15]++;
        }
    }
    printf("reachable: %d of %d\n", reach, N);
    printf("max distance: %d, sum: %ld\n", far, sum);
    printf("deque pops: %ld\n", pops);
    for (int d = 0; d <= far && d < 16; d++) printf("dist %d: %d\n", d, hist[d]);
    return 0;
}
