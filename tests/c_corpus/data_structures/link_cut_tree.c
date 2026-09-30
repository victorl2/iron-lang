/*
 * title: Link-cut tree with path aggregates
 * topic: data_structures
 * covers: link-cut tree, splay on preferred paths, access, evert, link, cut, path sum and max, connectivity
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 40

static int ch[N + 1][2], par[N + 1], rev[N + 1];
static long val[N + 1], sum[N + 1], mx[N + 1];

static unsigned long long rs = 0xBADC0FFEE0DDF00DULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* node 0 is the null sentinel */
static int is_root(int x) { return ch[par[x]][0] != x && ch[par[x]][1] != x; }
static void pull(int x) {
    sum[x] = sum[ch[x][0]] + val[x] + sum[ch[x][1]];
    mx[x] = val[x];
    if (ch[x][0] && mx[ch[x][0]] > mx[x]) mx[x] = mx[ch[x][0]];
    if (ch[x][1] && mx[ch[x][1]] > mx[x]) mx[x] = mx[ch[x][1]];
}
static void push(int x) {
    if (rev[x]) {
        int t = ch[x][0]; ch[x][0] = ch[x][1]; ch[x][1] = t;
        if (ch[x][0]) rev[ch[x][0]] ^= 1;
        if (ch[x][1]) rev[ch[x][1]] ^= 1;
        rev[x] = 0;
    }
}
static void rotate(int x) {
    int p = par[x], g = par[p], d = ch[p][1] == x;
    if (!is_root(p)) ch[g][ch[g][1] == p] = x;
    par[x] = g;
    ch[p][d] = ch[x][!d]; if (ch[x][!d]) par[ch[x][!d]] = p;
    ch[x][!d] = p; par[p] = x;
    pull(p); pull(x);
}
static int stk[N + 2];
static void splay(int x) {
    int top = 0, y = x;
    stk[top++] = y;
    while (!is_root(y)) { y = par[y]; stk[top++] = y; }
    while (top) push(stk[--top]);
    while (!is_root(x)) {
        int p = par[x], g = par[p];
        if (!is_root(p)) rotate(((ch[g][0] == p) == (ch[p][0] == x)) ? p : x);
        rotate(x);
    }
}
static void access(int x) {
    int last = 0;
    for (int y = x; y; y = par[y]) { splay(y); ch[y][1] = last; pull(y); last = y; }
    splay(x);
}
static void make_root(int x) { access(x); rev[x] ^= 1; }
static int find_root(int x) {
    access(x);
    while (1) { push(x); if (!ch[x][0]) break; x = ch[x][0]; }
    splay(x);
    return x;
}
static int connected(int a, int b) { return find_root(a) == find_root(b); }
static void link(int a, int b) { make_root(a); par[a] = b; }
static void cut(int a, int b) { make_root(a); access(b); /* b's left child is a */ ch[b][0] = 0; par[a] = 0; pull(b); }
static void set_val(int x, long v) { access(x); val[x] = v; pull(x); }
static void path_query(int a, int b, long *s, long *m) { make_root(a); access(b); *s = sum[b]; *m = mx[b]; }
static int lca_check(int a, int b) { access(a); int last = 0, y = b; int res = 0; for (; y; y = par[y]) { splay(y); ch[y][1] = last; pull(y); last = y; res = y; } return res; }

/* brute-force forest */
static int adj[N + 1][N + 1];
static int seen_par[N + 1];
static int dfs_path(int u, int target, int p) {
    seen_par[u] = p;
    if (u == target) return 1;
    for (int v = 1; v <= N; v++) if (adj[u][v] && v != p && dfs_path(v, target, u)) return 1;
    return 0;
}
static int b_connected(int a, int b) { return dfs_path(a, b, 0); }
static void b_path(int a, int b, long *s, long *m) {
    dfs_path(a, b, 0);
    *s = 0; *m = -1000000;
    for (int x = b; x; x = seen_par[x]) { *s += val[x]; if (val[x] > *m) *m = val[x]; }
}

int main(void) {
    for (int i = 1; i <= N; i++) { val[i] = (long)(rnd() % 100); sum[i] = mx[i] = val[i]; }
    int edges = 0, links = 0, cuts = 0, queries = 0, lcas = 0, updates = 0;
    long checksum = 0;
    for (int step = 0; step < 4000; step++) {
        int a = 1 + (int)(rnd() % N), b = 1 + (int)(rnd() % N);
        unsigned op = rnd() % 10;
        if (a == b) continue;
        if (op < 3) {
            int c = b_connected(a, b);
            check(connected(a, b) == c, "connected");
            if (!c) { link(a, b); adj[a][b] = adj[b][a] = 1; edges++; links++; }
        } else if (op < 5) {
            if (edges > 0) { /* pick a random existing edge */
                int k = (int)(rnd() % (unsigned)edges);
                for (a = 1; a <= N; a++) { for (b = a + 1; b <= N; b++) if (adj[a][b] && k-- == 0) break; if (b <= N) break; }
                check(a <= N, "edge found");
                cut(a, b); adj[a][b] = adj[b][a] = 0; edges--; cuts++;
            }
        } else if (op < 6) {
            long v = (long)(rnd() % 100);
            set_val(a, v); /* val[] is also the brute-force array */
            updates++;
        } else if (op < 9) {
            int c = b_connected(a, b);
            check(connected(a, b) == c, "connected query");
            if (c) {
                long s, m, bs, bm;
                b_path(a, b, &bs, &bm);
                path_query(a, b, &s, &m);
                check(s == bs && m == bm, "path aggregates");
                checksum += s * 7 + m;
                queries++;
            }
        } else {
            /* lca in tree rooted at 1, when both connected to 1 */
            if (b_connected(1, a) && b_connected(1, b)) {
                make_root(1);
                int l = lca_check(a, b);
                /* brute force: path from 1 to a marks ancestors */
                dfs_path(1, a, 0);
                int anc[N + 1] = {0};
                for (int x = a; x; x = seen_par[x]) anc[x] = 1;
                dfs_path(1, b, 0);
                int want = 0;
                for (int x = b; x; x = seen_par[x]) if (anc[x]) { want = x; break; }
                check(l == want, "lca");
                lcas++;
            }
        }
        if (step % 500 == 499) printf("step %4d: edges=%2d links=%d cuts=%d updates=%d path-queries=%d lcas=%d\n", step + 1, edges, links, cuts, updates, queries, lcas);
    }
    printf("checksum %ld\n", checksum);
    /* build a long path 1-2-...-N, verify aggregate across it, cut in the middle */
    for (int a = 1; a <= N; a++) for (int b = 1; b <= N; b++) if (adj[a][b]) { cut(a, b); adj[a][b] = 0; }
    for (int i = 1; i < N; i++) { check(!connected(i, i + 1), "isolated before link"); link(i, i + 1); }
    long s, m, tot = 0, big = -1;
    for (int i = 1; i <= N; i++) { tot += val[i]; if (val[i] > big) big = val[i]; }
    path_query(1, N, &s, &m);
    check(s == tot && m == big, "whole path");
    cut(N / 2, N / 2 + 1);
    check(!connected(1, N), "path cut");
    printf("path total %ld max %ld, connected after cut: %d\n", tot, big, connected(1, N));
    return 0;
}
