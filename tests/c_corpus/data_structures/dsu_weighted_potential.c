/*
 * title: Weighted union-find with potentials
 * topic: data_structures
 * covers: weighted DSU, relative offsets, path compression with potential update, contradiction detection, difference constraints, BFS oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 60

static unsigned long long rs = 0x7E16874EDULL * 977;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* pot[x] = value(x) - value(parent[x]) */
static int par[N], sz[N];
static long pot[N];

static int find(int x) {
    if (par[x] == x) return x;
    int p = par[x];
    int r = find(p);
    pot[x] += pot[p];   /* p now points at r with pot[p] = value(p) - value(r) */
    par[x] = r;
    return r;
}
static long diff(int x, int y, int *ok) { /* value(x) - value(y) */
    int rx = find(x), ry = find(y);
    if (rx != ry) { *ok = 0; return 0; }
    *ok = 1;
    return pot[x] - pot[y];
}
/* assert value(x) - value(y) = d. Returns 1 new, 0 redundant consistent, -1 contradiction. */
static int assert_diff(int x, int y, long d) {
    int rx = find(x), ry = find(y);
    if (rx == ry) return (pot[x] - pot[y] == d) ? 0 : -1;
    /* value(rx) - value(ry) = d - pot[x] + pot[y] */
    long w = d - pot[x] + pot[y];
    if (sz[rx] < sz[ry]) { int t = rx; rx = ry; ry = t; w = -w; }
    par[ry] = rx; pot[ry] = -w; sz[rx] += sz[ry];   /* value(ry) - value(rx) = -w */
    return 1;
}

/* oracle: adjacency with weighted edges, BFS to assign values within components */
static int eu[400], ev[400]; static long ew[400]; static int ne;
static int oracle_diff(int x, int y, long *out) {
    long val[N]; int seen[N] = {0}, st[N * 4], sp = 0;
    seen[x] = 1; val[x] = 0; st[sp++] = x;
    while (sp) {
        int u = st[--sp];
        for (int i = 0; i < ne; i++) {
            if (eu[i] == u && !seen[ev[i]]) { seen[ev[i]] = 1; val[ev[i]] = val[u] - ew[i]; st[sp++] = ev[i]; }
            else if (ev[i] == u && !seen[eu[i]]) { seen[eu[i]] = 1; val[eu[i]] = val[u] + ew[i]; st[sp++] = eu[i]; }
        }
    }
    if (!seen[y]) return 0;
    *out = val[x] - val[y];
    return 1;
}

int main(void) {
    for (int i = 0; i < N; i++) { par[i] = i; sz[i] = 1; pot[i] = 0; }
    long truth[N];
    for (int i = 0; i < N; i++) truth[i] = (long)(rnd() % 2001) - 1000;
    int added = 0, redundant = 0, bad = 0, queries = 0, connected_q = 0;
    long qsum = 0;
    for (int step = 0; step < 700; step++) {
        int x = (int)(rnd() % N), y = (int)(rnd() % N);
        unsigned k = rnd() % 10;
        if (k < 5) {
            long d = truth[x] - truth[y];
            int r = assert_diff(x, y, d);
            check(r >= 0, "true constraint never contradicts");
            if (r == 1) { eu[ne] = x; ev[ne] = y; ew[ne] = d; ne++; added++; } else redundant++;
        } else if (k < 6) {
            long d = truth[x] - truth[y] + 1 + (long)(rnd() % 5);
            int r = assert_diff(x, y, d);
            check(r == -1 || r == 1, "bad constraint either contradicts or merges");
            if (r == 1) { /* it merged two unrelated components with a false-but-consistent diff; undo not possible: keep truth consistent by replaying */
                for (int i = 0; i < N; i++) { par[i] = i; sz[i] = 1; pot[i] = 0; }
                for (int i = 0; i < ne; i++) assert_diff(eu[i], ev[i], ew[i]);
            } else bad++;
        } else {
            int ok; long d = diff(x, y, &ok); queries++;
            long od = 0; int ook = oracle_diff(x, y, &od);
            check(ok == ook, "connectivity vs oracle");
            if (ok) { check(d == od, "difference vs oracle"); check(d == truth[x] - truth[y], "difference vs truth"); connected_q++; qsum += d; }
        }
    }
    int comps = 0, big = 0;
    for (int i = 0; i < N; i++) if (find(i) == i) { comps++; if (sz[i] > big) big = sz[i]; }
    printf("edges_added=%d redundant=%d contradictions=%d\n", added, redundant, bad);
    printf("queries=%d connected=%d sum=%ld components=%d largest=%d\n", queries, connected_q, qsum, comps, big);
    return 0;
}
