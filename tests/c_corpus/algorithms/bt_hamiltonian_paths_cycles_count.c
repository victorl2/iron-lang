/*
 * title: Counting Hamiltonian paths and cycles by backtracking
 * topic: algorithms
 * covers: backtracking, visited bitmask, adjacency bitsets, pruning by degree, bitmask DP cross-check, grid graphs
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXV 20

typedef struct {
    int n;
    unsigned adj[MAXV];
} Graph;

static unsigned st = 2718281u;

static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void add_edge(Graph *g, int a, int b) {
    g->adj[a] |= 1u << b;
    g->adj[b] |= 1u << a;
}

static long nodes;

static int popcount(unsigned v) {
    int c = 0;
    for (; v; v &= v - 1)
        c++;
    return c;
}

/* Count directed Hamiltonian paths starting at v. A dead-end check prunes states where
 * some unvisited vertex has no unvisited neighbour and is not adjacent to the current tip. */
static long paths_from(const Graph *g, int v, unsigned seen, int prune) {
    nodes++;
    unsigned all = (1u << g->n) - 1;
    if (seen == all)
        return 1;
    if (prune) {
        unsigned rest = all & ~seen;
        int isolated = 0;
        for (unsigned r = rest; r; r &= r - 1) {
            int u = 0;
            while (!((r >> u) & 1u))
                u++;
            if (!(g->adj[u] & (rest | (1u << v)))) {
                return 0;
            }
            if (popcount(g->adj[u] & (rest | (1u << v))) == 1 && !((g->adj[v] >> u) & 1u))
                isolated++;
        }
        if (isolated > 1)
            return 0; /* two vertices that can only end the path: impossible */
    }
    long total = 0;
    for (unsigned c = g->adj[v] & ~seen; c; c &= c - 1) {
        int u = 0;
        while (!((c >> u) & 1u))
            u++;
        total += paths_from(g, u, seen | (1u << u), prune);
    }
    return total;
}

static long count_paths(const Graph *g, int prune) {
    long t = 0;
    for (int v = 0; v < g->n; v++)
        t += paths_from(g, v, 1u << v, prune);
    return t; /* every undirected path is counted once per direction */
}

/* Hamiltonian cycles through vertex 0, as directed cycles. */
static long cycles_from(const Graph *g, int v, unsigned seen) {
    unsigned all = (1u << g->n) - 1;
    if (seen == all)
        return (g->adj[v] & 1u) ? 1 : 0;
    long total = 0;
    for (unsigned c = g->adj[v] & ~seen; c; c &= c - 1) {
        int u = 0;
        while (!((c >> u) & 1u))
            u++;
        total += cycles_from(g, u, seen | (1u << u));
    }
    return total;
}

/* Reference by bitmask DP: dp[mask][v] = number of paths covering mask ending at v. */
static long dp_paths(const Graph *g) {
    int n = g->n;
    long *dp = calloc((size_t)(1u << n) * (size_t)n, sizeof(long));
    for (int v = 0; v < n; v++)
        dp[(size_t)(1u << v) * (size_t)n + (size_t)v] = 1;
    for (unsigned m = 1; m < (1u << n); m++)
        for (int v = 0; v < n; v++) {
            long w = dp[(size_t)m * (size_t)n + (size_t)v];
            if (!w)
                continue;
            for (unsigned c = g->adj[v] & ~m; c; c &= c - 1) {
                int u = 0;
                while (!((c >> u) & 1u))
                    u++;
                dp[(size_t)(m | (1u << u)) * (size_t)n + (size_t)u] += w;
            }
        }
    long t = 0;
    for (int v = 0; v < n; v++)
        t += dp[(size_t)((1u << n) - 1) * (size_t)n + (size_t)v];
    free(dp);
    return t;
}

static Graph grid(int h, int w) {
    Graph g;
    memset(&g, 0, sizeof g);
    g.n = h * w;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (x + 1 < w)
                add_edge(&g, y * w + x, y * w + x + 1);
            if (y + 1 < h)
                add_edge(&g, y * w + x, (y + 1) * w + x);
        }
    return g;
}

int main(void) {
    int dims[][2] = {{2, 2}, {2, 3}, {3, 3}, {3, 4}, {4, 4}, {3, 5}, {4, 5}};
    for (int i = 0; i < 7; i++) {
        Graph g = grid(dims[i][0], dims[i][1]);
        nodes = 0;
        long plain = count_paths(&g, 0);
        long n_plain = nodes;
        nodes = 0;
        long pruned = count_paths(&g, 1);
        long n_pruned = nodes;
        long ref = dp_paths(&g);
        check(plain == ref && pruned == ref, "grid path count matches DP");
        long cyc = cycles_from(&g, 0, 1u);
        printf("grid %dx%d: undirected paths=%4ld cycles=%3ld nodes plain=%7ld pruned=%7ld\n", dims[i][0],
               dims[i][1], ref / 2, cyc / 2, n_plain, n_pruned);
        if (dims[i][0] * dims[i][1] % 2 == 1)
            check(cyc == 0, "odd bipartite grid has no Hamiltonian cycle");
    }
    /* complete graphs: n! directed paths, (n-1)! directed cycles */
    for (int n = 3; n <= 8; n += 1) {
        Graph g;
        memset(&g, 0, sizeof g);
        g.n = n;
        for (int a = 0; a < n; a++)
            for (int b = a + 1; b < n; b++)
                add_edge(&g, a, b);
        long f = 1, f1 = 1;
        for (int k = 2; k <= n; k++)
            f *= k;
        for (int k = 2; k < n; k++)
            f1 *= k;
        check(count_paths(&g, 0) == f && cycles_from(&g, 0, 1u) == f1, "complete graph counts");
    }
    printf("complete graphs K3..K8 verified against n! and (n-1)!\n");
    long tot_paths = 0, tot_cycles = 0;
    int with_cycle = 0;
    for (int t = 0; t < 60; t++) {
        Graph g;
        memset(&g, 0, sizeof g);
        g.n = 7 + (int)(rnd() % 6);
        for (int a = 0; a < g.n; a++)
            for (int b = a + 1; b < g.n; b++)
                if (rnd() % 100 < 40)
                    add_edge(&g, a, b);
        long p = count_paths(&g, 1);
        check(p == dp_paths(&g), "random graph agrees with DP");
        long c = cycles_from(&g, 0, 1u);
        tot_paths += p / 2;
        tot_cycles += c / 2;
        with_cycle += c > 0;
    }
    printf("60 random graphs: paths=%ld cycles=%ld graphs with a Hamiltonian cycle=%d\n", tot_paths, tot_cycles,
           with_cycle);
    return 0;
}
