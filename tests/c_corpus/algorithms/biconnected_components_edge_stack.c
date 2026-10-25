/*
 * title: Biconnected components with an edge stack
 * topic: algorithms
 * covers: biconnected components, edge stack, block-cut tree, articulation points, component partition invariants
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 26, M = 36 };

static unsigned st = 1597u;
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

static int eu[M], ev[M];
static int adjv[N][M], adje[N][M], deg[N];
static int disc[N], low[N], timer_;
static int estack[M], esp;
static int block_of_edge[M], nblocks;
static int in_block[N][M]; /* in_block[v][b] */

static void dfs(int u, int pe) {
    disc[u] = low[u] = ++timer_;
    for (int i = 0; i < deg[u]; i++) {
        int v = adjv[u][i], e = adje[u][i];
        if (e == pe) continue;
        if (!disc[v]) {
            estack[esp++] = e;
            dfs(v, e);
            if (low[v] < low[u]) low[u] = low[v];
            if (low[v] >= disc[u]) {
                int x;
                do {
                    x = estack[--esp];
                    block_of_edge[x] = nblocks;
                } while (x != e);
                nblocks++;
            }
        } else if (disc[v] < disc[u]) {
            estack[esp++] = e;
            if (disc[v] < low[u]) low[u] = disc[v];
        }
    }
}

/* number of components with vertex skip removed */
static int comps_without(int skip) {
    int comp[N], c = 0;
    memset(comp, -1, sizeof comp);
    for (int s = 0; s < N; s++) {
        if (s == skip || comp[s] >= 0) continue;
        int stack[N], sp = 0;
        stack[sp++] = s;
        comp[s] = c;
        while (sp) {
            int u = stack[--sp];
            for (int i = 0; i < deg[u]; i++) {
                int v = adjv[u][i];
                if (v != skip && comp[v] < 0) comp[v] = c, stack[sp++] = v;
            }
        }
        c++;
    }
    return c;
}

int main(void) {
    int m = 0;
    int seen[N][N] = {{0}};
    while (m < M) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        if (u == v || seen[u][v]) continue;
        seen[u][v] = seen[v][u] = 1;
        eu[m] = u, ev[m] = v;
        adjv[u][deg[u]] = v, adje[u][deg[u]++] = m;
        adjv[v][deg[v]] = u, adje[v][deg[v]++] = m;
        m++;
    }
    for (int u = 0; u < N; u++)
        if (!disc[u]) dfs(u, -1);
    check(esp == 0, "edge stack drained");
    int bsize[M] = {0};
    for (int e = 0; e < M; e++) {
        check(block_of_edge[e] >= 0 && block_of_edge[e] < nblocks, "edge in a block");
        bsize[block_of_edge[e]]++;
        in_block[eu[e]][block_of_edge[e]] = 1;
        in_block[ev[e]][block_of_edge[e]] = 1;
    }
    int base = comps_without(-1);
    int arts = 0;
    for (int v = 0; v < N; v++) {
        int nb = 0;
        for (int b = 0; b < nblocks; b++) nb += in_block[v][b];
        int iso = deg[v] == 0;
        int cut = comps_without(v) > base - (iso ? 1 : 0);
        check((nb > 1) == cut, "vertex in several blocks iff articulation point");
        arts += cut;
    }
    int bridges = 0, biggest = 0, vsum = 0;
    for (int b = 0; b < nblocks; b++) {
        bridges += bsize[b] == 1;
        if (bsize[b] > biggest) biggest = bsize[b];
        int vc = 0;
        for (int v = 0; v < N; v++) vc += in_block[v][b];
        vsum += vc;
        /* a block with k>=2 edges is 2-connected: at least 3 vertices; and edges >= vertices */
        if (bsize[b] >= 2) check(vc >= 3 && bsize[b] >= vc, "block shape");
    }
    /* block-cut tree is a forest: sum over vertices of (blocks-1) = blocks - components(with edges) */
    printf("blocks: %d, single-edge blocks (bridges): %d, largest block: %d edges\n", nblocks, bridges, biggest);
    printf("articulation points: %d, vertex-block incidences: %d\n", arts, vsum);
    for (int b = 0; b < nblocks; b++) {
        if (bsize[b] < 3) continue;
        printf("block %d (%d edges): vertices", b, bsize[b]);
        for (int v = 0; v < N; v++)
            if (in_block[v][b]) printf(" %d", v);
        printf("\n");
    }
    return 0;
}
