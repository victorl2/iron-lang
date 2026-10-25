/*
 * title: Morris traversals with O(1) extra space
 * topic: data_structures
 * covers: Morris traversal, temporary threads, inorder, preorder, postorder with reversed right-chains, tree restoration check, kth smallest, BST validation, random trees
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
static int cnt(N *t) { return t ? 1 + cnt(t->l) + cnt(t->r) : 0; }

static int rn, ro[MAXN];
static void pre_r(N *t) { if (!t) return; ro[rn++] = t->v; pre_r(t->l); pre_r(t->r); }
static void in_r(N *t) { if (!t) return; in_r(t->l); ro[rn++] = t->v; in_r(t->r); }
static void post_r(N *t) { if (!t) return; post_r(t->l); post_r(t->r); ro[rn++] = t->v; }

static long links_made;
static int morris_in(N *t, int *o) {
    int n = 0;
    while (t) {
        if (!t->l) { o[n++] = t->v; t = t->r; }
        else {
            N *p = t->l;
            while (p->r && p->r != t) p = p->r;
            if (!p->r) { p->r = t; links_made++; t = t->l; }
            else { p->r = NULL; o[n++] = t->v; t = t->r; }
        }
    }
    return n;
}
static int morris_pre(N *t, int *o) {
    int n = 0;
    while (t) {
        if (!t->l) { o[n++] = t->v; t = t->r; }
        else {
            N *p = t->l;
            while (p->r && p->r != t) p = p->r;
            if (!p->r) { o[n++] = t->v; p->r = t; links_made++; t = t->l; }
            else { p->r = NULL; t = t->r; }
        }
    }
    return n;
}
static void reverse_chain(N *a, N *b) {
    if (a == b) return;
    N *x = a, *y = a->r, *z;
    while (x != b) { z = y->r; y->r = x; x = y; y = z; }
}
static int emit_chain(N *from, N *to, int *o, int n) {
    N *saved = to->r;
    reverse_chain(from, to);
    N *p = to;
    for (;;) {
        o[n++] = p->v;
        if (p == from) break;
        p = p->r;
    }
    reverse_chain(to, from);
    to->r = saved;
    return n;
}
static int morris_post(N *root, int *o) {
    N dummy, *t = &dummy;
    int n = 0;
    dummy.v = 0; dummy.l = root; dummy.r = NULL;
    while (t) {
        if (!t->l) t = t->r;
        else {
            N *p = t->l;
            while (p->r && p->r != t) p = p->r;
            if (!p->r) { p->r = t; links_made++; t = t->l; }
            else { p->r = NULL; n = emit_chain(t->l, p, o, n); t = t->r; }
        }
    }
    return n;
}
/* O(1)-space applications, all built on the inorder walk */
static int kth_smallest(N *t, int k) {
    int i = 0, ans = -1;
    while (t) {
        if (!t->l) { if (++i == k) ans = t->v; t = t->r; }
        else {
            N *p = t->l;
            while (p->r && p->r != t) p = p->r;
            if (!p->r) { p->r = t; t = t->l; }
            else { p->r = NULL; if (++i == k) ans = t->v; t = t->r; }
        }
    }
    return ans;
}
static int is_bst(N *t) {
    int ok = 1, have = 0, prev = 0;
    while (t) {
        if (!t->l) {
            if (have && t->v <= prev) ok = 0;
            prev = t->v; have = 1;
            t = t->r;
        } else {
            N *p = t->l;
            while (p->r && p->r != t) p = p->r;
            if (!p->r) { p->r = t; t = t->l; }
            else {
                p->r = NULL;
                if (have && t->v <= prev) ok = 0;
                prev = t->v; have = 1;
                t = t->r;
            }
        }
    }
    return ok;
}
static N *copy(N *t) {
    if (!t) return NULL;
    N *n = mk(t->v);
    n->l = copy(t->l); n->r = copy(t->r);
    return n;
}
static int same(N *a, N *b) {
    if (!a || !b) return a == b;
    return a->v == b->v && same(a->l, b->l) && same(a->r, b->r);
}
static N *random_shape(int n, int *next) {
    if (n == 0) return NULL;
    int left = (int)(rnd() % (unsigned)n);
    N *t = mk(0);
    t->l = random_shape(left, next);
    t->v = (*next)++;              /* values assigned in inorder: makes it a BST */
    t->r = random_shape(n - 1 - left, next);
    return t;
}
static void scramble(N *t) {
    if (!t) return;
    t->v = (int)(rnd() % 1000);
    scramble(t->l); scramble(t->r);
}
static unsigned hash(const int *a, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (unsigned)a[i]; h *= 16777619u; }
    return h;
}

int main(void) {
    static int B[MAXN];
    long total_nodes = 0;
    int bst_ok = 0, rounds = 60;
    for (int round = 0; round < rounds; round++) {
        int size = (int)(rnd() % 150) + 1, next = 0;
        N *t = random_shape(size, &next);
        if (round % 3 == 2) scramble(t);
        N *snap = copy(t);
        int n = cnt(t), m;
        total_nodes += n;
        rn = 0; in_r(t);
        m = morris_in(t, B); check(m == n && !memcmp(ro, B, sizeof(int) * (size_t)n), "morris inorder");
        check(same(t, snap), "restored after inorder");
        rn = 0; pre_r(t);
        m = morris_pre(t, B); check(m == n && !memcmp(ro, B, sizeof(int) * (size_t)n), "morris preorder");
        check(same(t, snap), "restored after preorder");
        rn = 0; post_r(t);
        m = morris_post(t, B); check(m == n && !memcmp(ro, B, sizeof(int) * (size_t)n), "morris postorder");
        check(same(t, snap), "restored after postorder");
        int bst = is_bst(t);
        check(same(t, snap), "restored after is_bst");
        if (round % 3 != 2) check(bst, "constructed BST");
        bst_ok += bst;
        if (round % 3 != 2) {
            for (int k = 1; k <= n; k += 1 + n / 7) {
                int got = kth_smallest(t, k);
                rn = 0; in_r(t);
                check(got == ro[k - 1], "kth smallest");
                check(same(t, snap), "restored after kth");
            }
        }
        if (round < 3) {
            rn = 0; post_r(t);
            printf("round %d: n=%d bst=%d postorder hash=%u\n", round, n, bst, hash(ro, n));
        }
        freet(t); freet(snap);
    }
    printf("%d rounds, %ld nodes, %d valid BSTs, temporary links created=%ld\n", rounds, total_nodes, bst_ok, links_made);
    /* degenerate shapes */
    N *chain = NULL, *zig = NULL;
    for (int i = 20; i >= 1; i--) { N *n = mk(i); n->r = chain; chain = n; }
    N **pp = &zig;
    for (int i = 1; i <= 20; i++) {
        N *n = mk(i);
        *pp = n;
        pp = (i % 2) ? &n->r : &n->l;
    }
    int m = morris_post(chain, B);
    printf("right chain postorder: %d..%d (%d nodes)\n", B[0], B[m - 1], m);
    check(B[0] == 20 && B[m - 1] == 1, "chain postorder");
    m = morris_pre(zig, B);
    printf("zigzag preorder first five: %d %d %d %d %d\n", B[0], B[1], B[2], B[3], B[4]);
    freet(chain); freet(zig);
    return 0;
}
