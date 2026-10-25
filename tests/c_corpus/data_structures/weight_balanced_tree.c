/*
 * title: Weight-balanced tree (BB[alpha]) with size-based rotations
 * topic: data_structures
 * covers: weight-balanced tree, delta and gamma rotation parameters, subtree sizes, single and double rotations, balance invariant on weights, sorted-array model
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

#define DELTA 3
#define GAMMA 2

typedef struct N {
    int k, size;
    struct N *l, *r;
} N;

static long rots;
static int sz(N *t) { return t ? t->size : 0; }
static void upd(N *t) { t->size = 1 + sz(t->l) + sz(t->r); }
static N *rotl(N *x) { N *y = x->r; x->r = y->l; y->l = x; upd(x); upd(y); rots++; return y; }
static N *rotr(N *y) { N *x = y->l; y->l = x->r; x->r = y; upd(y); upd(x); rots++; return x; }
static N *balance(N *t) {
    upd(t);
    int wl = sz(t->l) + 1, wr = sz(t->r) + 1;
    if (wr > DELTA * wl) {
        if (sz(t->r->l) + 1 < GAMMA * (sz(t->r->r) + 1)) return rotl(t);
        t->r = rotr(t->r);
        return rotl(t);
    }
    if (wl > DELTA * wr) {
        if (sz(t->l->r) + 1 < GAMMA * (sz(t->l->l) + 1)) return rotr(t);
        t->l = rotl(t->l);
        return rotr(t);
    }
    return t;
}
static N *ins(N *t, int k, int *added) {
    if (!t) {
        N *n = xmalloc(sizeof *n);
        n->k = k; n->size = 1; n->l = n->r = NULL;
        *added = 1;
        return n;
    }
    if (k < t->k) t->l = ins(t->l, k, added);
    else if (k > t->k) t->r = ins(t->r, k, added);
    else return t;
    return balance(t);
}
static N *remove_min(N *t, N **m) {
    if (!t->l) { *m = t; return t->r; }
    t->l = remove_min(t->l, m);
    return balance(t);
}
static N *del(N *t, int k, int *removed) {
    if (!t) return NULL;
    if (k < t->k) t->l = del(t->l, k, removed);
    else if (k > t->k) t->r = del(t->r, k, removed);
    else {
        *removed = 1;
        N *l = t->l, *r = t->r;
        free(t);
        if (!r) return l;
        N *m;
        r = remove_min(r, &m);
        m->l = l; m->r = r;
        t = m;
    }
    return balance(t);
}
static int verify(N *t, long lo, long hi) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "order");
    int c = 1 + verify(t->l, lo, t->k) + verify(t->r, t->k, hi);
    check(c == t->size, "size field");
    int wl = sz(t->l) + 1, wr = sz(t->r) + 1;
    check(wl <= DELTA * wr && wr <= DELTA * wl, "weight balance");
    return c;
}
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static int has(N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t != NULL;
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    N *root = NULL;
    int added, ins_n = 0, del_n = 0, maxh = 0;
    for (int i = 0; i < 600; i++) {
        added = 0; root = ins(root, i, &added); minsert(i);
        check(verify(root, -1, 100000) == mn, "asc");
    }
    printf("ascending 600: height=%d rotations=%ld\n", height(root), rots);
    for (int i = 0; i < 600; i += 3) {
        added = 0; root = del(root, i, &added); check(added, "present"); mdelete(i);
        check(verify(root, -1, 100000) == mn, "del asc");
    }
    printf("after deleting every third: size=%d height=%d\n", mn, height(root));
    for (int op = 0; op < 5000; op++) {
        int k = (int)(rnd() % 800);
        added = 0;
        if (rnd() % 2) {
            root = ins(root, k, &added);
            check(added == minsert(k), "insert result"); ins_n += added;
        } else {
            root = del(root, k, &added);
            check(added == mdelete(k), "delete result"); del_n += added;
        }
        check(verify(root, -1, 100000) == mn, "size");
        check(has(root, k) == mhas(k), "search");
        int h = height(root);
        if (h > maxh) maxh = h;
        if (op % 10 == 0) {
            no = 0; inorder(root);
            check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
        }
    }
    printf("random: inserts=%d deletes=%d size=%d maxheight=%d rotations=%ld\n", ins_n, del_n, mn, maxh, rots);
    while (mn) {
        int k = mod[(unsigned)mn / 2];
        added = 0; root = del(root, k, &added); check(added, "drain"); mdelete(k);
        check(verify(root, -1, 100000) == mn, "drain size");
    }
    printf("drained\n");
    freet(root);
    return 0;
}
