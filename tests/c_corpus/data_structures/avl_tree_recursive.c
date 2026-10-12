/*
 * title: AVL tree with recursive insert and delete
 * topic: data_structures
 * covers: AVL tree, height-balance, single and double rotations, delete rebalancing, invariant checker, rotation counters, sorted-array model
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 2048

typedef struct N {
    int k, h;
    struct N *l, *r;
} N;

static unsigned long long rs = 88172645463325252ULL;
static unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}
static void check(int c, const char *w) {
    if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); }
}
static int mod[MAXN], mn;
static int mfind(int k) {
    int lo = 0, hi = mn;
    while (lo < hi) { int m = (lo + hi) / 2; if (mod[m] < k) lo = m + 1; else hi = m; }
    return lo;
}
static int minsert(int k) {
    int p = mfind(k);
    if (p < mn && mod[p] == k) return 0;
    memmove(mod + p + 1, mod + p, (size_t)(mn - p) * sizeof(int));
    mod[p] = k; mn++;
    return 1;
}
static int mdelete(int k) {
    int p = mfind(k);
    if (p >= mn || mod[p] != k) return 0;
    memmove(mod + p, mod + p + 1, (size_t)(mn - p - 1) * sizeof(int));
    mn--;
    return 1;
}

static long rot_single, rot_double;
static int ht(N *t) { return t ? t->h : 0; }
static void upd(N *t) { int a = ht(t->l), b = ht(t->r); t->h = 1 + (a > b ? a : b); }
static N *rot_r(N *y) { N *x = y->l; y->l = x->r; x->r = y; upd(y); upd(x); return x; }
static N *rot_l(N *x) { N *y = x->r; x->r = y->l; y->l = x; upd(x); upd(y); return y; }
static N *rebalance(N *t) {
    upd(t);
    int bf = ht(t->l) - ht(t->r);
    if (bf > 1) {
        if (ht(t->l->l) < ht(t->l->r)) { t->l = rot_l(t->l); rot_double++; }
        else rot_single++;
        return rot_r(t);
    }
    if (bf < -1) {
        if (ht(t->r->r) < ht(t->r->l)) { t->r = rot_r(t->r); rot_double++; }
        else rot_single++;
        return rot_l(t);
    }
    return t;
}
static int changed;
static N *insert(N *t, int k) {
    if (!t) {
        N *n = malloc(sizeof *n);
        if (!n) exit(2);
        n->k = k; n->h = 1; n->l = n->r = NULL;
        changed = 1;
        return n;
    }
    if (k < t->k) t->l = insert(t->l, k);
    else if (k > t->k) t->r = insert(t->r, k);
    else return t;
    return rebalance(t);
}
static N *remove_min(N *t, N **out) {
    if (!t->l) { *out = t; return t->r; }
    t->l = remove_min(t->l, out);
    return rebalance(t);
}
static N *delete(N *t, int k) {
    if (!t) return NULL;
    if (k < t->k) t->l = delete(t->l, k);
    else if (k > t->k) t->r = delete(t->r, k);
    else {
        changed = 1;
        N *l = t->l, *r = t->r;
        free(t);
        if (!r) return l;
        N *m;
        r = remove_min(r, &m);
        m->l = l; m->r = r;
        t = m;
    }
    return rebalance(t);
}
static int verify(N *t, long lo, long hi) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "bst order");
    int c = 1 + verify(t->l, lo, t->k) + verify(t->r, t->k, hi);
    int a = ht(t->l), b = ht(t->r);
    check(t->h == 1 + (a > b ? a : b), "stored height");
    check(a - b >= -1 && a - b <= 1, "balance factor");
    return c;
}
static int no, outk[MAXN];
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }
static int search(N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t != NULL;
}

int main(void) {
    N *root = NULL;
    int ins = 0, del = 0, maxh = 0;
    /* phase 1: ascending inserts (worst case for plain BST) */
    for (int i = 0; i < 500; i++) {
        changed = 0; root = insert(root, i); minsert(i); ins += changed;
        check(verify(root, -1, 100000) == mn, "size asc");
    }
    printf("ascending 500: height=%d rotations single=%ld double=%ld\n", ht(root), rot_single, rot_double);
    /* phase 2: delete the lower half */
    for (int i = 0; i < 250; i++) {
        changed = 0; root = delete(root, i); mdelete(i); del += changed;
        check(verify(root, -1, 100000) == mn, "size del");
    }
    printf("after deleting 250: size=%d height=%d\n", mn, ht(root));
    /* phase 3: random mixed workload */
    for (int op = 0; op < 4000; op++) {
        int k = (int)(rnd() % 600);
        if (rnd() % 2) {
            changed = 0; root = insert(root, k);
            check(changed == minsert(k), "insert result"); ins += changed;
        } else {
            changed = 0; root = delete(root, k);
            check(changed == mdelete(k), "delete result"); del += changed;
        }
        check(verify(root, -1, 100000) == mn, "size");
        no = 0; inorder(root);
        check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
        check(search(root, k) == (mfind(k) < mn && mod[mfind(k)] == k), "search");
        if (ht(root) > maxh) maxh = ht(root);
    }
    /* AVL height bound: h <= 1.44 log2(n+2) */
    int bound = 1;
    for (int x = mn + 2; x > 1; x >>= 1) bound++;
    check(ht(root) <= bound * 3 / 2 + 1, "height bound");
    printf("random: inserts=%d deletes=%d size=%d maxheight=%d finalheight=%d\n", ins, del, mn, maxh, ht(root));
    printf("rotations single=%ld double=%ld\n", rot_single, rot_double);
    /* drain everything */
    while (mn) {
        int k = mod[rnd() % (unsigned)mn];
        root = delete(root, k); mdelete(k);
        check(verify(root, -1, 100000) == mn, "drain");
    }
    check(root == NULL, "empty");
    printf("drained\n");
    freet(root);
    return 0;
}
