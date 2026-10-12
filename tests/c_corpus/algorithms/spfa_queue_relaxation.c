/*
 * title: SPFA queue-based relaxation
 * topic: algorithms
 * covers: SPFA, in-queue flags, relaxation counts, negative edges, comparison against Bellman-Ford
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { N = 250, M = 1200, INF = 1 << 29 };

static unsigned st = 13579u;
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
    int u, v, w, next;
} Edge;
static Edge E[M];
static int head[N];

int main(void) {
    int pot[N];
    for (int i = 0; i < N; i++) head[i] = -1, pot[i] = (int)(rnd() % 30);
    for (int i = 0; i < M; i++) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        int w = (int)(rnd() % 15) + pot[u] - pot[v]; /* nonnegative base, potentials make some negative */
        E[i] = (Edge){u, v, w, head[u]};
        head[u] = i;
    }
    int neg = 0;
    for (int i = 0; i < M; i++) neg += E[i].w < 0;

    /* SPFA with circular queue */
    int dist[N], inq[N], cnt[N], q[N + 1], qh = 0, qt = 0;
    long relax = 0, pops = 0;
    for (int i = 0; i < N; i++) dist[i] = INF, inq[i] = 0, cnt[i] = 0;
    dist[0] = 0;
    q[qt++] = 0;
    inq[0] = 1;
    int cycle = 0;
    while (qh != qt && !cycle) {
        int u = q[qh];
        qh = (qh + 1) % (N + 1);
        inq[u] = 0;
        pops++;
        for (int e = head[u]; e != -1; e = E[e].next) {
            int v = E[e].v;
            if (dist[u] + E[e].w < dist[v]) {
                dist[v] = dist[u] + E[e].w;
                relax++;
                if (!inq[v]) {
                    if (++cnt[v] > N) cycle = 1;
                    q[qt] = v;
                    qt = (qt + 1) % (N + 1);
                    inq[v] = 1;
                }
            }
        }
    }
    check(!cycle, "no negative cycle (weights derived from potentials)");
    /* Bellman-Ford reference */
    int ref[N];
    int passes = 0;
    for (int i = 0; i < N; i++) ref[i] = INF;
    ref[0] = 0;
    for (int again = 1; again;) {
        again = 0;
        passes++;
        for (int i = 0; i < M; i++)
            if (ref[E[i].u] < INF && ref[E[i].u] + E[i].w < ref[E[i].v]) {
                ref[E[i].v] = ref[E[i].u] + E[i].w;
                again = 1;
            }
        check(passes <= N + 1, "bounded passes");
    }
    long sum = 0;
    int reach = 0, mn = 0;
    for (int i = 0; i < N; i++) {
        check(dist[i] == ref[i], "spfa equals bellman-ford");
        if (dist[i] < INF) reach++, sum += dist[i];
        if (dist[i] < INF && dist[i] < mn) mn = dist[i];
    }
    printf("edges %d, negative edges %d\n", M, neg);
    printf("reachable %d, sum %ld, min %d\n", reach, sum, mn);
    printf("spfa pops %ld, successful relaxations %ld\n", pops, relax);
    printf("bellman-ford passes %d\n", passes);
    return 0;
}
