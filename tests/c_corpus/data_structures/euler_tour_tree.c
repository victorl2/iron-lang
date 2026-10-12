/*
 * title: Euler tour tree on an implicit treap
 * topic: data_structures
 * covers: euler tour tree, dynamic forest, link, cut, connectivity, subtree size, implicit treap split and merge
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 30
#define MAXT (N + N * N)

/* Tokens: id < N is the vertex token; id N + u*N + v is the directed edge token u->v.
   The tour of each tree is a cyclic sequence stored linearly in a treap. */
typedef struct { int l, r, p, size, vcnt; unsigned pri; int isv; } T;
static T t[MAXT + 1]; /* index token id + 1; 0 is null */

static unsigned long long rs = 0x4242424242ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int vtok(int v) { return v + 1; }
static int etok(int u, int v) { return N + u * N + v + 1; }

static void upd(int x) {
    t[x].size = 1 + t[t[x].l].size + t[t[x].r].size;
    t[x].vcnt = t[x].isv + t[t[x].l].vcnt + t[t[x].r].vcnt;
    if (t[x].l) t[t[x].l].p = x;
    if (t[x].r) t[t[x].r].p = x;
}
static int merge(int a, int b) {
    if (!a) return b;
    if (!b) return a;
    if (t[a].pri > t[b].pri) { t[a].r = merge(t[a].r, b); upd(a); t[a].p = 0; return a; }
    t[b].l = merge(a, t[b].l); upd(b); t[b].p = 0; return b;
}
/* split first k elements into a, rest into b */
static void split(int x, int k, int *a, int *b) {
    if (!x) { *a = *b = 0; return; }
    if (t[t[x].l].size >= k) { split(t[x].l, k, a, &t[x].l); *b = x; }
    else { split(t[x].r, k - t[t[x].l].size - 1, &t[x].r, b); *a = x; }
    upd(x);
    t[x].p = 0;
    if (*a) t[*a].p = 0;
    if (*b) t[*b].p = 0;
}
static int root_of(int x) { while (t[x].p) x = t[x].p; return x; }
static int index_of(int x) { /* 0-based position in its sequence */
    int idx = t[t[x].l].size;
    while (t[x].p) { int p = t[x].p; if (t[p].r == x) idx += t[t[p].l].size + 1; x = p; }
    return idx;
}
static void init_tok(int id, int isv) {
    memset(&t[id], 0, sizeof t[id]);
    t[id].size = 1; t[id].isv = isv; t[id].vcnt = isv; t[id].pri = rnd() | 1u;
}
static int adj[N][N];

static int connected(int u, int v) { return root_of(vtok(u)) == root_of(vtok(v)); }
static int comp_size(int u) { return t[root_of(vtok(u))].vcnt; }
static void link(int u, int v) {
    /* cut u's cycle right after V(u) and splice v's whole cycle between the two edge tokens */
    int ru = root_of(vtok(u)), rv = root_of(vtok(v));
    int pos = index_of(vtok(u)) + 1;
    int a, b;
    split(ru, pos, &a, &b);
    init_tok(etok(u, v), 0); init_tok(etok(v, u), 0);
    /* rotate v's cycle so that it starts at V(v): the seam then lies on v's side of every v edge */
    int va, vb;
    split(rv, index_of(vtok(v)), &va, &vb);
    rv = merge(vb, va);
    merge(merge(merge(a, etok(u, v)), rv), merge(etok(v, u), b));
}
static void cut(int u, int v) {
    int x = etok(u, v), y = etok(v, u);
    int i = index_of(x), j = index_of(y);
    if (i > j) { int tmp = i; i = j; j = tmp; }
    int rt = root_of(x);
    int a, rest, mid, c;
    split(rt, i, &a, &rest);
    split(rest, 1, &c, &rest); /* c = first edge token */
    split(rest, j - i - 1, &mid, &rest);
    int d;
    split(rest, 1, &d, &rest); /* d = second edge token */
    merge(a, rest);
    (void)mid; (void)c; (void)d;
}
/* number of vertices on x's side of tree edge (p,x): the tokens between E(p->x) and E(x->p) */
static int subtree_size(int p, int x) {
    int i = index_of(etok(p, x)), j = index_of(etok(x, p));
    int lo = i < j ? i : j, hi = i < j ? j : i;
    int rt = root_of(etok(p, x));
    int total = t[rt].vcnt;
    int a, b, m, c;
    split(rt, lo + 1, &a, &b);
    split(b, hi - lo - 1, &m, &c);
    int inner = t[m].vcnt;
    merge(merge(a, m), c);
    return i < j ? inner : total - inner;
}

/* brute force */
static int dfs_count(int u, int forbid, int *mark) {
    int c = 1; mark[u] = 1;
    for (int v = 0; v < N; v++) if (adj[u][v] && v != forbid && !mark[v]) c += dfs_count(v, forbid, mark);
    return c;
}
static int b_comp(int u) { int m[N] = {0}; return dfs_count(u, -1, m); }
static int b_connected(int u, int v) { int m[N] = {0}; dfs_count(u, -1, m); return m[v]; }
static int b_subtree(int p, int x) { /* size of x's side after removing edge p-x */
    int m[N] = {0}; m[p] = 1; int c = 1; m[x] = 1;
    int stack[N], sp = 0; stack[sp++] = x;
    while (sp) { int u = stack[--sp]; for (int v = 0; v < N; v++) if (adj[u][v] && !m[v]) { m[v] = 1; c++; stack[sp++] = v; } }
    return c;
}

int main(void) {
    for (int v = 0; v < N; v++) init_tok(vtok(v), 1);
    int links = 0, cuts = 0, qs = 0, edges = 0, maxcomp = 1;
    long checksum = 0;
    for (int step = 0; step < 3000; step++) {
        int u = (int)(rnd() % N), v = (int)(rnd() % N);
        if (u == v) continue;
        unsigned op = rnd() % 10;
        if (op < 4) {
            int c = b_connected(u, v);
            check(connected(u, v) == c, "connected");
            if (!c) { link(u, v); adj[u][v] = adj[v][u] = 1; links++; edges++; }
        } else if (op < 6) {
            if (edges > 0) { /* pick a random existing edge */
                int k = (int)(rnd() % (unsigned)edges);
                for (u = 0; u < N; u++) { for (v = u + 1; v < N; v++) if (adj[u][v] && k-- == 0) break; if (v < N) break; }
                check(u < N, "edge found");
                cut(u, v); adj[u][v] = adj[v][u] = 0; cuts++; edges--;
            }
        } else if (op < 8) {
            check(comp_size(u) == b_comp(u), "component size");
            if (comp_size(u) > maxcomp) maxcomp = comp_size(u);
            qs++;
        } else if (adj[u][v]) {
            int s1 = subtree_size(u, v), s2 = subtree_size(v, u);
            check(s1 == b_subtree(u, v), "subtree size u->v");
            check(s2 == b_subtree(v, u), "subtree size v->u");
            check(s1 + s2 == comp_size(u), "two sides partition the tree");
            checksum += s1 * 31 + s2;
            qs++;
        }
        if (step % 500 == 499) {
            /* every sequence length is 3*(vertices) - 2 for a tree of k vertices: k + 2(k-1) */
            int ok = 1;
            for (int x = 0; x < N; x++) { int r = root_of(vtok(x)); if (t[r].size != 3 * t[r].vcnt - 2) ok = 0; }
            check(ok, "tour length 3k-2");
            printf("step %4d: edges=%2d links=%d cuts=%d queries=%d largest component=%d\n", step + 1, edges, links, cuts, qs, maxcomp);
        }
    }
    printf("checksum %ld\n", checksum);
    return 0;
}
