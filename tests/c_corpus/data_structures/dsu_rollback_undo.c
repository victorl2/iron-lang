/*
 * title: Union-find with rollback and offline dynamic connectivity
 * topic: data_structures
 * covers: DSU rollback, union by size without compression, undo stack, snapshots, segment tree over time, offline connectivity
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 40
#define T 64      /* time steps */
#define MAXE 200

static unsigned long long rs = 0xD50011BAC4ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int par[N], sz[N], comps;
static int hist_child[N * 4], hist_root[N * 4], hn;

static int find(int x) { while (par[x] != x) x = par[x]; return x; }
static int unite(int a, int b) {
    a = find(a); b = find(b);
    if (a == b) return 0;
    if (sz[a] < sz[b]) { int t = a; a = b; b = t; }
    par[b] = a; sz[a] += sz[b]; comps--;
    hist_child[hn] = b; hist_root[hn] = a; hn++;
    return 1;
}
static void rollback_to(int mark) {
    while (hn > mark) {
        hn--;
        int b = hist_child[hn], a = hist_root[hn];
        par[b] = b; sz[a] -= sz[b]; comps++;
    }
}
static void init(void) { for (int i = 0; i < N; i++) { par[i] = i; sz[i] = 1; } comps = N; hn = 0; }

/* Edges with lifetimes [l, r). Offline: segment tree over time, DFS with rollback. */
static int eu[MAXE], ev[MAXE], el[MAXE], er[MAXE], ne;
static int seg[4 * T][MAXE], segn[4 * T];

static int ans_comps[T], ans_maxsz[T];
static void seg_add(int node, int lo, int hi, int l, int r, int e) {
    if (r <= lo || hi <= l) return;
    if (l <= lo && hi <= r) { seg[node][segn[node]++] = e; return; }
    int mid = (lo + hi) / 2;
    seg_add(2 * node, lo, mid, l, r, e); seg_add(2 * node + 1, mid, hi, l, r, e);
}
static int deepest;
static void dfs(int node, int lo, int hi, int depth) {
    int mark = hn;
    for (int i = 0; i < segn[node]; i++) unite(eu[seg[node][i]], ev[seg[node][i]]);
    if (depth > deepest) deepest = depth;
    if (hi - lo == 1) {
        ans_comps[lo] = comps;
        int m = 0; for (int i = 0; i < N; i++) if (par[i] == i && sz[i] > m) m = sz[i];
        ans_maxsz[lo] = m;
    } else {
        int mid = (lo + hi) / 2;
        dfs(2 * node, lo, mid, depth + 1); dfs(2 * node + 1, mid, hi, depth + 1);
    }
    rollback_to(mark);
}

int main(void) {
    /* Part 1: random unions with periodic rollback vs recomputation from the surviving union list */
    init();
    int ulist[400][2], un = 0, marks[64], mn = 0, unions_done = 0, rollbacks = 0;
    for (int step = 0; step < 600; step++) {
        unsigned r = rnd() % 10;
        if (r < 7) {
            int a = (int)(rnd() % N), b = (int)(rnd() % N);
            if (un < 400) { ulist[un][0] = a; ulist[un][1] = b; un++; unite(a, b); unions_done++; }
        } else if (r < 8 && mn < 64) {
            marks[mn++] = hn;     /* remember (history length, union-list length) */
            marks[mn - 1] = hn * 1000 + un;
        } else if (mn > 0) {
            int hm = marks[mn - 1] / 1000, um = marks[mn - 1] % 1000; mn--;
            rollback_to(hm); un = um; rollbacks++;
            /* recompute from scratch */
            int rp[N], rc = N;
            for (int i = 0; i < N; i++) rp[i] = i;
            for (int i = 0; i < un; i++) {
                int x = ulist[i][0], y = ulist[i][1];
                while (rp[x] != x) x = rp[x];
                while (rp[y] != y) y = rp[y];
                if (x != y) { rp[x] = y; rc--; }
            }
            check(rc == comps, "component count after rollback");
            for (int i = 0; i < N; i++) for (int j = i + 1; j < N; j++) {
                int a = i, b = j;
                while (rp[a] != a) a = rp[a];
                while (rp[b] != b) b = rp[b];
                check((a == b) == (find(i) == find(j)), "connectivity after rollback");
            }
        }
    }
    printf("part1: unions=%d rollbacks=%d final_comps=%d history=%d\n", unions_done, rollbacks, comps, hn);

    /* Part 2: offline dynamic connectivity */
    init();
    ne = 0;
    for (int i = 0; i < 120; i++) {
        int a = (int)(rnd() % N), b = (int)(rnd() % N);
        int l = (int)(rnd() % T), len = 1 + (int)(rnd() % 30);
        int r2 = l + len > T ? T : l + len;
        eu[ne] = a; ev[ne] = b; el[ne] = l; er[ne] = r2; ne++;
    }
    for (int e = 0; e < ne; e++) seg_add(1, 0, T, el[e], er[e], e);
    dfs(1, 0, T, 0);
    check(hn == 0 && comps == N, "fully rolled back");
    unsigned long long chk = 0; int minc = N, maxc = 0;
    for (int t = 0; t < T; t++) {
        int rp[N], rc = N;
        for (int i = 0; i < N; i++) rp[i] = i;
        for (int e = 0; e < ne; e++) if (el[e] <= t && t < er[e]) {
            int x = eu[e], y = ev[e];
            while (rp[x] != x) x = rp[x];
            while (rp[y] != y) y = rp[y];
            if (x != y) { rp[x] = y; rc--; }
        }
        check(rc == ans_comps[t], "offline components at time t");
        chk = chk * 31u + (unsigned)(ans_comps[t] * 7 + ans_maxsz[t]);
        if (ans_comps[t] < minc) minc = ans_comps[t];
        if (ans_comps[t] > maxc) maxc = ans_comps[t];
    }
    printf("part2: edges=%d depth=%d comps_range=[%d,%d] checksum=%llu\n", ne, deepest, minc, maxc, chk % 1000003u);
    for (int t = 0; t < T; t += 8) printf("  t=%2d comps=%2d largest=%2d\n", t, ans_comps[t], ans_maxsz[t]);
    return 0;
}
