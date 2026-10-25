/*
 * title: Randomized binary search tree (Martinez and Roura)
 * topic: data_structures
 * covers: randomized BST, subtree sizes, insert at root by split, random join on delete, size-proportional probabilities, sorted-array model
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
    int k, size;
    struct N *l, *r;
} N;

static int sz(N *t) { return t ? t->size : 0; }
static void upd(N *t) { t->size = 1 + sz(t->l) + sz(t->r); }
static void split(N *t, int k, N **lo, N **hi) {
    if (!t) { *lo = *hi = NULL; return; }
    if (k < t->k) { *hi = t; split(t->l, k, lo, &t->l); }
    else { *lo = t; split(t->r, k, &t->r, hi); }
    upd(t);
}
static int has(N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t != NULL;
}
static N *ins(N *t, int k) {
    if (!t) {
        N *n = xmalloc(sizeof *n);
        n->k = k; n->size = 1; n->l = n->r = NULL;
        return n;
    }
    if (rnd() % (unsigned)(t->size + 1) == 0) {
        N *n = xmalloc(sizeof *n);
        n->k = k;
        split(t, k, &n->l, &n->r);
        upd(n);
        return n;
    }
    if (k < t->k) t->l = ins(t->l, k);
    else t->r = ins(t->r, k);
    upd(t);
    return t;
}
static N *join(N *a, N *b) {
    if (!a) return b;
    if (!b) return a;
    if (rnd() % (unsigned)(a->size + b->size) < (unsigned)a->size) {
        a->r = join(a->r, b);
        upd(a);
        return a;
    }
    b->l = join(a, b->l);
    upd(b);
    return b;
}
static N *del(N *t, int k) {
    if (k < t->k) t->l = del(t->l, k);
    else if (k > t->k) t->r = del(t->r, k);
    else {
        N *j = join(t->l, t->r);
        free(t);
        return j;
    }
    upd(t);
    return t;
}
static int verify(N *t, long lo, long hi) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "order");
    int c = 1 + verify(t->l, lo, t->k) + verify(t->r, t->k, hi);
    check(t->size == c, "size field");
    return c;
}
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}

int main(void) {
    N *root = NULL;
    int ins_n = 0, del_n = 0, maxh = 0;
    for (int i = 0; i < 800; i++) {
        root = ins(root, i); minsert(i);
    }
    check(verify(root, -1, 100000) == mn, "asc");
    printf("ascending 800: height=%d\n", height(root));
    check(height(root) < 60, "random shape despite sorted input");
    for (int op = 0; op < 4000; op++) {
        int k = (int)(rnd() % 1000);
        if (rnd() % 2) {
            int had = has(root, k);
            if (!had) { root = ins(root, k); ins_n++; }
            check((!had) == minsert(k), "insert");
        } else {
            int had = has(root, k);
            if (had) { root = del(root, k); del_n++; }
            check(had == mdelete(k), "delete");
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
    printf("random: inserts=%d deletes=%d size=%d maxheight=%d finalheight=%d\n", ins_n, del_n, mn, maxh, height(root));
    /* split by key partitions the set exactly */
    N *a, *b;
    int pivot = mod[mn / 2];
    split(root, pivot, &a, &b);
    check(verify(a, -1, pivot + 1) == mn / 2 + 1, "split low");
    check(verify(b, pivot, 100000) == mn - mn / 2 - 1, "split high");
    printf("split at %d: %d low, %d high\n", pivot, sz(a), sz(b));
    root = join(a, b);
    check(verify(root, -1, 100000) == mn, "rejoined");
    while (mn) {
        int k = mod[(unsigned)mn / 2];
        root = del(root, k); mdelete(k);
        check(verify(root, -1, 100000) == mn, "drain");
    }
    printf("drained\n");
    return 0;
}
