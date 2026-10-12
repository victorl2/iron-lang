/*
 * title: Red-black tree with iterative top-down insertion
 * topic: data_structures
 * covers: red-black tree, single-pass top-down insertion, color flips on descent, grandparent and great-grandparent tracking, sentinel nil, header node, invariant checker
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

enum { RED, BLACK };

typedef struct N {
    int k, color;
    struct N *l, *r;
} N;

static N nil_node;
static N *const NIL = &nil_node;
static N *header, *cur, *parent, *grand, *great;
static long flips, rots;

static N *mk(int k) {
    N *n = xmalloc(sizeof *n);
    n->k = k; n->color = BLACK; n->l = n->r = NIL;
    return n;
}
static N *rot_left_child(N *k2) { N *k1 = k2->l; k2->l = k1->r; k1->r = k2; rots++; return k1; }
static N *rot_right_child(N *k1) { N *k2 = k1->r; k1->r = k2->l; k2->l = k1; rots++; return k2; }
static N *rotate(int item, N *par) {
    if (item < par->k) {
        par->l = item < par->l->k ? rot_left_child(par->l) : rot_right_child(par->l);
        return par->l;
    }
    par->r = item < par->r->k ? rot_left_child(par->r) : rot_right_child(par->r);
    return par->r;
}
static void reorient(int item) {
    cur->color = RED;
    cur->l->color = BLACK;
    cur->r->color = BLACK;
    flips++;
    if (parent->color == RED) {
        grand->color = RED;
        if ((item < grand->k) != (item < parent->k)) parent = rotate(item, grand);
        cur = rotate(item, great);
        cur->color = BLACK;
    }
    header->r->color = BLACK;
}
static int insert(int item) {
    cur = parent = grand = header;
    NIL->k = item;
    while (cur->k != item) {
        great = grand; grand = parent; parent = cur;
        cur = item < cur->k ? cur->l : cur->r;
        if (cur->l->color == RED && cur->r->color == RED) reorient(item);
    }
    if (cur != NIL) return 0;
    cur = mk(item);
    cur->color = BLACK;
    if (item < parent->k) parent->l = cur; else parent->r = cur;
    reorient(item);
    return 1;
}
static int has(int k) {
    N *x = header->r;
    while (x != NIL && x->k != k) x = k < x->k ? x->l : x->r;
    return x != NIL;
}
static int bh_check(N *t, long lo, long hi, int *cnt, int depth, int *maxd) {
    if (t == NIL) { if (depth > *maxd) *maxd = depth; return 1; }
    check(t->k > lo && t->k < hi, "order");
    if (t->color == RED) check(t->l->color == BLACK && t->r->color == BLACK, "red-red");
    (*cnt)++;
    int a = bh_check(t->l, lo, t->k, cnt, depth + 1, maxd);
    int b = bh_check(t->r, t->k, hi, cnt, depth + 1, maxd);
    check(a == b, "black height");
    return a + (t->color == BLACK);
}
static void inorder(N *t) { if (t == NIL) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static int verify(int *bh, int *maxd) {
    N *root = header->r;
    check(root == NIL || root->color == BLACK, "root black");
    int cnt = 0;
    *maxd = 0;
    *bh = bh_check(root, -1, 1000000, &cnt, 0, maxd);
    check(cnt == mn, "count");
    no = 0; inorder(root);
    check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
    return cnt;
}
static void freet(N *t) { if (t == NIL) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    nil_node.color = BLACK; nil_node.l = nil_node.r = NIL;
    header = mk(-1000000);
    header->l = header->r = NIL;
    int bh, md, added = 0, worst = 0;
    for (int i = 0; i < 300; i++) { insert(i); minsert(i); verify(&bh, &md); }
    printf("ascending 300: black height=%d depth=%d rotations=%ld flips=%ld\n", bh, md, rots, flips);
    freet(header->r); header->r = NIL; mn = 0;
    for (int i = 300; i > 0; i--) { insert(i); minsert(i); verify(&bh, &md); }
    printf("descending 300: black height=%d depth=%d rotations=%ld flips=%ld\n", bh, md, rots, flips);
    freet(header->r); header->r = NIL; mn = 0;
    long dups = 0;
    for (int op = 0; op < 4000; op++) {
        int k = (int)(rnd() % 1500);
        int a = insert(k);
        check(a == minsert(k), "insert result");
        added += a; dups += !a;
        verify(&bh, &md);
        check(has(k), "present after insert");
        if (md > worst) worst = md;
        check(has(k + 5000) == 0, "absent key");
    }
    printf("random: inserted=%d duplicates=%ld size=%d black height=%d worst depth=%d\n", added, dups, mn, bh, worst);
    check(worst <= 2 * bh + 1 && bh <= 12, "depth bounded by twice black height");
    printf("rotations=%ld flips=%ld\n", rots, flips);
    freet(header->r);
    free(header);
    return 0;
}
