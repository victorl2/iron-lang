/*
 * title: Reconstructing trees from traversal sequences
 * topic: data_structures
 * covers: binary tree reconstruction, preorder+inorder, postorder+inorder, level-order+inorder, preorder+postorder full trees, BST from preorder/postorder/level-order alone, bounds recursion
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
    int v;
    struct N *l, *r;
} N;

static N *mk(int v) {
    N *n = xmalloc(sizeof *n);
    n->v = v; n->l = n->r = NULL;
    return n;
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }
static int same(N *a, N *b) {
    if (!a || !b) return a == b;
    return a->v == b->v && same(a->l, b->l) && same(a->r, b->r);
}
static int cnt(N *t) { return t ? 1 + cnt(t->l) + cnt(t->r) : 0; }
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static int wn;
static void w_pre(N *t, int *o) { if (!t) return; o[wn++] = t->v; w_pre(t->l, o); w_pre(t->r, o); }
static void w_in(N *t, int *o) { if (!t) return; w_in(t->l, o); o[wn++] = t->v; w_in(t->r, o); }
static void w_post(N *t, int *o) { if (!t) return; w_post(t->l, o); w_post(t->r, o); o[wn++] = t->v; }
static int w_level(N *t, int *o) {
    N *q[MAXN]; int h = 0, tl = 0, k = 0;
    if (t) q[tl++] = t;
    while (h < tl) {
        N *x = q[h++];
        o[k++] = x->v;
        if (x->l) q[tl++] = x->l;
        if (x->r) q[tl++] = x->r;
    }
    return k;
}

static int pos_of[MAXN * 2];
/* 1. preorder + inorder */
static N *from_pre_in(const int *pre, int *pi, const int *in, int lo, int hi) {
    if (lo > hi) return NULL;
    N *t = mk(pre[(*pi)++]);
    int m = pos_of[t->v];
    t->l = from_pre_in(pre, pi, in, lo, m - 1);
    t->r = from_pre_in(pre, pi, in, m + 1, hi);
    return t;
}
/* 2. postorder + inorder (consume postorder from the back) */
static N *from_post_in(const int *post, int *pi, const int *in, int lo, int hi) {
    if (lo > hi) return NULL;
    N *t = mk(post[(*pi)--]);
    int m = pos_of[t->v];
    t->r = from_post_in(post, pi, in, m + 1, hi);
    t->l = from_post_in(post, pi, in, lo, m - 1);
    return t;
}
/* 3. level order + inorder: split the level sequence by membership in the inorder range */
static N *from_level_in(const int *lv, int nl, const int *in, int lo, int hi) {
    if (lo > hi || nl == 0) return NULL;
    N *t = mk(lv[0]);
    int m = pos_of[t->v];
    int left[MAXN], right[MAXN], nleft = 0, nright = 0;
    for (int i = 1; i < nl; i++) {
        if (pos_of[lv[i]] < m) left[nleft++] = lv[i];
        else right[nright++] = lv[i];
    }
    t->l = from_level_in(left, nleft, in, lo, m - 1);
    t->r = from_level_in(right, nright, in, m + 1, hi);
    return t;
}
/* 4. full binary tree from preorder + postorder */
static N *from_pre_post(const int *pre, int *pi, const int *post, int lo, int hi, int n) {
    if (*pi >= n || lo > hi) return NULL;
    N *t = mk(pre[(*pi)++]);
    if (lo == hi || *pi >= n) return t;
    int i = lo;
    while (post[i] != pre[*pi]) i++;
    if (i <= hi) {
        t->l = from_pre_post(pre, pi, post, lo, i, n);
        t->r = from_pre_post(pre, pi, post, i + 1, hi - 1, n);
    }
    return t;
}
/* 5. BST from preorder alone: upper-bound recursion, O(n) */
static N *bst_from_pre(const int *pre, int n, int *i, long bound) {
    if (*i >= n || pre[*i] > bound) return NULL;
    N *t = mk(pre[(*i)++]);
    t->l = bst_from_pre(pre, n, i, t->v);
    t->r = bst_from_pre(pre, n, i, bound);
    return t;
}
/* 6. BST from postorder alone: walk backwards, right subtree first */
static N *bst_from_post(const int *post, int *i, long lo) {
    if (*i < 0 || post[*i] < lo) return NULL;
    N *t = mk(post[(*i)--]);
    t->r = bst_from_post(post, i, t->v);
    t->l = bst_from_post(post, i, lo);
    return t;
}
/* 7. BST from level order alone: queue of (node, lo, hi) ranges */
static N *bst_from_level(const int *lv, int n) {
    if (!n) return NULL;
    N *nodes[MAXN]; long lo[MAXN], hi[MAXN];
    int h = 0, tl = 0, i = 1;
    N *root = mk(lv[0]);
    nodes[tl] = root; lo[tl] = -1000000; hi[tl] = 1000000; tl++;
    while (h < tl && i < n) {
        N *x = nodes[h]; long l = lo[h], u = hi[h]; h++;
        if (i < n && lv[i] > l && lv[i] < x->v) {
            x->l = mk(lv[i++]);
            nodes[tl] = x->l; lo[tl] = l; hi[tl] = x->v; tl++;
        }
        if (i < n && lv[i] > x->v && lv[i] < u) {
            x->r = mk(lv[i++]);
            nodes[tl] = x->r; lo[tl] = x->v; hi[tl] = u; tl++;
        }
    }
    return root;
}

static N *random_shape(int n, const int *vals, int *next) {
    if (n == 0) return NULL;
    int left = (int)(rnd() % (unsigned)n);
    N *t = mk(0);
    t->l = random_shape(left, vals, next);
    t->v = vals[(*next)++];
    t->r = random_shape(n - 1 - left, vals, next);
    return t;
}
/* random full binary tree (0 or 2 children) with unique values */
static N *random_full(int leaves, int *counter) {
    N *t = mk((*counter)++);
    if (leaves > 1) {
        int a = 1 + (int)(rnd() % (unsigned)(leaves - 1));
        t->l = random_full(a, counter);
        t->r = random_full(leaves - a, counter);
    }
    return t;
}
static N *bst_insert(N *t, int v) {
    if (!t) return mk(v);
    if (v < t->v) t->l = bst_insert(t->l, v);
    else t->r = bst_insert(t->r, v);
    return t;
}
static unsigned hash(const int *a, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (unsigned)a[i]; h *= 16777619u; }
    return h;
}

int main(void) {
    static int pre[MAXN], in[MAXN], post[MAXN], lv[MAXN], vals[MAXN];
    int ok_general = 0, ok_bst = 0, ok_full = 0;
    for (int round = 0; round < 60; round++) {
        int n = 1 + (int)(rnd() % 120);
        for (int i = 0; i < n; i++) vals[i] = i * 3 + 1;
        for (int i = n - 1; i > 0; i--) {                 /* shuffle */
            int j = (int)(rnd() % (unsigned)(i + 1)), t = vals[i];
            vals[i] = vals[j]; vals[j] = t;
        }
        /* general binary tree with distinct values */
        int next = 0;
        N *t = random_shape(n, vals, &next);
        wn = 0; w_pre(t, pre);
        wn = 0; w_in(t, in);
        wn = 0; w_post(t, post);
        w_level(t, lv);
        for (int i = 0; i < n; i++) pos_of[in[i]] = i;
        int pi = 0;
        N *a = from_pre_in(pre, &pi, in, 0, n - 1);
        check(same(a, t), "pre+in");
        pi = n - 1;
        N *b = from_post_in(post, &pi, in, 0, n - 1);
        check(same(b, t), "post+in");
        N *c = from_level_in(lv, n, in, 0, n - 1);
        check(same(c, t), "level+in");
        ok_general += 3;
        freet(a); freet(b); freet(c); freet(t);
        /* BST from insertion order: needs only one traversal */
        N *bt = NULL;
        for (int i = 0; i < n; i++) bt = bst_insert(bt, vals[i]);
        wn = 0; w_pre(bt, pre);
        wn = 0; w_post(bt, post);
        int nl = w_level(bt, lv);
        pi = 0;
        N *d = bst_from_pre(pre, n, &pi, 1000000);
        check(same(d, bt) && pi == n, "bst from preorder");
        pi = n - 1;
        N *e = bst_from_post(post, &pi, -1000000);
        check(same(e, bt) && pi == -1, "bst from postorder");
        N *f = bst_from_level(lv, nl);
        check(same(f, bt), "bst from level order");
        ok_bst += 3;
        if (round < 3) {
            printf("round %d: n=%d height=%d preorder hash=%u level hash=%u\n", round, n, height(bt), hash(pre, n), hash(lv, nl));
        }
        freet(d); freet(e); freet(f); freet(bt);
        /* full binary tree: preorder + postorder is enough */
        int counter = 0;
        N *ft = random_full(1 + (int)(rnd() % 40), &counter);
        int fn = cnt(ft);
        wn = 0; w_pre(ft, pre);
        wn = 0; w_post(ft, post);
        pi = 0;
        N *g = from_pre_post(pre, &pi, post, 0, fn - 1, fn);
        check(same(g, ft), "full tree from pre+post");
        ok_full++;
        freet(g); freet(ft);
    }
    printf("reconstructed: %d general, %d BST, %d full-tree cases\n", ok_general, ok_bst, ok_full);
    /* the classic example */
    static const int p0[6] = {3, 9, 20, 15, 7, 0}, i0[5] = {9, 3, 15, 20, 7};
    for (int i = 0; i < 5; i++) { pos_of[i0[i]] = i; pre[i] = p0[i]; in[i] = i0[i]; }
    int pi = 0;
    N *x = from_pre_in(pre, &pi, in, 0, 4);
    wn = 0; w_post(x, post);
    printf("example postorder:");
    for (int i = 0; i < 5; i++) printf(" %d", post[i]);
    printf(" height=%d\n", height(x));
    freet(x);
    /* ambiguity: pre+post cannot separate a lone left child from a lone right child */
    N *L = mk(1); L->l = mk(2);
    N *R = mk(1); R->r = mk(2);
    int pl[2], ql[2], pr[2], qr[2];
    wn = 0; w_pre(L, pl); wn = 0; w_post(L, ql);
    wn = 0; w_pre(R, pr); wn = 0; w_post(R, qr);
    check(!memcmp(pl, pr, sizeof pl) && !memcmp(ql, qr, sizeof ql) && !same(L, R), "pre+post ambiguity");
    printf("left-only and right-only chains share preorder and postorder: yes\n");
    freet(L); freet(R);
    return 0;
}
