/*
 * title: Treap with rotation insert and delete
 * topic: data_structures
 * covers: treap, heap-ordered priorities, rotations up on insert, rotations down on delete, expected logarithmic height, sorted-array model
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
    unsigned pri;
    struct N *l, *r;
} N;

static long rots;
static N *rotr(N *y) { N *x = y->l; y->l = x->r; x->r = y; rots++; return x; }
static N *rotl(N *x) { N *y = x->r; x->r = y->l; y->l = x; rots++; return y; }
static N *ins(N *t, int k, unsigned pri, int *added) {
    if (!t) {
        N *n = xmalloc(sizeof *n);
        n->k = k; n->pri = pri; n->l = n->r = NULL;
        *added = 1;
        return n;
    }
    if (k < t->k) {
        t->l = ins(t->l, k, pri, added);
        if (t->l->pri > t->pri) t = rotr(t);
    } else if (k > t->k) {
        t->r = ins(t->r, k, pri, added);
        if (t->r->pri > t->pri) t = rotl(t);
    }
    return t;
}
static N *del(N *t, int k, int *removed) {
    if (!t) return NULL;
    if (k < t->k) t->l = del(t->l, k, removed);
    else if (k > t->k) t->r = del(t->r, k, removed);
    else {
        if (!t->l || !t->r) {
            N *c = t->l ? t->l : t->r;
            free(t);
            *removed = 1;
            return c;
        }
        if (t->l->pri > t->r->pri) { t = rotr(t); t->r = del(t->r, k, removed); }
        else { t = rotl(t); t->l = del(t->l, k, removed); }
    }
    return t;
}
static int verify(N *t, long lo, long hi) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "order");
    if (t->l) check(t->l->pri <= t->pri, "heap left");
    if (t->r) check(t->r->pri <= t->pri, "heap right");
    return 1 + verify(t->l, lo, t->k) + verify(t->r, t->k, hi);
}
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static int has(N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t != NULL;
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    N *root = NULL;
    int added, ins_n = 0, del_n = 0, maxh = 0;
    for (int i = 0; i < 1000; i++) {
        added = 0; root = ins(root, i, rnd(), &added); minsert(i);
    }
    check(verify(root, -1, 100000) == mn, "asc");
    printf("ascending 1000: height=%d rotations=%ld\n", height(root), rots);
    check(height(root) < 40, "expected logarithmic height");
    for (int i = 0; i < 1000; i += 2) {
        added = 0; root = del(root, i, &added); check(added, "del present"); mdelete(i);
    }
    check(verify(root, -1, 100000) == mn, "after even deletes");
    printf("after deleting evens: size=%d height=%d\n", mn, height(root));
    for (int op = 0; op < 4000; op++) {
        int k = (int)(rnd() % 1200);
        added = 0;
        if (rnd() % 2) {
            root = ins(root, k, rnd(), &added);
            check(added == minsert(k), "insert result"); ins_n += added;
        } else {
            root = del(root, k, &added);
            check(added == mdelete(k), "delete result"); del_n += added;
        }
        check(verify(root, -1, 100000) == mn, "size");
        check(has(root, k) == mhas(k), "search");
        if (op % 10 == 0) {
            no = 0; inorder(root);
            check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
        }
        int h = height(root);
        if (h > maxh) maxh = h;
    }
    printf("random: inserts=%d deletes=%d size=%d maxheight=%d rotations=%ld\n", ins_n, del_n, mn, maxh, rots);
    while (mn) {
        int k = mod[(unsigned)mn / 2];
        added = 0; root = del(root, k, &added); check(added, "drain"); mdelete(k);
    }
    check(root == NULL, "empty");
    printf("drained\n");
    freet(root);
    return 0;
}
