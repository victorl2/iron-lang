/*
 * title: Join-based AVL tree with split, union, intersection and difference
 * topic: data_structures
 * covers: AVL tree, join by height, split by key, join2, divide-and-conquer set union intersection difference, node reuse, invariant checker, sorted-array model
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
    struct N *l, *r;
} N;

static int ht(N *t) { return t ? t->h : 0; }
static void upd(N *t) { int a = ht(t->l), b = ht(t->r); t->h = 1 + (a > b ? a : b); }
static N *rot_l(N *x) { N *y = x->r; x->r = y->l; y->l = x; upd(x); upd(y); return y; }
static N *rot_r(N *y) { N *x = y->l; y->l = x->r; x->r = y; upd(y); upd(x); return x; }
static N *mknode(int k) {
    N *n = xmalloc(sizeof *n);
    n->k = k; n->h = 1; n->l = n->r = NULL;
    return n;
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

/* join(L, k, R): every key in L < k.key < every key in R; node k is reused */
static N *join(N *tl, N *k, N *tr);
static N *join_right(N *tl, N *k, N *tr) {
    N *l = tl->l, *c = tl->r;
    if (ht(c) <= ht(tr) + 1) {
        k->l = c; k->r = tr; upd(k);
        if (ht(k) <= ht(l) + 1) { tl->r = k; upd(tl); return tl; }
        tl->r = rot_r(k); upd(tl);
        return rot_l(tl);
    }
    tl->r = join_right(c, k, tr);
    upd(tl);
    if (ht(tl->r) <= ht(l) + 1) return tl;
    return rot_l(tl);
}
static N *join_left(N *tl, N *k, N *tr) {
    N *c = tr->l, *r = tr->r;
    if (ht(c) <= ht(tl) + 1) {
        k->r = c; k->l = tl; upd(k);
        if (ht(k) <= ht(r) + 1) { tr->l = k; upd(tr); return tr; }
        tr->l = rot_l(k); upd(tr);
        return rot_r(tr);
    }
    tr->l = join_left(tl, k, c);
    upd(tr);
    if (ht(tr->l) <= ht(r) + 1) return tr;
    return rot_r(tr);
}
static N *join(N *tl, N *k, N *tr) {
    if (ht(tl) > ht(tr) + 1) return join_right(tl, k, tr);
    if (ht(tr) > ht(tl) + 1) return join_left(tl, k, tr);
    k->l = tl; k->r = tr; upd(k);
    return k;
}
static N *split_last(N *t, N **m) {
    if (!t->r) { *m = t; N *l = t->l; t->l = NULL; return l; }
    N *rest = split_last(t->r, m);
    N *l = t->l;
    t->l = t->r = NULL;
    return join(l, t, rest);
}
static N *join2(N *a, N *b) {
    if (!a) return b;
    N *m;
    a = split_last(a, &m);
    return join(a, m, b);
}
/* split t at key k: *L < k < *R; returns 1 if k was present (that node is freed) */
static int split(N *t, int k, N **L, N **R) {
    if (!t) { *L = *R = NULL; return 0; }
    N *l = t->l, *r = t->r;
    t->l = t->r = NULL;
    if (k == t->k) { *L = l; *R = r; free(t); return 1; }
    int found;
    if (k < t->k) {
        N *ll, *lr;
        found = split(l, k, &ll, &lr);
        *L = ll; *R = join(lr, t, r);
    } else {
        N *rl, *rr;
        found = split(r, k, &rl, &rr);
        *L = join(l, t, rl); *R = rr;
    }
    return found;
}
static N *unite(N *a, N *b) {
    if (!a) return b;
    if (!b) return a;
    N *l = a->l, *r = a->r, *bl, *br;
    a->l = a->r = NULL;
    split(b, a->k, &bl, &br);
    return join(unite(l, bl), a, unite(r, br));
}
static N *intersect(N *a, N *b) {
    if (!a || !b) { freet(a); freet(b); return NULL; }
    N *l = a->l, *r = a->r, *bl, *br;
    a->l = a->r = NULL;
    int found = split(b, a->k, &bl, &br);
    N *L = intersect(l, bl), *R = intersect(r, br);
    if (found) return join(L, a, R);
    free(a);
    return join2(L, R);
}
static N *difference(N *a, N *b) {   /* a minus b */
    if (!a) { freet(b); return NULL; }
    if (!b) return a;
    N *l = a->l, *r = a->r, *bl, *br;
    a->l = a->r = NULL;
    int found = split(b, a->k, &bl, &br);
    N *L = difference(l, bl), *R = difference(r, br);
    if (found) { free(a); return join2(L, R); }
    return join(L, a, R);
}
static N *insert(N *t, int k) {
    N *l, *r;
    int found = split(t, k, &l, &r);
    (void)found;
    return join(l, mknode(k), r);
}
static N *delete(N *t, int k) {
    N *l, *r;
    split(t, k, &l, &r);
    return join2(l, r);
}
static int verify(N *t, long lo, long hi) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "order");
    int c = 1 + verify(t->l, lo, t->k) + verify(t->r, t->k, hi);
    int a = ht(t->l), b = ht(t->r);
    check(t->h == 1 + (a > b ? a : b), "height field");
    check(a - b >= -1 && a - b <= 1, "balance");
    return c;
}
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static N *from_sorted(const int *a, int lo, int hi) {
    if (lo >= hi) return NULL;
    int m = (lo + hi) / 2;
    N *t = mknode(a[m]);
    t->l = from_sorted(a, lo, m);
    t->r = from_sorted(a, m + 1, hi);
    upd(t);
    return t;
}
static int has(N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t != NULL;
}
static void expect(N *t, const int *m, int n, const char *what) {
    check(verify(t, -1, 1000000) == n, what);
    no = 0; inorder(t);
    check(no == n && !memcmp(outk, m, sizeof(int) * (size_t)n), what);
}
static int gen_set(int *dst, int n, int range) {
    /* n random distinct sorted values below range */
    static unsigned char seen[1000];
    memset(seen, 0, sizeof seen);
    int c = 0;
    while (c < n) {
        int v = (int)(rnd() % (unsigned)range);
        if (!seen[v]) { seen[v] = 1; c++; }
    }
    c = 0;
    for (int v = 0; v < range; v++) if (seen[v]) dst[c++] = v;
    return c;
}

int main(void) {
    static int A[MAXN], B[MAXN], R[MAXN];
    long total = 0;
    int shown = 0;
    for (int round = 0; round < 40; round++) {
        int na = (int)(rnd() % 200), nb = (int)(rnd() % 200);
        if (round % 7 == 0) na = 3;
        if (round % 11 == 0) nb = 0;
        na = gen_set(A, na, 400);
        nb = gen_set(B, nb, 400);
        /* union */
        int nr = 0, i = 0, j = 0;
        while (i < na || j < nb) {
            if (j >= nb || (i < na && A[i] < B[j])) R[nr++] = A[i++];
            else if (i >= na || B[j] < A[i]) R[nr++] = B[j++];
            else { R[nr++] = A[i]; i++; j++; }
        }
        N *t = unite(from_sorted(A, 0, na), from_sorted(B, 0, nb));
        expect(t, R, nr, "union");
        int nu = nr;
        freet(t);
        /* intersection */
        nr = 0; i = j = 0;
        while (i < na && j < nb) {
            if (A[i] < B[j]) i++;
            else if (B[j] < A[i]) j++;
            else { R[nr++] = A[i]; i++; j++; }
        }
        t = intersect(from_sorted(A, 0, na), from_sorted(B, 0, nb));
        expect(t, R, nr, "intersection");
        int ni = nr;
        freet(t);
        /* difference */
        nr = 0;
        for (i = 0; i < na; i++) {
            int in_b = 0;
            for (j = 0; j < nb; j++) if (B[j] == A[i]) in_b = 1;
            if (!in_b) R[nr++] = A[i];
        }
        t = difference(from_sorted(A, 0, na), from_sorted(B, 0, nb));
        expect(t, R, nr, "difference");
        int nd = nr;
        freet(t);
        check(nu == na + nb - ni, "inclusion-exclusion");
        total += nu;
        if (shown < 4) { printf("|A|=%d |B|=%d union=%d intersection=%d A-B=%d\n", na, nb, nu, ni, nd); shown++; }
    }
    printf("total union size over 40 rounds=%ld\n", total);
    /* join of trees with very different heights */
    {
        int big[MAXN], small[8];
        for (int i = 0; i < 1000; i++) big[i] = i;
        for (int i = 0; i < 5; i++) small[i] = 2000 + i;
        N *a = from_sorted(big, 0, 1000), *b = from_sorted(small, 0, 5);
        int hb = ht(a), hs = ht(b);
        N *j = join(a, mknode(1500), b);
        check(verify(j, -1, 1000000) == 1006, "unbalanced join");
        printf("join heights %d and %d -> %d, size %d\n", hb, hs, ht(j), 1006);
        N *j2 = join(NULL, mknode(-5), j);
        check(verify(j2, -10, 1000000) == 1007, "join with empty left");
        freet(j2);
    }
    /* insert and delete via split and join */
    N *t = NULL;
    int ins_n = 0, del_n = 0;
    for (int op = 0; op < 3000; op++) {
        int k = (int)(rnd() % 500);
        if (rnd() % 2) { int a = !mhas(k); t = a ? insert(t, k) : t; if (a) { minsert(k); ins_n++; } }
        else { int a = mhas(k); if (a) { t = delete(t, k); mdelete(k); del_n++; } }
        check(has(t, k) == mhas(k), "search");
        if (op % 5 == 0) expect(t, mod, mn, "model");
    }
    expect(t, mod, mn, "model final");
    printf("split/join updates: inserts=%d deletes=%d size=%d height=%d\n", ins_n, del_n, mn, ht(t));
    /* split at every pivot of a small tree and rejoin */
    int pivots = 0;
    for (int k = -1; k <= 500; k += 37) {
        N *l, *r;
        int found = split(t, k, &l, &r);
        int p = mfind(k);
        check(found == (p < mn && mod[p] == k), "split found flag");
        check(verify(l, -1, 1000000) == p, "left part size");
        check(verify(r, -1, 1000000) == mn - p - found, "right part size");
        t = join2(l, r);
        if (found) t = insert(t, k);
        expect(t, mod, mn, "rejoined");
        pivots++;
    }
    printf("split/rejoin at %d pivots ok\n", pivots);
    freet(t);
    return 0;
}
