/*
 * title: Zip tree with unzip insert and zip delete
 * topic: data_structures
 * covers: zip tree, geometric ranks derived from key hash, unzip on insert, zip on delete, history independence, rank histogram, sorted-array model
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
    int k, rank;
    struct N *l, *r;
} N;

static unsigned long long mix(unsigned long long z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
/* geometric rank: number of trailing one bits of a hash of the key */
static int rank_of(int k) {
    unsigned long long h = mix((unsigned long long)(unsigned)k + 12345ULL);
    int r = 0;
    while ((h & 1) && r < 40) { r++; h >>= 1; }
    return r;
}
static N *mknode(int k) {
    N *x = xmalloc(sizeof *x);
    x->k = k; x->rank = rank_of(k); x->l = x->r = NULL;
    return x;
}
static void unzip(N *t, int k, N **L, N **R) {
    if (!t) { *L = *R = NULL; return; }
    if (t->k < k) { *L = t; unzip(t->r, k, &t->r, R); }
    else { *R = t; unzip(t->l, k, L, &t->l); }
}
static N *zip(N *f, N *g) {
    if (!f) return g;
    if (!g) return f;
    if (f->rank < g->rank) { g->l = zip(f, g->l); return g; }
    f->r = zip(f->r, g);
    return f;
}
static N *ins(N *t, N *x, int *added) {
    if (!t) { *added = 1; return x; }
    if (t->k == x->k) { free(x); return t; }
    if (x->rank > t->rank || (x->rank == t->rank && x->k < t->k)) {
        /* x must not already exist below: check first */
        N *q = t;
        while (q && q->k != x->k) q = x->k < q->k ? q->l : q->r;
        if (q) { free(x); return t; }
        unzip(t, x->k, &x->l, &x->r);
        *added = 1;
        return x;
    }
    if (x->k < t->k) t->l = ins(t->l, x, added);
    else t->r = ins(t->r, x, added);
    return t;
}
static N *del(N *t, int k, int *removed) {
    if (!t) return NULL;
    if (k < t->k) t->l = del(t->l, k, removed);
    else if (k > t->k) t->r = del(t->r, k, removed);
    else {
        N *z = zip(t->l, t->r);
        free(t);
        *removed = 1;
        return z;
    }
    return t;
}
static int verify(N *t, long lo, long hi) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "order");
    check(t->rank == rank_of(t->k), "rank is a function of key");
    if (t->l) check(t->l->rank < t->rank, "left child rank strictly lower");
    if (t->r) check(t->r->rank <= t->rank, "right child rank not higher");
    return 1 + verify(t->l, lo, t->k) + verify(t->r, t->k, hi);
}
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static int same(N *a, N *b) {
    if (!a || !b) return a == b;
    return a->k == b->k && same(a->l, b->l) && same(a->r, b->r);
}
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static int has(N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t != NULL;
}
static void hist(N *t, int *h) { if (!t) return; h[t->rank < 9 ? t->rank : 9]++; hist(t->l, h); hist(t->r, h); }
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    N *root = NULL;
    int added, ins_n = 0, del_n = 0, maxh = 0;
    for (int i = 0; i < 1000; i++) {
        added = 0; root = ins(root, mknode(i), &added); minsert(i);
    }
    check(verify(root, -1, 100000) == mn, "asc");
    printf("ascending 1000: height=%d root rank=%d\n", height(root), root->rank);
    for (int op = 0; op < 5000; op++) {
        int k = (int)(rnd() % 1500);
        added = 0;
        if (rnd() % 2) {
            root = ins(root, mknode(k), &added);
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
    printf("random: inserts=%d deletes=%d size=%d maxheight=%d\n", ins_n, del_n, mn, maxh);
    int h[10] = {0};
    hist(root, h);
    printf("rank histogram:");
    for (int i = 0; i < 10; i++) printf(" %d", h[i]);
    printf("\n");
    /* history independence: same key set inserted in another order gives the same shape */
    N *alt = NULL;
    static int order[MAXN];
    for (int i = 0; i < mn; i++) order[i] = mod[i];
    for (int i = mn - 1; i > 0; i--) {
        int j = (int)(rnd() % (unsigned)(i + 1)), t = order[i];
        order[i] = order[j]; order[j] = t;
    }
    for (int i = 0; i < mn; i++) { added = 0; alt = ins(alt, mknode(order[i]), &added); }
    check(same(root, alt), "history independence");
    printf("shuffled rebuild has identical shape: yes (height %d)\n", height(alt));
    freet(alt);
    while (mn) {
        int k = mod[(unsigned)mn / 2];
        added = 0; root = del(root, k, &added); check(added, "drain"); mdelete(k);
        check(verify(root, -1, 100000) == mn, "drain size");
    }
    printf("drained\n");
    return 0;
}
