/*
 * title: Distance-vector routing and count-to-infinity
 * topic: networking
 * covers: Bellman-Ford distributed, RIP infinity of 16, split horizon, poison reverse, synchronous rounds, link failure
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void fail(const char *m) {
    fprintf(stderr, "check failed: %s\n", m);
    exit(1);
}
static void check(int c, const char *m) { if (!c) fail(m); }

enum { N = 5, INF = 16 };
enum { M_PLAIN, M_SPLIT, M_POISON };
static const char *mname[] = {"plain", "split-horizon", "poison-reverse"};
static const char nm[N] = {'A', 'B', 'C', 'D', 'E'};

typedef struct {
    int link[N][N];       /* 0 = no link */
    int dist[N][N];       /* dist[u][d] */
    int next[N][N];       /* next hop or -1 */
} Net;

static void net_init(Net *n) {
    memset(n->link, 0, sizeof n->link);
    for (int u = 0; u < N; u++)
        for (int d = 0; d < N; d++) {
            n->dist[u][d] = u == d ? 0 : INF;
            n->next[u][d] = u == d ? u : -1;
        }
}
static void add_link(Net *n, int a, int b, int c) { n->link[a][b] = n->link[b][a] = c; }

/* what u advertises to v about destination d */
static int advertised(const Net *n, int mode, int u, int v, int d) {
    int c = n->dist[u][d];
    if (mode != M_PLAIN && n->next[u][d] == v && d != u) return mode == M_POISON ? INF : -1; /* -1: omit */
    return c;
}

/* one synchronous round: everyone sends to neighbours, then everyone recomputes. Returns number of changed entries. */
static int round_(Net *n, int mode) {
    Net old = *n;
    int changed = 0;
    for (int v = 0; v < N; v++)
        for (int u = 0; u < N; u++) {
            if (!old.link[u][v]) continue;
            for (int d = 0; d < N; d++) {
                if (d == v) continue;
                int adv = advertised(&old, mode, u, v, d);
                int cand;
                if (adv < 0) {
                    /* omitted: a route through u that relied on this entry is stale only if it is not re-advertised */
                    if (n->next[v][d] == u) { /* u no longer offers a route */
                        n->dist[v][d] = INF;
                        n->next[v][d] = -1;
                        changed++;
                    }
                    continue;
                }
                cand = adv + old.link[u][v];
                if (cand > INF) cand = INF;
                if (n->next[v][d] == u) {
                    if (n->dist[v][d] != cand) { n->dist[v][d] = cand; changed++; }
                    if (cand >= INF) n->next[v][d] = -1;
                } else if (cand < n->dist[v][d]) {
                    n->dist[v][d] = cand;
                    n->next[v][d] = u;
                    changed++;
                }
            }
        }
    return changed;
}

static int converge(Net *n, int mode, int max_rounds) {
    int r = 0;
    while (r < max_rounds && round_(n, mode) > 0) r++;
    return r;
}

static int nn = N;
static void print_col(const Net *n, int d, const char *label) {
    printf("  %s dist to %c:", label, nm[d]);
    for (int u = 0; u < nn; u++) if (u != d) printf(" %c=%2d", nm[u], n->dist[u][d]);
    printf("\n");
}

static void scenario(const char *title, int nodes, const int (*edges)[3], int ne, int cut_a, int cut_b, int dest, int fatal) {
    nn = nodes;
    printf("%s (cut %c-%c)\n", title, nm[cut_a], nm[cut_b]);
    int rounds_to_settle[3];
    for (int mode = 0; mode < 3; mode++) {
        Net n;
        net_init(&n);
        for (int i = 0; i < ne; i++) add_link(&n, edges[i][0], edges[i][1], edges[i][2]);
        int r0 = converge(&n, mode, 200);
        n.link[cut_a][cut_b] = n.link[cut_b][cut_a] = 0;
        /* neighbours notice the failure locally */
        for (int x = 0; x < 2; x++) {
            int u = x ? cut_b : cut_a, v = x ? cut_a : cut_b;
            for (int d = 0; d < N; d++)
                if (n.next[u][d] == v && d != u) { n.dist[u][d] = INF; n.next[u][d] = -1; }
        }
        int r = 0, maxd = 0;
        while (r < 200) {
            int c = round_(&n, mode);
            if (c == 0) break;
            r++;
            for (int u = 0; u < nn; u++) if (n.dist[u][dest] < INF && n.dist[u][dest] > maxd) maxd = n.dist[u][dest];
            if (mode == M_PLAIN && r <= 4) print_col(&n, dest, "  plain round");
        }
        rounds_to_settle[mode] = r;
        printf(" %-14s initial rounds=%d, rounds after failure=%2d, peak finite cost to %c=%d\n", mname[mode], r0, r, nm[dest], maxd);
        int all_inf = 1;
        for (int u = 0; u < nn; u++) if (u != dest && n.dist[u][dest] < INF) all_inf = 0;
        if (fatal) check(all_inf, "destination unreachable everywhere after cut");
    }
    check(rounds_to_settle[M_SPLIT] <= rounds_to_settle[M_PLAIN], "split horizon is no slower than plain");
    check(rounds_to_settle[M_POISON] <= rounds_to_settle[M_PLAIN], "poison reverse is no slower than plain");
}

int main(void) {
    /* chain A-B-C-D: the textbook two-node loop between B and C */
    static const int chain[][3] = {{0, 1, 1}, {1, 2, 1}, {2, 3, 1}};
    scenario("chain A-B-C-D", 4, chain, 3, 2, 3, 3, 1);
    /* triangle A,B,C with D behind A: B and C can loop through each other */
    static const int tri[][3] = {{0, 1, 1}, {1, 2, 1}, {0, 2, 1}, {0, 3, 1}};
    scenario("triangle ABC with D behind A", 4, tri, 4, 0, 3, 3, 1);
    /* redundant path: cut link is not fatal */
    static const int ring[][3] = {{0, 1, 1}, {1, 2, 1}, {2, 3, 1}, {3, 4, 1}, {4, 0, 1}};
    scenario("ring of five", 5, ring, 5, 3, 4, 4, 0);
    nn = N;
    Net n;
    net_init(&n);
    for (int i = 0; i < 5; i++) add_link(&n, i, (i + 1) % 5, 1);
    converge(&n, M_POISON, 50);
    for (int u = 0; u < N; u++)
        for (int d = 0; d < N; d++) {
            int cw = (d - u + N) % N, ccw = N - cw;
            int want = cw < ccw ? cw : ccw;
            if (u == d) want = 0;
            check(n.dist[u][d] == want, "ring distances are shortest");
        }
    printf("ring routing table at A:");
    for (int d = 1; d < N; d++) printf(" %c via %c (%d)", nm[d], nm[n.next[0][d]], n.dist[0][d]);
    printf("\n");
    return 0;
}
