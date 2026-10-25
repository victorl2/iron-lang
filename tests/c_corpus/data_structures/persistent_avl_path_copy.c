/*
 * title: Persistent AVL tree with path copying and version history
 * topic: data_structures
 * covers: persistent data structure, path copying, structural sharing, immutable AVL nodes, version array, snapshot model per version, node allocation counts, rollback to old versions
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 2048

static unsigned long long rs = 88172645463325252ULL;
unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}
void check(int c, const char *w) {
    if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); }
}
void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) exit(2);
    return p;
}
/* reference model: sorted array of unique keys */
int mod[MAXN], mn;
int mfind(int k) {
    int lo = 0, hi = mn;
    while (lo < hi) { int m = (lo + hi) / 2; if (mod[m] < k) lo = m + 1; else hi = m; }
    return lo;
}
int mhas(int k) { int p = mfind(k); return p < mn && mod[p] == k; }
int minsert(int k) {
    int p = mfind(k);
    if (p < mn && mod[p] == k) return 0;
    memmove(mod + p + 1, mod + p, (size_t)(mn - p) * sizeof(int));
    mod[p] = k; mn++;
    return 1;
}
int mdelete(int k) {
    int p = mfind(k);
    if (p >= mn || mod[p] != k) return 0;
    memmove(mod + p, mod + p + 1, (size_t)(mn - p - 1) * sizeof(int));
    mn--;
    return 1;
}
int no, outk[MAXN];

typedef struct N {
    int k, h;
    const struct N *l, *r;      /* nodes are immutable once published */
} N;

static N *arena[MAXN * 40];
static int an;
static long allocs;
static int ht(const N *t) { return t ? t->h : 0; }
static const N *mkn(int k, const N *l, const N *r) {
    N *n = xmalloc(sizeof *n);
    n->k = k; n->l = l; n->r = r;
    int a = ht(l), b = ht(r);
    n->h = 1 + (a > b ? a : b);
    arena[an++] = n;
    allocs++;
    return n;
}
static const N *bal(int k, const N *l, const N *r) {
    int bf = ht(l) - ht(r);
    if (bf > 1) {
        if (ht(l->l) >= ht(l->r)) return mkn(l->k, l->l, mkn(k, l->r, r));
        return mkn(l->r->k, mkn(l->k, l->l, l->r->l), mkn(k, l->r->r, r));
    }
    if (bf < -1) {
        if (ht(r->r) >= ht(r->l)) return mkn(r->k, mkn(k, l, r->l), r->r);
        return mkn(r->l->k, mkn(k, l, r->l->l), mkn(r->k, r->l->r, r->r));
    }
    return mkn(k, l, r);
}
static const N *ins(const N *t, int k, int *added) {
    if (!t) { *added = 1; return mkn(k, NULL, NULL); }
    if (k < t->k) {
        const N *l = ins(t->l, k, added);
        return *added ? bal(t->k, l, t->r) : t;
    }
    if (k > t->k) {
        const N *r = ins(t->r, k, added);
        return *added ? bal(t->k, t->l, r) : t;
    }
    return t;
}
static const N *del_min(const N *t, int *minkey) {
    if (!t->l) { *minkey = t->k; return t->r; }
    const N *l = del_min(t->l, minkey);
    return bal(t->k, l, t->r);
}
static const N *del(const N *t, int k, int *removed) {
    if (!t) return NULL;
    if (k < t->k) {
        const N *l = del(t->l, k, removed);
        return *removed ? bal(t->k, l, t->r) : t;
    }
    if (k > t->k) {
        const N *r = del(t->r, k, removed);
        return *removed ? bal(t->k, t->l, r) : t;
    }
    *removed = 1;
    if (!t->l) return t->r;
    if (!t->r) return t->l;
    int m;
    const N *r = del_min(t->r, &m);
    return bal(m, t->l, r);
}
static int verify(const N *t, long lo, long hi) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "order");
    int c = 1 + verify(t->l, lo, t->k) + verify(t->r, t->k, hi);
    int a = ht(t->l), b = ht(t->r);
    check(t->h == 1 + (a > b ? a : b) && a - b >= -1 && a - b <= 1, "avl");
    return c;
}
static void inorder(const N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static int has(const N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t != NULL;
}
/* count distinct nodes reachable from a set of roots */
static int reach_mark[MAXN * 40];
static int idx_of(const N *n) {
    for (int i = an - 1; i >= 0; i--) if (arena[i] == n) return i;
    return -1;
}
static int mark(const N *t) {
    if (!t) return 0;
    int i = idx_of(t);
    if (reach_mark[i]) return 0;
    reach_mark[i] = 1;
    return 1 + mark(t->l) + mark(t->r);
}

#define VERS 160
static const N *ver[VERS + 1];
static int snap[VERS + 1][130];
static int snapn[VERS + 1];

int main(void) {
    int nv = 0;
    ver[0] = NULL; snapn[0] = 0; nv = 1;
    mn = 0;
    long max_per_op = 0, total_ops = 0;
    for (int step = 1; step <= VERS; step++) {
        int k = (int)(rnd() % 120);
        long before = allocs;
        const N *t = ver[nv - 1];
        int flag = 0;
        if (rnd() % 3) {
            const N *nt = ins(t, k, &flag);
            check(flag == minsert(k), "insert result");
            t = nt;
        } else {
            const N *nt = del(t, k, &flag);
            check(flag == mdelete(k), "delete result");
            t = nt;
        }
        long used = allocs - before;
        if (used > max_per_op) max_per_op = used;
        total_ops++;
        ver[nv] = t;
        memcpy(snap[nv], mod, sizeof(int) * (size_t)mn);
        snapn[nv] = mn;
        check(verify(t, -1, 100000) == mn, "new version valid");
        nv++;
    }
    /* every historical version must still equal its snapshot */
    for (int v = 0; v < nv; v++) {
        check(verify(ver[v], -1, 100000) == snapn[v], "old version size");
        no = 0; inorder(ver[v]);
        check(no == snapn[v] && !memcmp(outk, snap[v], sizeof(int) * (size_t)no), "old version contents");
    }
    printf("versions=%d, final size=%d, height=%d\n", nv, snapn[nv - 1], ht(ver[nv - 1]));
    printf("operations=%ld, total nodes allocated=%ld, max per operation=%ld\n", total_ops, allocs, max_per_op);
    /* sharing: distinct nodes reachable from all versions vs sum of sizes */
    long sum_sizes = 0;
    for (int v = 0; v < nv; v++) sum_sizes += snapn[v];
    int distinct = 0;
    for (int v = 0; v < nv; v++) distinct += mark(ver[v]);
    printf("sum of version sizes=%ld, distinct nodes reachable=%d\n", sum_sizes, distinct);
    check(distinct <= allocs, "reachable within allocated");
    /* time travel queries */
    int hits = 0;
    for (int q = 0; q < 2000; q++) {
        int v = (int)(rnd() % (unsigned)nv), k = (int)(rnd() % 120);
        int want = 0;
        for (int i = 0; i < snapn[v]; i++) if (snap[v][i] == k) want = 1;
        check(has(ver[v], k) == want, "time-travel lookup");
        hits += want;
    }
    printf("2000 lookups across versions: %d hits\n", hits);
    /* branching: fork an old version, the other branch is unaffected */
    int f = nv / 2, added = 0;
    const N *branch = ver[f];
    for (int k = 200; k < 230; k++) branch = ins(branch, k, &added);
    check(verify(branch, -1, 100000) == snapn[f] + 30, "branch size");
    no = 0; inorder(ver[f]);
    check(no == snapn[f] && !memcmp(outk, snap[f], sizeof(int) * (size_t)no), "forked version unchanged");
    printf("fork of version %d: branch size %d, original still %d\n", f, snapn[f] + 30, snapn[f]);
    for (int i = 0; i < an; i++) free(arena[i]);
    return 0;
}
