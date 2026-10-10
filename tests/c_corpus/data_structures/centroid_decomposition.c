/*
 * title: Centroid decomposition tree
 * topic: data_structures
 * covers: centroid decomposition, centroid tree depth bound, nearest marked vertex, path counting by length, BFS cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 300
#define LOGN 10

static int n;
static int head[MAXN], nxt_e[2 * MAXN], to_e[2 * MAXN], ne;
static int removed[MAXN], sz[MAXN];
static int cpar[MAXN], clevel[MAXN];
static int cdist[LOGN][MAXN]; /* distance from v to its centroid ancestor at level l */

static unsigned long long rs = 0x600DF00D5EEDULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static void add_edge(int a, int b) {
    to_e[ne] = b; nxt_e[ne] = head[a]; head[a] = ne++;
    to_e[ne] = a; nxt_e[ne] = head[b]; head[b] = ne++;
}
static int calc_size(int v, int p) {
    sz[v] = 1;
    for (int e = head[v]; e >= 0; e = nxt_e[e]) { int w = to_e[e]; if (w != p && !removed[w]) sz[v] += calc_size(w, v); }
    return sz[v];
}
static int find_centroid(int v, int p, int total) {
    for (int e = head[v]; e >= 0; e = nxt_e[e]) {
        int w = to_e[e];
        if (w != p && !removed[w] && sz[w] * 2 > total) return find_centroid(w, v, total);
    }
    return v;
}
static void fill_dist(int v, int p, int lvl, int d) {
    cdist[lvl][v] = d;
    for (int e = head[v]; e >= 0; e = nxt_e[e]) { int w = to_e[e]; if (w != p && !removed[w]) fill_dist(w, v, lvl, d + 1); }
}
static int max_level, root_c;
static int comp_max_size[MAXN];
static int decompose(int v, int parent, int lvl) {
    int total = calc_size(v, -1);
    int c = find_centroid(v, -1, total);
    cpar[c] = parent; clevel[c] = lvl;
    comp_max_size[c] = total;
    if (lvl > max_level) max_level = lvl;
    fill_dist(c, -1, lvl, 0);
    removed[c] = 1;
    for (int e = head[c]; e >= 0; e = nxt_e[e]) { int w = to_e[e]; if (!removed[w]) decompose(w, c, lvl + 1); }
    return c;
}

/* nearest-marked-vertex structure */
static int best[MAXN];
static void mark(int v) { for (int c = v; c >= 0; c = cpar[c]) { int d = cdist[clevel[c]][v]; if (d < best[c]) best[c] = d; } }
static int nearest(int v) {
    int r = 1 << 28;
    for (int c = v; c >= 0; c = cpar[c]) { int d = cdist[clevel[c]][v]; if (best[c] < (1 << 28) && best[c] + d < r) r = best[c] + d; }
    return r;
}

static int all_d[MAXN][MAXN];
static void bfs(int s, int *d) {
    int q[MAXN], h = 0, t = 0;
    for (int i = 0; i < n; i++) d[i] = -1;
    d[s] = 0; q[t++] = s;
    while (h < t) { int u = q[h++]; for (int e = head[u]; e >= 0; e = nxt_e[e]) if (d[to_e[e]] < 0) { d[to_e[e]] = d[u] + 1; q[t++] = to_e[e]; } }
}

/* count pairs at exact distance K via centroid decomposition */
static int cnt_k[MAXN + 1], cnt_sub[MAXN + 1];
static void collect(int v, int p, int d, int *cnt, int *maxd) {
    if (d > *maxd) *maxd = d;
    cnt[d]++;
    for (int e = head[v]; e >= 0; e = nxt_e[e]) { int w = to_e[e]; if (w != p && !removed[w]) collect(w, v, d + 1, cnt, maxd); }
}
static long pairs_k(int v, int K) {
    long res = 0;
    calc_size(v, -1);
    int c = find_centroid(v, -1, sz[v]);
    removed[c] = 1;
    memset(cnt_k, 0, sizeof cnt_k);
    cnt_k[0] = 1;
    int maxk = 0;
    for (int e = head[c]; e >= 0; e = nxt_e[e]) {
        int w = to_e[e];
        if (removed[w]) continue;
        memset(cnt_sub, 0, sizeof cnt_sub);
        int md = 0;
        collect(w, c, 1, cnt_sub, &md);
        for (int d = 1; d <= md; d++) if (K - d >= 0 && K - d <= maxk) res += (long)cnt_sub[d] * cnt_k[K - d];
        for (int d = 1; d <= md; d++) cnt_k[d] += cnt_sub[d];
        if (md > maxk) maxk = md;
    }
    for (int e = head[c]; e >= 0; e = nxt_e[e]) if (!removed[to_e[e]]) res += pairs_k(to_e[e], K);
    return res;
}

static void build_tree(int shape) {
    ne = 0;
    for (int i = 0; i < n; i++) head[i] = -1;
    for (int i = 1; i < n; i++) {
        int p;
        if (shape == 0) p = i - 1;                      /* path */
        else if (shape == 1) p = 0;                     /* star */
        else if (shape == 2) p = (i - 1) / 2;           /* binary heap shape */
        else p = (int)(rnd() % (unsigned)i);            /* random recursive tree */
        add_edge(p, i);
    }
}

int main(void) {
    const char *names[] = { "path", "star", "binary", "random", "random" };
    int sizes[] = { 255, 201, 255, 300, 120 };
    for (int shape = 0; shape < 5; shape++) {
        n = sizes[shape];
        build_tree(shape > 3 ? 3 : shape);
        memset(removed, 0, sizeof removed);
        max_level = 0;
        root_c = decompose(0, -1, 0);
        int lg = 0; while ((1 << lg) < n + 1) lg++;
        check(max_level + 1 <= lg, "centroid tree depth is at most log2(n+1)");
        for (int i = 0; i < n; i++) bfs(i, all_d[i]);
        /* every centroid's component has at most half the parent's component (rounded) */
        for (int v = 0; v < n; v++) if (cpar[v] >= 0) check(comp_max_size[v] * 2 <= comp_max_size[cpar[v]], "component halves");
        /* distance stored to centroid ancestors equals BFS distance */
        for (int v = 0; v < n; v++) for (int c = v; c >= 0; c = cpar[c]) check(cdist[clevel[c]][v] == all_d[c][v], "stored ancestor distance");
        /* path between u,v passes through the centroid-tree LCA: dist(u,v) == d(u,l)+d(l,v) for l=LCA */
        long lsum = 0;
        for (int t = 0; t < 2000; t++) {
            int u = (int)(rnd() % (unsigned)n), v = (int)(rnd() % (unsigned)n);
            int a = u, b = v;
            while (clevel[a] > clevel[b]) a = cpar[a];
            while (clevel[b] > clevel[a]) b = cpar[b];
            while (a != b) { a = cpar[a]; b = cpar[b]; }
            check(all_d[u][v] == all_d[u][a] + all_d[a][v], "LCA centroid lies on path");
            lsum += clevel[a];
        }
        /* nearest marked vertex with interleaved marks and queries */
        for (int i = 0; i < n; i++) best[i] = 1 << 28;
        int marked[MAXN], nm = 0; long dsum = 0;
        for (int step = 0; step < 300; step++) {
            if (step % 3 == 0) { int v = (int)(rnd() % (unsigned)n); mark(v); marked[nm++] = v; }
            else if (nm) {
                int v = (int)(rnd() % (unsigned)n), bd = 1 << 28;
                for (int i = 0; i < nm; i++) if (all_d[v][marked[i]] < bd) bd = all_d[v][marked[i]];
                check(nearest(v) == bd, "nearest marked");
                dsum += bd;
            }
        }
        /* pairs at distance K */
        int Ks[] = { 1, 2, 5 };
        long pk[3];
        for (int ki = 0; ki < 3; ki++) {
            memset(removed, 0, sizeof removed);
            pk[ki] = pairs_k(0, Ks[ki]);
            long brute = 0;
            for (int u = 0; u < n; u++) for (int v = u + 1; v < n; v++) if (all_d[u][v] == Ks[ki]) brute++;
            check(pk[ki] == brute, "pairs at distance K");
        }
        printf("%-6s n=%3d centroid=%3d levels=%d dist-sum(marked)=%ld lca-level-sum=%ld pairs(d=1,2,5)=%ld,%ld,%ld\n",
               names[shape], n, root_c, max_level + 1, dsum, lsum, pk[0], pk[1], pk[2]);
    }
    return 0;
}
