/*
 * title: Bottom-up splay tree with parent pointers
 * topic: data_structures
 * covers: splay tree, bottom-up splaying, zig, zig-zig, zig-zag, parent pointers, join delete, rotation counts, sorted-array model
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 2048

static unsigned long long rs = 88172645463325252ULL;
static inline unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}
static inline void check(int c, const char *w) {
    if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); }
}
static inline void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) exit(2);
    return p;
}
/* reference model: sorted array of unique keys */
static int mod[MAXN], mn;
static inline int mfind(int k) {
    int lo = 0, hi = mn;
    while (lo < hi) { int m = (lo + hi) / 2; if (mod[m] < k) lo = m + 1; else hi = m; }
    return lo;
}
static inline int mhas(int k) { int p = mfind(k); return p < mn && mod[p] == k; }
static inline int minsert(int k) {
    int p = mfind(k);
    if (p < mn && mod[p] == k) return 0;
    memmove(mod + p + 1, mod + p, (size_t)(mn - p) * sizeof(int));
    mod[p] = k; mn++;
    return 1;
}
static inline int mdelete(int k) {
    int p = mfind(k);
    if (p >= mn || mod[p] != k) return 0;
    memmove(mod + p, mod + p + 1, (size_t)(mn - p - 1) * sizeof(int));
    mn--;
    return 1;
}
static int no, outk[MAXN];

typedef struct N {
    int k;
    struct N *l, *r, *p;
} N;

typedef struct { N *root; long zig, zigzig, zigzag; } Tree;

static void rotate(Tree *T, N *x) {
    N *p = x->p, *g = p->p;
    if (x == p->l) {
        p->l = x->r;
        if (x->r) x->r->p = p;
        x->r = p;
    } else {
        p->r = x->l;
        if (x->l) x->l->p = p;
        x->l = p;
    }
    p->p = x;
    x->p = g;
    if (!g) T->root = x;
    else if (g->l == p) g->l = x;
    else g->r = x;
}
static void splay(Tree *T, N *x) {
    while (x->p) {
        N *p = x->p, *g = p->p;
        if (!g) { rotate(T, x); T->zig++; }
        else if ((g->l == p) == (p->l == x)) { rotate(T, p); rotate(T, x); T->zigzig++; }
        else { rotate(T, x); rotate(T, x); T->zigzag++; }
    }
}
static N *find(Tree *T, int k) {
    N *x = T->root, *last = NULL;
    while (x) {
        last = x;
        if (k == x->k) break;
        x = k < x->k ? x->l : x->r;
    }
    if (last) splay(T, last);
    return x;
}
static int insert(Tree *T, int k) {
    N *y = NULL, *x = T->root;
    while (x) {
        y = x;
        if (k == x->k) { splay(T, x); return 0; }
        x = k < x->k ? x->l : x->r;
    }
    N *n = xmalloc(sizeof *n);
    n->k = k; n->l = n->r = NULL; n->p = y;
    if (!y) T->root = n;
    else if (k < y->k) y->l = n;
    else y->r = n;
    splay(T, n);
    return 1;
}
static int delete(Tree *T, int k) {
    N *x = find(T, k);
    if (!x) return 0;
    N *l = x->l, *r = x->r;
    free(x);
    if (l) l->p = NULL;
    if (r) r->p = NULL;
    if (!l) { T->root = r; return 1; }
    T->root = l;
    N *m = l;
    while (m->r) m = m->r;
    splay(T, m);
    m->r = r;
    if (r) r->p = m;
    return 1;
}
static int verify(N *t, N *par, long lo, long hi) {
    if (!t) return 0;
    check(t->p == par, "parent link");
    check(t->k > lo && t->k < hi, "order");
    return 1 + verify(t->l, t, lo, t->k) + verify(t->r, t, t->k, hi);
}
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    Tree T = {NULL, 0, 0, 0};
    int ins = 0, del = 0, hits = 0, maxh = 0;
    for (int i = 0; i < 250; i++) { insert(&T, i); minsert(i); }
    check(verify(T.root, NULL, -1, 100000) == mn, "asc size");
    printf("ascending 250: height=%d root=%d zig=%ld zigzig=%ld zigzag=%ld\n",
           height(T.root), T.root->k, T.zig, T.zigzig, T.zigzag);
    /* accessing the deepest node reshapes the tree, halving path depth */
    N *deep = T.root;
    while (deep->l || deep->r) deep = deep->l ? deep->l : deep->r;
    int dk = deep->k;
    int hb = height(T.root);
    find(&T, dk);
    printf("splayed deepest key %d: height %d -> %d\n", dk, hb, height(T.root));
    check(T.root->k == dk, "deepest at root");
    for (int op = 0; op < 4000; op++) {
        int k = (int)(rnd() % 500);
        unsigned c = rnd() % 3;
        if (c == 0) { int a = insert(&T, k); check(a == minsert(k), "ins"); ins += a; }
        else if (c == 1) { int a = delete(&T, k); check(a == mdelete(k), "del"); del += a; }
        else {
            N *x = find(&T, k);
            check((x != NULL) == mhas(k), "find");
            if (x) { check(T.root == x, "root after find"); hits++; }
        }
        check(verify(T.root, NULL, -1, 100000) == mn, "size and links");
        if (op % 20 == 0) {
            no = 0; inorder(T.root);
            check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
            int h = height(T.root);
            if (h > maxh) maxh = h;
        }
    }
    no = 0; inorder(T.root);
    check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "final inorder");
    printf("random: inserts=%d deletes=%d hits=%d size=%d sampled maxheight=%d\n", ins, del, hits, mn, maxh);
    printf("rotation kinds zig=%ld zigzig=%ld zigzag=%ld\n", T.zig, T.zigzig, T.zigzag);
    while (mn) {
        int k = mod[(unsigned)mn - 1 - (unsigned)mn / 4];
        check(delete(&T, k), "drain"); mdelete(k);
        check(verify(T.root, NULL, -1, 100000) == mn, "drain size");
    }
    check(T.root == NULL, "empty");
    printf("drained\n");
    freet(T.root);
    return 0;
}
