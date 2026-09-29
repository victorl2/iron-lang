/*
 * title: M-tree lite for metric range and kNN search
 * topic: data_structures
 * covers: m-tree, routing objects, covering radii, parent-distance pruning, node split by farthest pair, knn branch and bound
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPN 6
#define NP 1200
#define DIM 4

typedef struct MN MN;
typedef struct { int obj; int radius; int pdist; MN *child; } Ent;
struct MN { int leaf, n; Ent e[CAPN + 1]; };

static int P[NP][DIM];
static long dcalls;

static unsigned long long rs = 0xA11CE5EEDULL * 17;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int dist(int a, int b) { dcalls++; int s = 0; for (int i = 0; i < DIM; i++) s += abs(P[a][i] - P[b][i]); return s; }
static int distq(const int *q, int b) { dcalls++; int s = 0; for (int i = 0; i < DIM; i++) s += abs(q[i] - P[b][i]); return s; }

static MN *mk(int leaf) { MN *n = calloc(1, sizeof *n); n->leaf = leaf; return n; }
static int splits;

/* split overfull node n: promote two objects, partition, return two new nodes and promoted entries */
static void split_node(MN *n, Ent *pa, Ent *pb, MN **na, MN **nb) {
    splits++;
    int cnt = n->n, a = 0, b = 1, best = -1;
    for (int i = 0; i < cnt; i++) for (int j = i + 1; j < cnt; j++) { int d = dist(n->e[i].obj, n->e[j].obj); if (d > best) { best = d; a = i; b = j; } }
    *na = mk(n->leaf); *nb = mk(n->leaf);
    int oa = n->e[a].obj, ob = n->e[b].obj;
    int da[CAPN + 1], db[CAPN + 1], side[CAPN + 1];
    int cA = 0, cB = 0;
    for (int i = 0; i < cnt; i++) {
        da[i] = i == a ? 0 : dist(oa, n->e[i].obj);
        db[i] = i == b ? 0 : dist(ob, n->e[i].obj);
        side[i] = (da[i] < db[i] || (da[i] == db[i] && cA <= cB)) ? 0 : 1;
        if (i == a) side[i] = 0;
        if (i == b) side[i] = 1;
        if (side[i]) cB++; else cA++;
    }
    int ra = 0, rb = 0;
    for (int i = 0; i < cnt; i++) {
        Ent e = n->e[i];
        MN *dst = side[i] ? *nb : *na;
        e.pdist = side[i] ? db[i] : da[i];
        dst->e[dst->n++] = e;
        int reach = e.pdist + e.radius;
        if (side[i]) { if (reach > rb) rb = reach; } else { if (reach > ra) ra = reach; }
    }
    pa->obj = oa; pa->radius = ra; pa->child = *na; pa->pdist = 0;
    pb->obj = ob; pb->radius = rb; pb->child = *nb; pb->pdist = 0;
    free(n);
}
/* insert object o into subtree of n whose routing object is `parent` (or -1 at the root); returns 1 if n was split */
static int insert_rec(MN *n, int o, int parent, Ent *pa, Ent *pb, MN **na, MN **nb) {
    if (n->leaf) {
        n->e[n->n].obj = o; n->e[n->n].radius = 0; n->e[n->n].pdist = parent >= 0 ? dist(o, parent) : 0; n->e[n->n].child = NULL; n->n++;
    } else {
        int best = -1, bd = 0, bestin = 0, bestgrow = 1 << 30;
        for (int i = 0; i < n->n; i++) {
            int d = dist(o, n->e[i].obj);
            if (d <= n->e[i].radius) { if (!bestin || d < bd) { best = i; bd = d; bestin = 1; } }
            else if (!bestin) { int grow = d - n->e[i].radius; if (grow < bestgrow) { bestgrow = grow; best = i; bd = d; } }
        }
        if (bd > n->e[best].radius) n->e[best].radius = bd;
        Ent qa, qb; MN *ca, *cb;
        if (insert_rec(n->e[best].child, o, n->e[best].obj, &qa, &qb, &ca, &cb)) {
            /* replace entry `best` by the two promoted routing entries */
            qa.pdist = parent >= 0 ? dist(qa.obj, parent) : 0;
            qb.pdist = parent >= 0 ? dist(qb.obj, parent) : 0;
            n->e[best] = qa;
            n->e[n->n++] = qb;
        }
    }
    if (n->n > CAPN) { split_node(n, pa, pb, na, nb); return 1; }
    return 0;
}
static void insert(MN **root, int o) {
    Ent a, b; MN *na, *nb;
    if (insert_rec(*root, o, -1, &a, &b, &na, &nb)) {
        MN *r = mk(0);
        r->e[0] = a; r->e[1] = b; r->n = 2;
        *root = r;
    }
}
static void range(const MN *n, const int *q, int r, int dparent, int *out, int *cnt) {
    for (int i = 0; i < n->n; i++) {
        const Ent *e = &n->e[i];
        if (dparent >= 0 && abs(dparent - e->pdist) > r + e->radius) continue; /* no distance computed */
        int d = distq(q, e->obj);
        if (n->leaf) { if (d <= r) out[(*cnt)++] = e->obj; }
        else if (d <= r + e->radius) range(e->child, q, r, d, out, cnt);
    }
}
typedef struct { int d[12], id[12], n, k; } Best;
static int worse(int d1, int i1, int d2, int i2) { return d1 != d2 ? d1 > d2 : i1 > i2; }
static int tau(const Best *b) { return b->n < b->k ? 1 << 30 : b->d[b->n - 1]; }
static void offer(Best *b, int d, int id) {
    if (b->n == b->k && !worse(b->d[b->n - 1], b->id[b->n - 1], d, id)) return;
    int i = b->n < b->k ? b->n++ : b->n - 1;
    while (i > 0 && worse(b->d[i - 1], b->id[i - 1], d, id)) { b->d[i] = b->d[i - 1]; b->id[i] = b->id[i - 1]; i--; }
    b->d[i] = d; b->id[i] = id;
}
static void knn(const MN *n, const int *q, int dparent, Best *b) {
    int dd[CAPN + 1], ord[CAPN + 1], m = 0;
    for (int i = 0; i < n->n; i++) {
        const Ent *e = &n->e[i];
        if (dparent >= 0 && abs(dparent - e->pdist) - e->radius > tau(b)) { continue; }
        int d = distq(q, e->obj);
        if (n->leaf) offer(b, d, e->obj);
        else { dd[m] = d - e->radius; ord[m] = i; m++; }
    }
    if (n->leaf) return;
    for (int i = 1; i < m; i++) { int v = dd[i], o = ord[i], j = i - 1; while (j >= 0 && dd[j] > v) { dd[j + 1] = dd[j]; ord[j + 1] = ord[j]; j--; } dd[j + 1] = v; ord[j + 1] = o; }
    for (int i = 0; i < m; i++) {
        const Ent *e = &n->e[ord[i]];
        if (dd[i] > tau(b)) break;
        knn(e->child, q, distq(q, e->obj), b);
    }
}
static int depth_check(const MN *n, int *leafd, int d, int *count, int *nodes) {
    (*nodes)++;
    if (n->leaf) { if (*leafd < 0) *leafd = d; else check(*leafd == d, "balanced"); *count += n->n; return 0; }
    for (int i = 0; i < n->n; i++) depth_check(n->e[i].child, leafd, d + 1, count, nodes);
    return 0;
}
/* every object below entry e is within e->radius of e->obj; pdist is exact */
static void collect_check(const MN *n, int obj, int radius) {
    for (int i = 0; i < n->n; i++) {
        const Ent *e = &n->e[i];
        if (obj >= 0) { check(dist(obj, e->obj) <= radius, "covering radius"); }
        if (!n->leaf) collect_check(e->child, obj, radius);
    }
}
static void verify(const MN *n, int parent) {
    for (int i = 0; i < n->n; i++) {
        const Ent *e = &n->e[i];
        if (parent >= 0) check(e->pdist == dist(parent, e->obj), "parent distance exact");
        if (!n->leaf) { collect_check(e->child, e->obj, e->radius); verify(e->child, e->obj); }
    }
}
static void destroy(MN *n) { if (!n->leaf) for (int i = 0; i < n->n; i++) destroy(n->e[i].child); free(n); }

int main(void) {
    for (int i = 0; i < NP; i++) {
        int c = (int)(rnd() % 8);
        for (int d = 0; d < DIM; d++) P[i][d] = (i % 4 == 0) ? (int)(rnd() % 400) : (c * 47 + d * 13) % 400 + (int)(rnd() % 30);
    }
    MN *root = mk(1);
    for (int i = 0; i < NP; i++) insert(&root, i);
    int leafd = -1, count = 0, nodes = 0;
    depth_check(root, &leafd, 0, &count, &nodes);
    check(count == NP, "all objects stored once");
    verify(root, -1);
    printf("objects %d nodes %d height %d splits %d\n", count, nodes, leafd + 1, splits);
    for (int r = 15; r <= 60; r *= 2) {
        long hits = 0, tc = 0;
        for (int q = 0; q < 100; q++) {
            int qp[DIM];
            int base = (int)(rnd() % NP);
            for (int d = 0; d < DIM; d++) qp[d] = P[base][d] + (int)(rnd() % 9) - 4;
            int out[NP], cnt = 0; dcalls = 0;
            range(root, qp, r, -1, out, &cnt);
            tc += dcalls;
            int want = 0;
            for (int i = 0; i < NP; i++) if (distq(qp, i) <= r) want++;
            check(cnt == want, "range vs brute force");
            hits += cnt;
        }
        printf("range r=%2d: %ld hits over 100 queries, avg distance computations %ld (brute %d)\n", r, hits, tc / 100, NP);
    }
    int ks[] = { 1, 5, 10 };
    for (int ki = 0; ki < 3; ki++) {
        long tc = 0, sd = 0;
        for (int q = 0; q < 100; q++) {
            int qp[DIM];
            for (int d = 0; d < DIM; d++) qp[d] = (int)(rnd() % 450);
            Best b; b.n = 0; b.k = ks[ki]; dcalls = 0;
            knn(root, qp, -1, &b);
            tc += dcalls;
            Best r; r.n = 0; r.k = ks[ki];
            for (int i = 0; i < NP; i++) offer(&r, distq(qp, i), i);
            for (int i = 0; i < ks[ki]; i++) { check(b.d[i] == r.d[i] && b.id[i] == r.id[i], "knn vs brute force"); sd += b.d[i]; }
        }
        printf("k=%2d: avg distance computations %ld (brute %d), sum of distances %ld\n", ks[ki], tc / 100, NP, sd);
    }
    destroy(root);
    return 0;
}
