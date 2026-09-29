/*
 * title: Dinic max flow with level graph and current-arc pointers
 * topic: algorithms
 * covers: Dinic, level graph, blocking flow, residual edges, flow conservation, min cut from residual reachability
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 60, MAXE = 2000 };

static unsigned st = 8080u;
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
    int to, cap, orig;
} Arc;
static Arc arcs[MAXE];
static int nxt[MAXE], head[N], na;
static int level[N], it[N];

static void add_edge(int u, int v, int c) {
    arcs[na] = (Arc){v, c, c};
    nxt[na] = head[u];
    head[u] = na++;
    arcs[na] = (Arc){u, 0, 0};
    nxt[na] = head[v];
    head[v] = na++;
}

static int bfs(int s, int t) {
    int q[N], qh = 0, qt = 0;
    memset(level, -1, sizeof level);
    level[s] = 0;
    q[qt++] = s;
    while (qh < qt) {
        int u = q[qh++];
        for (int e = head[u]; e != -1; e = nxt[e])
            if (arcs[e].cap > 0 && level[arcs[e].to] < 0) level[arcs[e].to] = level[u] + 1, q[qt++] = arcs[e].to;
    }
    return level[t] >= 0;
}

static int dfs(int u, int t, int f) {
    if (u == t) return f;
    for (int *e = &it[u]; *e != -1; *e = nxt[*e]) {
        Arc *a = &arcs[*e];
        if (a->cap > 0 && level[a->to] == level[u] + 1) {
            int d = dfs(a->to, t, f < a->cap ? f : a->cap);
            if (d > 0) {
                a->cap -= d;
                arcs[*e ^ 1].cap += d;
                return d;
            }
        }
    }
    return 0;
}

int main(void) {
    memset(head, -1, sizeof head);
    /* layered network with random capacities */
    enum { LAYERS = 6, PER = 9 };
    int s = N - 2, t = N - 1;
    for (int i = 0; i < PER; i++) add_edge(s, i, 5 + (int)(rnd() % 20));
    for (int l = 0; l + 1 < LAYERS; l++)
        for (int i = 0; i < PER; i++)
            for (int k = 0; k < 3; k++) add_edge(l * PER + i, (l + 1) * PER + (int)(rnd() % PER), 1 + (int)(rnd() % 12));
    for (int k = 0; k < 30; k++) { /* skip edges break the uniform layering */
        int l = (int)(rnd() % (LAYERS - 2)), i = (int)(rnd() % PER), j = (int)(rnd() % PER);
        add_edge(l * PER + i, (l + 2) * PER + j, 1 + (int)(rnd() % 8));
    }
    for (int i = 0; i < PER; i++) add_edge((LAYERS - 1) * PER + i, t, 5 + (int)(rnd() % 20));
    int flow = 0, phases = 0, augment = 0;
    while (bfs(s, t)) {
        phases++;
        memcpy(it, head, sizeof head);
        int f;
        while ((f = dfs(s, t, 1 << 28)) > 0) flow += f, augment++;
    }
    /* conservation and capacity */
    int excess[N] = {0};
    for (int e = 0; e < na; e += 2) {
        int f = arcs[e].orig - arcs[e].cap;
        check(f >= 0 && f <= arcs[e].orig, "capacity respected");
        excess[arcs[e ^ 1].to] -= f;
        excess[arcs[e].to] += f;
    }
    for (int v = 0; v < N; v++)
        if (v != s && v != t) check(excess[v] == 0, "conservation");
    check(excess[t] == flow && excess[s] == -flow, "source and sink balance");
    /* min cut: vertices reachable from s in residual graph */
    int cut = 0, side = 0;
    for (int v = 0; v < N; v++) side += level[v] >= 0;
    for (int e = 0; e < na; e += 2)
        if (level[arcs[e ^ 1].to] >= 0 && level[arcs[e].to] < 0) cut += arcs[e].orig;
    check(cut == flow, "max-flow min-cut");
    int used = 0, saturated = 0;
    for (int e = 0; e < na; e += 2) {
        used += arcs[e].cap < arcs[e].orig;
        saturated += arcs[e].cap == 0 && arcs[e].orig > 0;
    }
    printf("edges %d, max flow %d\n", na / 2, flow);
    printf("phases %d, augmenting paths %d\n", phases, augment);
    printf("source side size %d, cut capacity %d\n", side, cut);
    printf("edges carrying flow %d, saturated %d\n", used, saturated);
    return 0;
}
