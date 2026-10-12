/*
 * title: Implicit k-ary tree versus pointer tree
 * topic: data_structures
 * covers: complete k-ary tree, index arithmetic, array layout versus linked nodes, parent chains, lca, subtree sums, heapify equivalence
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PN {
    int val, idx;
    struct PN *parent, **kids;
    int nkids;
} PN;

static unsigned long long rs = 0x4A5A7E11ULL * 911;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* implicit layout: root 0, children of i are k*i+1 .. k*i+k, parent (i-1)/k */
static int parent_of(int i, int k) { return (i - 1) / k; }
static int first_child(int i, int k) { return k * i + 1; }
static int level_of(int i, int k) { int l = 0; while (i > 0) { i = parent_of(i, k); l++; } return l; }

static PN **all;
static PN *build_pointer(const int *a, int n, int k) {
    all = calloc((size_t)n, sizeof *all);
    for (int i = 0; i < n; i++) { all[i] = calloc(1, sizeof(PN)); all[i]->val = a[i]; all[i]->idx = i; }
    for (int i = 0; i < n; i++) {
        int f = first_child(i, k), c = 0;
        while (c < k && f + c < n) c++;
        all[i]->nkids = c;
        all[i]->kids = c ? calloc((size_t)c, sizeof(PN *)) : NULL;
        for (int j = 0; j < c; j++) { all[i]->kids[j] = all[f + j]; all[f + j]->parent = all[i]; }
    }
    return all[0];
}
static void free_pointer(int n) {
    for (int i = 0; i < n; i++) { free(all[i]->kids); free(all[i]); }
    free(all); all = NULL;
}
static long subtree_sum_ptr(const PN *p) { long s = p->val; for (int i = 0; i < p->nkids; i++) s += subtree_sum_ptr(p->kids[i]); return s; }
static long subtree_sum_imp(const int *a, int n, int k, int i) {
    if (i >= n) return 0;
    long s = a[i];
    for (int j = 0; j < k; j++) s += subtree_sum_imp(a, n, k, first_child(i, k) + j);
    return s;
}
static int height_ptr(const PN *p) { int h = 0; for (int i = 0; i < p->nkids; i++) { int c = height_ptr(p->kids[i]); if (c > h) h = c; } return h + 1; }
static int height_imp(int n, int k) { return n ? level_of(n - 1, k) + 1 : 0; }

static int lca_imp(int a, int b, int k) {
    int la = level_of(a, k), lb = level_of(b, k);
    while (la > lb) { a = parent_of(a, k); la--; }
    while (lb > la) { b = parent_of(b, k); lb--; }
    while (a != b) { a = parent_of(a, k); b = parent_of(b, k); }
    return a;
}
static const PN *lca_ptr(const PN *a, const PN *b) {
    int la = 0, lb = 0;
    for (const PN *x = a; x->parent; x = x->parent) la++;
    for (const PN *x = b; x->parent; x = x->parent) lb++;
    while (la > lb) { a = a->parent; la--; }
    while (lb > la) { b = b->parent; lb--; }
    while (a != b) { a = a->parent; b = b->parent; }
    return a;
}
/* min-heap via sift-down on the implicit array */
static long sift_swaps_imp;
static void sift_down_imp(int *a, int n, int k, int i) {
    for (;;) {
        int f = first_child(i, k), m = i;
        for (int j = 0; j < k && f + j < n; j++) if (a[f + j] < a[m]) m = f + j;
        if (m == i) return;
        int t = a[i]; a[i] = a[m]; a[m] = t; sift_swaps_imp++; i = m;
    }
}
/* the same algorithm walking pointers */
static long sift_swaps_ptr;
static void sift_down_ptr(PN *p) {
    for (;;) {
        PN *m = p;
        for (int j = 0; j < p->nkids; j++) if (p->kids[j]->val < m->val) m = p->kids[j];
        if (m == p) return;
        int t = p->val; p->val = m->val; m->val = t; sift_swaps_ptr++; p = m;
    }
}
static void level_order_ptr(const PN *root, int *out) {
    const PN **q = malloc(sizeof(PN *) * 4096); int h = 0, t = 0, n = 0;
    q[t++] = root;
    while (h < t) { const PN *p = q[h++]; out[n++] = p->val; for (int i = 0; i < p->nkids; i++) q[t++] = p->kids[i]; }
    free(q);
}

int main(void) {
    int ks[] = { 2, 3, 4, 8 };
    for (int ki = 0; ki < 4; ki++) {
        int k = ks[ki], n = 1000 + ki * 37;
        static int a[2000], b[2000], out[2000];
        for (int i = 0; i < n; i++) a[i] = (int)(rnd() % 100000);
        memcpy(b, a, sizeof(int) * (size_t)n);
        PN *root = build_pointer(a, n, k);
        /* structure agreement */
        long links = 0;
        for (int i = 0; i < n; i++) {
            check((i == 0) == (all[i]->parent == NULL), "root has no parent");
            if (i) check(all[i]->parent->idx == parent_of(i, k), "parent index arithmetic");
            links += all[i]->nkids + 1;
        }
        check(height_ptr(root) == height_imp(n, k), "height");
        check(subtree_sum_ptr(root) == subtree_sum_imp(a, n, k, 0), "whole tree sum");
        long chk = 0;
        for (int t = 0; t < 200; t++) {
            int i = (int)(rnd() % (unsigned)n);
            check(subtree_sum_ptr(all[i]) == subtree_sum_imp(a, n, k, i), "subtree sum");
            int j = (int)(rnd() % (unsigned)n);
            check(lca_ptr(all[i], all[j])->idx == lca_imp(i, j, k), "lca");
            chk += lca_imp(i, j, k);
        }
        /* heapify both representations from the same data; results must be identical arrays */
        sift_swaps_imp = sift_swaps_ptr = 0;
        for (int i = n / k; i >= 0; i--) if (first_child(i, k) < n) sift_down_imp(b, n, k, i);
        for (int i = n - 1; i >= 0; i--) if (all[i]->nkids) sift_down_ptr(all[i]);
        for (int i = 0; i < n; i++) check(b[i] == all[i]->val, "identical heap layouts");
        check(sift_swaps_imp == sift_swaps_ptr, "same number of swaps");
        for (int i = 1; i < n; i++) check(b[parent_of(i, k)] <= b[i], "heap property");
        level_order_ptr(root, out);
        check(memcmp(out, b, sizeof(int) * (size_t)n) == 0, "breadth-first walk equals array order");
        int internal = 0; for (int i = 0; i < n; i++) if (first_child(i, k) < n) internal++;
        printf("k=%d n=%d: height %d, internal nodes %d, leaves %d, swaps %ld, link fields: implicit 0 vs pointer %ld, lca checksum %ld\n",
               k, n, height_imp(n, k), internal, n - internal, sift_swaps_imp, links, chk);
        free_pointer(n);
    }
    /* level widths of complete trees */
    printf("level sizes k=3 n=100:");
    { int cnt[10] = {0}; for (int i = 0; i < 100; i++) cnt[level_of(i, 3)]++; for (int l = 0; l < 6; l++) printf(" %d", cnt[l]); printf("\n"); }
    return 0;
}
