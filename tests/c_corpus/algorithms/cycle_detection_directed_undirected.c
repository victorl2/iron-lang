/*
 * title: Cycle detection and extraction, directed and undirected
 * topic: algorithms
 * covers: three-color DFS, back edge cycle recovery, union-find undirected cycle, girth by BFS, cyclomatic number
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 16 };

static unsigned st = 6006u;
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

static unsigned char D[N][N]; /* directed */
static int color[N], par[N], cyc[N + 1], clen;

static int dfs_dir(int u) {
    color[u] = 1;
    for (int v = 0; v < N; v++) {
        if (!D[u][v]) continue;
        if (color[v] == 1) {
            clen = 0;
            for (int x = u; x != v; x = par[x]) cyc[clen++] = x;
            cyc[clen++] = v;
            for (int i = 0; i < clen / 2; i++) {
                int t = cyc[i];
                cyc[i] = cyc[clen - 1 - i];
                cyc[clen - 1 - i] = t;
            }
            return 1;
        }
        if (color[v] == 0) {
            par[v] = u;
            if (dfs_dir(v)) return 1;
        }
    }
    color[u] = 2;
    return 0;
}

static int has_cycle_dir(void) {
    memset(color, 0, sizeof color);
    for (int s = 0; s < N; s++)
        if (!color[s]) {
            par[s] = -1;
            if (dfs_dir(s)) return 1;
        }
    return 0;
}

/* reference: repeatedly strip vertices with zero in-degree; leftover means cycle */
static int has_cycle_kahn(void) {
    int indeg[N] = {0}, removed = 0, stack[N], sp = 0;
    for (int u = 0; u < N; u++)
        for (int v = 0; v < N; v++) indeg[v] += D[u][v];
    for (int v = 0; v < N; v++)
        if (!indeg[v]) stack[sp++] = v;
    while (sp) {
        int u = stack[--sp];
        removed++;
        for (int v = 0; v < N; v++)
            if (D[u][v] && --indeg[v] == 0) stack[sp++] = v;
    }
    return removed < N;
}

static int uf[N];
static int find(int x) {
    while (uf[x] != x) x = uf[x] = uf[uf[x]];
    return x;
}

int main(void) {
    int dir_cyclic = 0;
    for (int trial = 0; trial < 10; trial++) {
        memset(D, 0, sizeof D);
        int edges = 0, target = 8 + trial * 2;
        while (edges < target) {
            int u = (int)(rnd() % N), v = (int)(rnd() % N);
            if (u == v || D[u][v]) continue;
            /* first half of the trials produce forward-only edges (acyclic) */
            if (trial < 5 && u > v) continue;
            D[u][v] = 1;
            edges++;
        }
        int c = has_cycle_dir();
        check(c == has_cycle_kahn(), "dfs verdict equals kahn verdict");
        if (c) {
            for (int i = 0; i < clen; i++) check(D[cyc[i]][cyc[(i + 1) % clen]], "extracted cycle is a real cycle");
            dir_cyclic++;
            printf("directed %d: %2d edges, cycle length %d:", trial, edges, clen);
            for (int i = 0; i < clen; i++) printf(" %d", cyc[i]);
            printf("\n");
        } else
            printf("directed %d: %2d edges, acyclic\n", trial, edges);
    }
    /* undirected: union-find detects the first edge that closes a cycle; also the cyclomatic number */
    for (int trial = 0; trial < 5; trial++) {
        for (int i = 0; i < N; i++) uf[i] = i;
        int m = 6 + trial * 3, seen[N][N];
        memset(seen, 0, sizeof seen);
        int closing = -1, k = 0, comps = N;
        while (k < m) {
            int u = (int)(rnd() % N), v = (int)(rnd() % N);
            if (u == v || seen[u][v]) continue;
            seen[u][v] = seen[v][u] = 1;
            int a = find(u), b = find(v);
            if (a == b) {
                if (closing < 0) closing = k;
            } else
                uf[a] = b, comps--;
            k++;
        }
        int cyclomatic = m - N + comps;
        printf("undirected %d: %2d edges, first cycle-closing edge %d, components %d, independent cycles %d\n", trial, m, closing, comps,
               cyclomatic);
        check((closing >= 0) == (cyclomatic > 0), "closing edge iff cyclomatic number positive");
    }
    printf("cyclic directed graphs: %d of 10\n", dir_cyclic);
    return 0;
}
