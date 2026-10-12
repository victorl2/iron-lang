/*
 * title: Dominator tree by Lengauer-Tarjan
 * topic: data_structures
 * covers: dominator tree, lengauer-tarjan, semidominators, path compression, dominance frontier, removal brute force
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 64

static int n;
static unsigned char adj[MAXN][MAXN];

static unsigned long long rs = 0xD0D0CAFE12345ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* ---- Lengauer-Tarjan (with simple path compression) ---- */
static int dfn[MAXN], vertex[MAXN], dpar[MAXN], semi[MAXN], idom_lt[MAXN], ancestor[MAXN], label[MAXN], cnt;
static int bucket[MAXN][MAXN], bn[MAXN];
static void dfs(int v) {
    dfn[v] = ++cnt; vertex[cnt] = v; semi[v] = dfn[v]; label[v] = v; ancestor[v] = -1;
    for (int w = 0; w < n; w++) if (adj[v][w] && !dfn[w]) { dpar[w] = v; dfs(w); }
}
static void compress(int v) {
    int a = ancestor[v];
    if (ancestor[a] >= 0) {
        compress(a);
        if (semi[label[a]] < semi[label[v]]) label[v] = label[a];
        ancestor[v] = ancestor[a];
    }
}
static int eval(int v) {
    if (ancestor[v] < 0) return v;
    compress(v);
    return label[v];
}
static int lengauer_tarjan(int root) {
    memset(dfn, 0, sizeof dfn); memset(bn, 0, sizeof bn);
    cnt = 0;
    for (int i = 0; i < n; i++) { idom_lt[i] = -1; dpar[i] = -1; }
    dfs(root);
    for (int i = cnt; i >= 2; i--) {
        int w = vertex[i];
        for (int v = 0; v < n; v++) {
            if (!adj[v][w] || !dfn[v]) continue;
            int u = eval(v);
            if (semi[u] < semi[w]) semi[w] = semi[u];
        }
        bucket[vertex[semi[w]]][bn[vertex[semi[w]]]++] = w;
        ancestor[w] = dpar[w];
        int p = dpar[w];
        for (int k = 0; k < bn[p]; k++) {
            int v = bucket[p][k];
            int u = eval(v);
            idom_lt[v] = semi[u] < semi[v] ? u : p;
        }
        bn[p] = 0;
    }
    for (int i = 2; i <= cnt; i++) {
        int w = vertex[i];
        if (idom_lt[w] != vertex[semi[w]]) idom_lt[w] = idom_lt[idom_lt[w]];
    }
    idom_lt[root] = -1;
    return cnt;
}

/* ---- brute force: d dominates v iff v unreachable when d removed ---- */
static int reach_without(int root, int removed, int *seen) {
    int stack[MAXN], sp = 0, c = 0;
    memset(seen, 0, sizeof(int) * MAXN);
    if (root == removed) return 0;
    seen[root] = 1; stack[sp++] = root;
    while (sp) { int u = stack[--sp]; c++; for (int w = 0; w < n; w++) if (adj[u][w] && !seen[w] && w != removed) { seen[w] = 1; stack[sp++] = w; } }
    return c;
}
static int dom[MAXN][MAXN]; /* dom[d][v] */
static int brute_idom[MAXN];
static void brute(int root) {
    int seen0[MAXN], seen[MAXN];
    reach_without(root, -1, seen0);
    for (int d = 0; d < n; d++) {
        reach_without(root, d, seen);
        for (int v = 0; v < n; v++) dom[d][v] = seen0[v] && (d == v || !seen[v]);
    }
    for (int v = 0; v < n; v++) {
        brute_idom[v] = -1;
        if (!seen0[v] || v == root) continue;
        /* idom = strict dominator dominated by all other strict dominators */
        for (int d = 0; d < n; d++) {
            if (d == v || !dom[d][v]) continue;
            int ok = 1;
            for (int e = 0; e < n; e++) if (e != v && e != d && dom[e][v] && !dom[e][d]) ok = 0;
            if (ok) brute_idom[v] = d;
        }
    }
}
static int depth_in_tree(int v) { int d = 0; while (idom_lt[v] >= 0) { v = idom_lt[v]; d++; } return d; }

int main(void) {
    int total_reach = 0, maxdepth_all = 0, graphs = 0;
    long df_total = 0;
    for (int g = 0; g < 60; g++) {
        n = 6 + (int)(rnd() % 30);
        if (g < 2) n = 12;
        memset(adj, 0, sizeof adj);
        int m = n + (int)(rnd() % (unsigned)(2 * n));
        if (g == 0) { /* classic diamond-with-loop example */
            static const int e[][2] = {{0,1},{0,2},{1,3},{2,3},{3,4},{4,3},{4,5},{5,6},{3,6},{6,7},{7,1},{2,8},{8,9},{9,10},{10,8},{10,11}};
            for (unsigned i = 0; i < sizeof e / sizeof e[0]; i++) adj[e[i][0]][e[i][1]] = 1;
        } else {
            for (int i = 0; i < m; i++) { int a = (int)(rnd() % (unsigned)n), b = (int)(rnd() % (unsigned)n); if (a != b || rnd() % 8 == 0) adj[a][b] = 1; }
        }
        int reach = lengauer_tarjan(0);
        brute(0);
        for (int v = 0; v < n; v++) {
            if (!dfn[v]) { check(brute_idom[v] == -1, "unreachable has no idom"); continue; }
            check(idom_lt[v] == brute_idom[v], "idom matches brute force");
        }
        /* dominator relation from the tree equals brute force */
        for (int d = 0; d < n; d++) for (int v = 0; v < n; v++) {
            if (!dfn[v] || !dfn[d]) continue;
            int x = v, isdom = 0;
            for (;;) { if (x == d) { isdom = 1; break; } if (idom_lt[x] < 0) break; x = idom_lt[x]; }
            check(isdom == dom[d][v], "tree ancestor == dominates");
        }
        /* dominance frontier by definition: v in DF(d) iff d dominates a pred of v but not strictly v */
        long dfs_count = 0;
        for (int d = 0; d < n; d++) for (int v = 0; v < n; v++) {
            if (!dfn[d] || !dfn[v]) continue;
            int hit = 0;
            for (int p = 0; p < n; p++) if (adj[p][v] && dfn[p] && dom[d][p]) hit = 1;
            int strict = dom[d][v] && d != v;
            if (hit && !strict) dfs_count++;
        }
        /* DF via the standard bottom-up runner walk must give the same count */
        long runner_count = 0;
        static char inDF[MAXN][MAXN];
        memset(inDF, 0, sizeof inDF);
        for (int v = 0; v < n; v++) {
            if (!dfn[v]) continue;
            int preds = 0; for (int p = 0; p < n; p++) if (adj[p][v] && dfn[p]) preds++;
            if (preds < 2) continue;
            for (int p = 0; p < n; p++) if (adj[p][v] && dfn[p]) {
                int r = p;
                while (r != idom_lt[v] && r >= 0) { if (!inDF[r][v]) { inDF[r][v] = 1; runner_count++; } r = idom_lt[r]; }
            }
        }
        /* a join node (>=2 preds) contributes through runners; single-pred nodes contribute too via self loops; compare only join nodes */
        long join_by_def = 0;
        for (int d = 0; d < n; d++) for (int v = 0; v < n; v++) {
            if (!dfn[d] || !dfn[v]) continue;
            int preds = 0; for (int p = 0; p < n; p++) if (adj[p][v] && dfn[p]) preds++;
            if (preds < 2) continue;
            int hit = 0; for (int p = 0; p < n; p++) if (adj[p][v] && dfn[p] && dom[d][p]) hit = 1;
            if (hit && !(dom[d][v] && d != v)) join_by_def++;
        }
        check(join_by_def == runner_count, "dominance frontier at join nodes");
        int md = 0; for (int v = 0; v < n; v++) if (dfn[v]) { int d = depth_in_tree(v); if (d > md) md = d; }
        if (md > maxdepth_all) maxdepth_all = md;
        total_reach += reach; df_total += dfs_count; graphs++;
        if (g < 2 || g % 15 == 0) {
            printf("graph %2d: n=%2d reachable=%2d tree-depth=%d |DF|=%ld idom:", g, n, reach, md, dfs_count);
            for (int v = 1; v < n && v < 14; v++) printf(" %d", dfn[v] ? idom_lt[v] : -9);
            printf("\n");
        }
    }
    printf("%d graphs, reachable total %d, max dominator depth %d, |DF| total %ld\n", graphs, total_reach, maxdepth_all, df_total);
    return 0;
}
