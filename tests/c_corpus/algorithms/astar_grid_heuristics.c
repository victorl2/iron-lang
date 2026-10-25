/*
 * title: A* search with Manhattan and Chebyshev heuristics
 * topic: algorithms
 * covers: A*, admissible heuristics, node expansion counts, 8-connected grid, comparison to Dijkstra
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { H = 40, W = 40, N = H * W, INF = 1 << 28 };

static unsigned st = 20240229u;
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

static unsigned char wall[N];
/* costs are scaled: orthogonal step 10, diagonal 14 */
static const int DR[8] = {-1, 0, 1, 0, -1, -1, 1, 1};
static const int DC[8] = {0, 1, 0, -1, -1, 1, 1, -1};

typedef struct {
    int f, g, v;
} Node;
static Node heap[N * 8];
static int hn;
static int lessn(Node a, Node b) {
    if (a.f != b.f) return a.f < b.f;
    if (a.g != b.g) return a.g > b.g;
    return a.v < b.v;
}
static void push(Node x) {
    int i = hn++;
    while (i > 0 && lessn(x, heap[(i - 1) / 2])) {
        heap[i] = heap[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    heap[i] = x;
}
static Node pop(void) {
    Node top = heap[0], x = heap[--hn];
    int i = 0;
    while (2 * i + 1 < hn) {
        int c = 2 * i + 1;
        if (c + 1 < hn && lessn(heap[c + 1], heap[c])) c++;
        if (!lessn(heap[c], x)) break;
        heap[i] = heap[c];
        i = c;
    }
    if (hn) heap[i] = x;
    return top;
}

static int heur(int kind, int v, int goal) {
    int dr = abs(v / W - goal / W), dc = abs(v % W - goal % W);
    int mn = dr < dc ? dr : dc, mx = dr < dc ? dc : dr;
    switch (kind) {
    case 0: return 0;
    case 1: return 10 * (dr + dc); /* inadmissible for diagonal moves */
    case 2: return 10 * mx;        /* Chebyshev, admissible but weak */
    default: return 14 * mn + 10 * (mx - mn); /* octile, exact on open grid */
    }
}

static int search(int kind, int start, int goal, int *expanded) {
    static int g[N];
    static unsigned char closed[N];
    for (int i = 0; i < N; i++) g[i] = INF, closed[i] = 0;
    hn = 0;
    *expanded = 0;
    g[start] = 0;
    push((Node){heur(kind, start, goal), 0, start});
    while (hn > 0) {
        Node n = pop();
        if (closed[n.v]) continue;
        closed[n.v] = 1;
        (*expanded)++;
        if (n.v == goal) return n.g;
        for (int d = 0; d < 8; d++) {
            int nr = n.v / W + DR[d], nc = n.v % W + DC[d];
            if (nr < 0 || nr >= H || nc < 0 || nc >= W || wall[nr * W + nc]) continue;
            int step = d < 4 ? 10 : 14;
            int nv = nr * W + nc;
            if (n.g + step < g[nv]) {
                g[nv] = n.g + step;
                push((Node){g[nv] + heur(kind, nv, goal), g[nv], nv});
            }
        }
    }
    return -1;
}

int main(void) {
    for (int i = 0; i < N; i++) wall[i] = rnd() % 100 < 22;
    int start = 0, goal = N - 1;
    wall[start] = wall[goal] = 0;
    const char *names[4] = {"dijkstra (h=0)", "manhattan", "chebyshev", "octile"};
    int cost[4], exp[4];
    for (int k = 0; k < 4; k++) cost[k] = search(k, start, goal, &exp[k]);
    if (cost[0] < 0) {
        printf("no route\n");
        return 0;
    }
    check(cost[2] == cost[0], "chebyshev admissible");
    check(cost[3] == cost[0], "octile admissible");
    check(cost[1] >= cost[0], "manhattan cost never below optimum");
    check(exp[3] <= exp[0], "octile expands no more than dijkstra");
    check(exp[2] <= exp[0], "chebyshev expands no more than dijkstra");
    for (int k = 0; k < 4; k++) printf("%-15s cost %4d expanded %4d\n", names[k], cost[k], exp[k]);
    printf("manhattan suboptimal: %s\n", cost[1] > cost[0] ? "yes" : "no");
    int walls = 0;
    for (int i = 0; i < N; i++) walls += wall[i];
    printf("walls: %d of %d\n", walls, N);
    return 0;
}
