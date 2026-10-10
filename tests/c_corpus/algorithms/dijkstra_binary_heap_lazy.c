/*
 * title: Dijkstra with a lazy-deletion binary heap
 * topic: algorithms
 * covers: Dijkstra, binary heap, lazy deletion, adjacency lists, O(n^2) cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { N = 300, M = 1500, INF = 1 << 30 };

static unsigned st = 5551212u;
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
static Edge E[M];
static int head[N], ne;

typedef struct {
    int d, v;
} Item;
static Item heap[M + N];
static int hn;
static long pushes, stale;

static int less(Item a, Item b) { return a.d < b.d || (a.d == b.d && a.v < b.v); }

static void hpush(Item x) {
    int i = hn++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!less(x, heap[p])) break;
        heap[i] = heap[p];
        i = p;
    }
    heap[i] = x;
    pushes++;
}
static Item hpop(void) {
    Item top = heap[0], x = heap[--hn];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= hn) break;
        if (c + 1 < hn && less(heap[c + 1], heap[c])) c++;
        if (!less(heap[c], x)) break;
        heap[i] = heap[c];
        i = c;
    }
    if (hn > 0) heap[i] = x;
    return top;
}

int main(void) {
    for (int i = 0; i < N; i++) head[i] = -1;
    for (int i = 0; i < M; i++) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        E[ne] = (Edge){v, 1 + (int)(rnd() % 50), head[u]};
        head[u] = ne++;
    }
    int dist[N];
    for (int i = 0; i < N; i++) dist[i] = INF;
    dist[0] = 0;
    hpush((Item){0, 0});
    while (hn > 0) {
        Item it = hpop();
        if (it.d > dist[it.v]) {
            stale++;
            continue;
        }
        for (int e = head[it.v]; e != -1; e = E[e].next) {
            int nd = it.d + E[e].w;
            if (nd < dist[E[e].to]) {
                dist[E[e].to] = nd;
                hpush((Item){nd, E[e].to});
            }
        }
    }
    /* reference: O(n^2) Dijkstra */
    int ref[N], done[N];
    for (int i = 0; i < N; i++) ref[i] = INF, done[i] = 0;
    ref[0] = 0;
    for (int k = 0; k < N; k++) {
        int u = -1;
        for (int i = 0; i < N; i++)
            if (!done[i] && (u < 0 || ref[i] < ref[u])) u = i;
        if (ref[u] == INF) break;
        done[u] = 1;
        for (int e = head[u]; e != -1; e = E[e].next)
            if (ref[u] + E[e].w < ref[E[e].to]) ref[E[e].to] = ref[u] + E[e].w;
    }
    long sum = 0;
    int reach = 0, far = 0, farv = 0;
    for (int i = 0; i < N; i++) {
        check(dist[i] == ref[i], "heap dijkstra equals quadratic");
        if (dist[i] < INF) {
            reach++;
            sum += dist[i];
            if (dist[i] > far) far = dist[i], farv = i;
        }
    }
    printf("reachable %d of %d\n", reach, N);
    printf("distance sum %ld, farthest vertex %d at %d\n", sum, farv, far);
    printf("heap pushes %ld, stale pops %ld\n", pushes, stale);
    for (int v = 1; v <= 8; v++) printf("d(0,%d) = %d\n", v, dist[v] < INF ? dist[v] : -1);
    return 0;
}
