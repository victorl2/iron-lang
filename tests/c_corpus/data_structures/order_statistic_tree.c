/*
 * title: Order-statistic tree (rank and select) on a red-black core
 * topic: data_structures
 * covers: order statistic tree, size augmentation, rank, select, delete k-th, range counting, inversion counting, left-leaning red-black balancing, sorted-array model
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
    int k, red, size;
    struct N *l, *r;
} N;

static int isred(N *h) { return h && h->red; }
static int sz(N *t) { return t ? t->size : 0; }
static void upd(N *t) { t->size = 1 + sz(t->l) + sz(t->r); }
static N *rotl(N *h) {
    N *x = h->r;
    h->r = x->l; x->l = h;
    x->red = h->red; h->red = 1;
    upd(h); upd(x);
    return x;
}
static N *rotr(N *h) {
    N *x = h->l;
    h->l = x->r; x->r = h;
    x->red = h->red; h->red = 1;
    upd(h); upd(x);
    return x;
}
static void flip(N *h) { h->red = !h->red; h->l->red = !h->l->red; h->r->red = !h->r->red; }
static N *fixup(N *h) {
    upd(h);
    if (isred(h->r) && !isred(h->l)) h = rotl(h);
    if (isred(h->l) && isred(h->l->l)) h = rotr(h);
    if (isred(h->l) && isred(h->r)) flip(h);
    return h;
}
static N *ins(N *h, int k, int *added) {
    if (!h) {
        N *n = xmalloc(sizeof *n);
        n->k = k; n->red = 1; n->size = 1; n->l = n->r = NULL;
        *added = 1;
        return n;
    }
    if (k < h->k) h->l = ins(h->l, k, added);
    else if (k > h->k) h->r = ins(h->r, k, added);
    return fixup(h);
}
static N *mrl(N *h) {
    flip(h);
    if (isred(h->r->l)) { h->r = rotr(h->r); h = rotl(h); flip(h); }
    return h;
}
static N *mrr(N *h) {
    flip(h);
    if (isred(h->l->l)) { h = rotr(h); flip(h); }
    return h;
}
static N *del_min(N *h) {
    if (!h->l) { free(h); return NULL; }
    if (!isred(h->l) && !isred(h->l->l)) h = mrl(h);
    h->l = del_min(h->l);
    return fixup(h);
}
static N *del(N *h, int k) {
    if (k < h->k) {
        if (!isred(h->l) && !isred(h->l->l)) h = mrl(h);
        h->l = del(h->l, k);
    } else {
        if (isred(h->l)) h = rotr(h);
        if (k == h->k && !h->r) { free(h); return NULL; }
        if (!isred(h->r) && !isred(h->r->l)) h = mrr(h);
        if (k == h->k) {
            N *m = h->r;
            while (m->l) m = m->l;
            h->k = m->k;
            h->r = del_min(h->r);
        } else h->r = del(h->r, k);
    }
    return fixup(h);
}
static int has(N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t != NULL;
}
static int insert(N **root, int k) {
    int added = 0;
    *root = ins(*root, k, &added);
    (*root)->red = 0;
    return added;
}
static int delete(N **root, int k) {
    if (!has(*root, k)) return 0;
    if (!isred((*root)->l) && !isred((*root)->r)) (*root)->red = 1;
    *root = del(*root, k);
    if (*root) (*root)->red = 0;
    return 1;
}
/* number of keys strictly less than k */
static int rank_of(N *t, int k) {
    int r = 0;
    while (t) {
        if (k <= t->k) t = t->l;
        else { r += sz(t->l) + 1; t = t->r; }
    }
    return r;
}
/* i-th smallest key, 0-based */
static int select_kth(N *t, int i) {
    for (;;) {
        int ls = sz(t->l);
        if (i < ls) t = t->l;
        else if (i == ls) return t->k;
        else { i -= ls + 1; t = t->r; }
    }
}
static int verify(N *t, long lo, long hi, int *bh) {
    if (!t) { *bh = 0; return 0; }
    check(t->k > lo && t->k < hi, "order");
    check(!isred(t->r), "right-leaning red");
    if (t->red) check(!isred(t->l), "double red");
    int a, b;
    int c = 1 + verify(t->l, lo, t->k, &a) + verify(t->r, t->k, hi, &b);
    check(a == b, "black balance");
    check(c == t->size, "size field");
    *bh = a + !t->red;
    return c;
}
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }
static void verify_all(N *root) {
    int bh;
    check(verify(root, -1, 1000000, &bh) == mn, "size");
    no = 0; inorder(root);
    check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
}

int main(void) {
    N *root = NULL;
    int ins_n = 0, del_n = 0;
    long rank_queries = 0, select_queries = 0;
    for (int op = 0; op < 3000; op++) {
        int k = (int)(rnd() % 700);
        unsigned c = rnd() % 10;
        if (c < 5) { int a = insert(&root, k); check(a == minsert(k), "ins"); ins_n += a; }
        else if (c < 7) { int a = delete(&root, k); check(a == mdelete(k), "del"); del_n += a; }
        else if (c < 8 && mn > 0) {
            /* delete the k-th smallest */
            int i = (int)(rnd() % (unsigned)mn);
            int key = select_kth(root, i);
            check(key == mod[i], "select before delete");
            check(delete(&root, key), "delete kth"); mdelete(key); del_n++;
        }
        verify_all(root);
        check(rank_of(root, k) == mfind(k), "rank");
        check(has(root, k) == mhas(k), "has");
        rank_queries++;
        if (mn > 0) {
            int i = (int)(rnd() % (unsigned)mn);
            check(select_kth(root, i) == mod[i], "select");
            select_queries++;
        }
        int lo = (int)(rnd() % 700), hi = lo + (int)(rnd() % 100);
        int cnt = rank_of(root, hi + 1) - rank_of(root, lo);
        int bf = 0;
        for (int i = 0; i < mn; i++) if (mod[i] >= lo && mod[i] <= hi) bf++;
        check(cnt == bf, "range count");
    }
    printf("inserts=%d deletes=%d size=%d rank queries=%ld select queries=%ld\n", ins_n, del_n, mn, rank_queries, select_queries);
    for (int i = 0; i < mn; i++) {
        check(select_kth(root, i) == mod[i], "full select");
        check(rank_of(root, mod[i]) == i, "full rank");
    }
    printf("select(0)=%d median=%d select(last)=%d\n", select_kth(root, 0), select_kth(root, mn / 2), select_kth(root, mn - 1));
    /* median of a stream */
    freet(root); root = NULL; mn = 0;
    unsigned long long msum = 0;
    for (int i = 0; i < 400; i++) {
        int v = (int)(rnd() % 100000);
        if (insert(&root, v)) minsert(v);
        int med = select_kth(root, (mn - 1) / 2);
        check(med == mod[(mn - 1) / 2], "running median");
        msum += (unsigned)med;
    }
    printf("sum of 400 running medians=%llu\n", msum);
    /* inversion count of a permutation using rank queries */
    freet(root); root = NULL; mn = 0;
    enum { P = 300 };
    int perm[P];
    for (int i = 0; i < P; i++) perm[i] = i;
    for (int i = P - 1; i > 0; i--) {
        int j = (int)(rnd() % (unsigned)(i + 1)), t = perm[i];
        perm[i] = perm[j]; perm[j] = t;
    }
    long inv = 0, brute = 0;
    for (int i = 0; i < P; i++) {
        inv += sz(root) - rank_of(root, perm[i]);
        insert(&root, perm[i]); minsert(perm[i]);
    }
    for (int i = 0; i < P; i++) for (int j = i + 1; j < P; j++) brute += perm[i] > perm[j];
    check(inv == brute, "inversions");
    printf("inversions in a random permutation of %d: %ld\n", P, inv);
    freet(root);
    return 0;
}
