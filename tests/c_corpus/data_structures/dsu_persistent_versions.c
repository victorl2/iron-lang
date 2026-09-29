/*
 * title: Persistent union-find through a path-copied array
 * topic: data_structures
 * covers: persistent DSU, persistent array as radix tree, structural sharing, versions, union by size without compression, snapshot oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 64         /* elements, leaves hold 4 entries, 16 leaves, fanout 4 => depth 2 */
#define LEAF 4
#define VERS 60

static unsigned long long rs = 0x9E25157ULL * 12289;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* Persistent array of ints: 3-level tree, fanout 4. Nodes are immutable once shared. */
typedef struct Node { int is_leaf; int v[4]; struct Node *c[4]; } Node;
static Node *pool[20000]; static int npool;

static Node *mk(void) { Node *n = calloc(1, sizeof *n); pool[npool++] = n; return n; }
static Node *build(int level, int base, int val_init) {
    Node *n = mk();
    if (level == 0) { n->is_leaf = 1; for (int i = 0; i < 4; i++) n->v[i] = val_init ? 1 : base + i; return n; }
    int span = 1; for (int i = 0; i < level; i++) span *= 4;
    for (int i = 0; i < 4; i++) n->c[i] = build(level - 1, base + i * span, val_init);
    return n;
}
static int pget(const Node *root, int i) {
    const Node *n = root;
    for (int level = 2; level > 0; level--) { int shift = level * 2; n = n->c[(i >> shift) & 3]; }
    return n->v[i & 3];
}
static Node *pset(const Node *n, int level, int i, int x) {
    Node *m = mk(); *m = *n;
    if (level == 0) { m->v[i & 3] = x; return m; }
    int slot = (i >> (level * 2)) & 3;
    m->c[slot] = pset(n->c[slot], level - 1, i, x);
    return m;
}

typedef struct { Node *par, *sz; int comps; } Ver;
static Ver ver[VERS + 1];
static int nver;

static int find(const Ver *v, int x) { while (pget(v->par, x) != x) x = pget(v->par, x); return x; }
static void unite_new(int from, int a, int b) {  /* creates ver[nver] from ver[from] */
    Ver nv = ver[from];
    int ra = find(&ver[from], a), rb = find(&ver[from], b);
    if (ra != rb) {
        int sa = pget(nv.sz, ra), sb = pget(nv.sz, rb);
        if (sa < sb) { int t = ra; ra = rb; rb = t; }
        nv.par = pset(nv.par, 2, rb, ra);
        nv.sz = pset(nv.sz, 2, ra, sa + sb);
        nv.comps--;
    }
    ver[nver++] = nv;
}

int main(void) {
    ver[0].par = build(2, 0, 0); ver[0].sz = build(2, 0, 1); ver[0].comps = N; nver = 1;
    int nodes_v0 = npool;
    /* history for oracle: op list per version */
    int oa[VERS + 1], ob[VERS + 1], of[VERS + 1];
    for (int v = 1; v <= VERS; v++) {
        int from = (int)(rnd() % (unsigned)v);           /* branch from any earlier version */
        int a = (int)(rnd() % N), b = (int)(rnd() % N);
        oa[v] = a; ob[v] = b; of[v] = from;
        unite_new(from, a, b);
    }
    check(nver == VERS + 1, "version count");
    /* oracle: rebuild version v's connectivity by replaying its ancestry chain */
    int total_pairs = 0, max_comps = 0, min_comps = N, max_depth = 0;
    for (int v = 0; v <= VERS; v++) {
        int chain[VERS + 1], cn = 0, x = v;
        while (x != 0) { chain[cn++] = x; x = of[x]; }
        if (cn > max_depth) max_depth = cn;
        int rp[N], rc = N;
        for (int i = 0; i < N; i++) rp[i] = i;
        for (int k = cn - 1; k >= 0; k--) {
            int p = oa[chain[k]], q = ob[chain[k]];
            while (rp[p] != p) p = rp[p];
            while (rp[q] != q) q = rp[q];
            if (p != q) { rp[p] = q; rc--; }
        }
        check(rc == ver[v].comps, "component count per version");
        for (int i = 0; i < N; i++) for (int j = i + 1; j < N; j++) {
            int p = i, q = j;
            while (rp[p] != p) p = rp[p];
            while (rp[q] != q) q = rp[q];
            check((p == q) == (find(&ver[v], i) == find(&ver[v], j)), "connectivity per version");
            total_pairs += p == q;
        }
        if (ver[v].comps > max_comps) max_comps = ver[v].comps;
        if (ver[v].comps < min_comps) min_comps = ver[v].comps;
    }
    printf("versions=%d max_chain=%d comps_range=[%d,%d] connected_pair_total=%d\n", nver, max_depth, min_comps, max_comps, total_pairs);
    printf("nodes initially=%d, total allocated=%d, avg new nodes per union=%d\n", nodes_v0, npool, (npool - nodes_v0) / VERS);
    printf("sample: ");
    for (int v = 0; v <= VERS; v += 12) printf("v%d:%d ", v, ver[v].comps);
    printf("\n");
    for (int i = 0; i < npool; i++) free(pool[i]);
    return 0;
}
