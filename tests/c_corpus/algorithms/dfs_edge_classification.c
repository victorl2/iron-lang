/*
 * title: DFS timestamps and edge classification
 * topic: algorithms
 * covers: depth-first search, discovery/finish times, tree/back/forward/cross edges, parenthesis theorem
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { N = 14, M = 32 };

static unsigned st = 777u;
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

static int adj[N][N], deg[N];
static int disc[N], fin[N], color[N], timer_;
static int counts[4];
static const char *names[4] = {"tree", "back", "forward", "cross"};
static int eu[M], ev[M], ek[M], ne;

static void dfs(int u) {
    color[u] = 1;
    disc[u] = ++timer_;
    for (int i = 0; i < deg[u]; i++) {
        int v = adj[u][i], kind;
        if (color[v] == 0) {
            kind = 0;
            eu[ne] = u, ev[ne] = v, ek[ne++] = kind;
            counts[kind]++;
            dfs(v);
            continue;
        }
        if (color[v] == 1)
            kind = 1;
        else
            kind = disc[u] < disc[v] ? 2 : 3;
        counts[kind]++;
        eu[ne] = u, ev[ne] = v, ek[ne++] = kind;
    }
    color[u] = 2;
    fin[u] = ++timer_;
}

int main(void) {
    int seen[N][N] = {{0}};
    int m = 0;
    while (m < M) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        if (u == v || seen[u][v]) continue;
        seen[u][v] = 1;
        adj[u][deg[u]++] = v;
        m++;
    }
    for (int u = 0; u < N; u++)
        if (!color[u]) dfs(u);
    check(ne == M, "every edge classified");
    /* parenthesis theorem and classification checks */
    for (int i = 0; i < ne; i++) {
        int u = eu[i], v = ev[i];
        switch (ek[i]) {
        case 0:
            check(disc[u] < disc[v] && fin[v] < fin[u], "tree nesting");
            break;
        case 1:
            check(disc[v] <= disc[u] && fin[u] <= fin[v], "back edge to ancestor");
            break;
        case 2:
            check(disc[u] < disc[v] && fin[v] < fin[u], "forward nesting");
            break;
        default:
            check(fin[v] < disc[u], "cross edge to finished earlier tree");
        }
    }
    for (int a = 0; a < N; a++)
        for (int b = a + 1; b < N; b++) {
            int disjoint = fin[a] < disc[b] || fin[b] < disc[a];
            int nested = (disc[a] < disc[b] && fin[b] < fin[a]) || (disc[b] < disc[a] && fin[a] < fin[b]);
            check(disjoint != nested, "parenthesis theorem");
        }
    for (int u = 0; u < N; u++) printf("v%-2d disc %2d fin %2d\n", u, disc[u], fin[u]);
    for (int k = 0; k < 4; k++) printf("%s edges: %d\n", names[k], counts[k]);
    printf("graph is %s\n", counts[1] ? "cyclic" : "acyclic");
    return 0;
}
