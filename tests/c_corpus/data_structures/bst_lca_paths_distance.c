/*
 * title: BST lowest common ancestor, paths and distances
 * topic: data_structures
 * covers: binary search tree, lowest common ancestor by comparison, root-to-node paths, node distance, ancestor test by key range, k-th ancestor, diameter, sum of pairwise distances by edge contribution, brute-force cross-check
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

typedef struct N {
    int k, depth, size, lo, hi;      /* lo/hi: smallest and largest key in the subtree */
    struct N *l, *r, *p;
} N;

static N *nodes[MAXN];
static int nn;

static N *insert(N *root, int k) {
    N *par = NULL, *x = root;
    while (x) {
        if (k == x->k) return root;
        par = x;
        x = k < x->k ? x->l : x->r;
    }
    x = xmalloc(sizeof *x);
    x->k = k; x->l = x->r = NULL; x->p = par;
    if (!par) return x;
    if (k < par->k) par->l = x; else par->r = x;
    return root;
}
static void annotate(N *t, int depth) {
    if (!t) return;
    t->depth = depth;
    annotate(t->l, depth + 1);
    annotate(t->r, depth + 1);
    t->size = 1 + (t->l ? t->l->size : 0) + (t->r ? t->r->size : 0);
    t->lo = t->l ? t->l->lo : t->k;
    t->hi = t->r ? t->r->hi : t->k;
    nodes[nn++] = t;
}
static N *find(N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t;
}
/* LCA using only key comparisons, both keys present */
static N *lca_keys(N *root, int a, int b) {
    N *x = root;
    for (;;) {
        if (a < x->k && b < x->k) x = x->l;
        else if (a > x->k && b > x->k) x = x->r;
        else return x;
    }
}
/* LCA by climbing parent pointers with depths */
static N *lca_climb(N *a, N *b) {
    while (a->depth > b->depth) a = a->p;
    while (b->depth > a->depth) b = b->p;
    while (a != b) { a = a->p; b = b->p; }
    return a;
}
static int path_to(N *root, int k, int *out) {
    int n = 0;
    N *x = root;
    while (x) {
        out[n++] = x->k;
        if (x->k == k) return n;
        x = k < x->k ? x->l : x->r;
    }
    return -1;
}
static int distance_keys(N *root, int a, int b) {
    N *l = lca_keys(root, a, b);
    return find(root, a)->depth + find(root, b)->depth - 2 * l->depth;
}
static int is_ancestor(N *a, N *b) { return b->k >= a->lo && b->k <= a->hi; }   /* a is an ancestor-or-self of b */
static int height(N *t) {
    if (!t) return 0;
    int x = height(t->l), y = height(t->r);
    return 1 + (x > y ? x : y);
}
/* diameter in edges by the standard recursion */
static int diam;
static int diameter_rec(N *t) {
    if (!t) return -1;
    int a = diameter_rec(t->l), b = diameter_rec(t->r);
    if (a + b + 2 > diam) diam = a + b + 2;
    return 1 + (a > b ? a : b);
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    N *root = NULL;
    for (int i = 0; i < 150; i++) root = insert(root, (int)(rnd() % 1000));
    nn = 0;
    annotate(root, 0);
    printf("nodes=%d height=%d root=%d\n", nn, height(root), root->k);
    long dsum_pairs = 0, dsum_edges = 0, checked = 0;
    int maxd = 0, pa = 0, pb = 0;
    for (int i = 0; i < nn; i++)
        for (int j = i + 1; j < nn; j++) {
            N *a = nodes[i], *b = nodes[j];
            N *l1 = lca_keys(root, a->k, b->k), *l2 = lca_climb(a, b);
            check(l1 == l2, "lca methods agree");
            int d = a->depth + b->depth - 2 * l1->depth;
            check(distance_keys(root, a->k, b->k) == d, "distance");
            /* the LCA is an ancestor of both, and no child of it is */
            check(is_ancestor(l1, a) && is_ancestor(l1, b), "lca is common ancestor");
            if (l1 != a && l1 != b) {
                N *c1 = a->k < l1->k ? l1->l : l1->r;
                check(!(is_ancestor(c1, a) && is_ancestor(c1, b)), "lca is lowest");
            }
            /* path length equals distance through the LCA */
            int pa_[MAXN], pb_[MAXN];
            int la = path_to(root, a->k, pa_), lb = path_to(root, b->k, pb_);
            check(la == a->depth + 1 && lb == b->depth + 1, "path lengths");
            int common = 0;
            while (common < la && common < lb && pa_[common] == pb_[common]) common++;
            check(pa_[common - 1] == l1->k, "last common path node is the lca");
            check((la - common) + (lb - common) == d, "distance from paths");
            dsum_pairs += d;
            if (d > maxd) { maxd = d; pa = a->k; pb = b->k; }
            checked++;
        }
    /* sum of pairwise distances = sum over edges of size * (n - size) */
    for (int i = 0; i < nn; i++)
        if (nodes[i]->p) dsum_edges += (long)nodes[i]->size * (nn - nodes[i]->size);
    check(dsum_pairs == dsum_edges, "edge contribution formula");
    diam = 0;
    diameter_rec(root);
    check(diam == maxd, "diameter equals the largest pair distance");
    printf("pairs checked=%ld, sum of distances=%ld, diameter=%d between %d and %d\n", checked, dsum_pairs, diam, pa, pb);
    /* k-th ancestor via parent chain against the root path */
    long anc = 0;
    for (int i = 0; i < nn; i += 3) {
        int path[MAXN];
        int len = path_to(root, nodes[i]->k, path);
        N *x = nodes[i];
        for (int up = 0; up < len; up++) {
            check(x->k == path[len - 1 - up], "k-th ancestor");
            anc += x->k;
            x = x->p;
        }
        check(x == NULL, "climbed past the root");
    }
    printf("ancestor checksum=%ld\n", anc);
    freet(root);
    return 0;
}
