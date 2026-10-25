/*
 * title: Double-threaded binary search tree with header node
 * topic: data_structures
 * covers: threaded binary tree, inorder threads, header sentinel, successor and predecessor without stack, insert, delete cases A/B/C, thread invariants, sorted-array model
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
static int outk[MAXN];

#define INF 1000000000

typedef struct N {
    int k;
    int lthread, rthread;   /* 1 when the link is a thread */
    struct N *l, *r;
} N;

static N *mknode(int k) {
    N *x = xmalloc(sizeof *x);
    x->k = k; x->lthread = x->rthread = 1; x->l = x->r = NULL;
    return x;
}
static N *header_new(void) {
    N *h = mknode(INF);
    h->l = h; h->lthread = 1;
    h->r = h; h->rthread = 0;
    return h;
}
static N *succ(N *p) {
    if (p->rthread) return p->r;
    p = p->r;
    while (!p->lthread) p = p->l;
    return p;
}
static N *pred(N *p) {
    if (p->lthread) return p->l;
    p = p->l;
    while (!p->rthread) p = p->r;
    return p;
}
static int insert(N *h, int k) {
    N *par = h, *p = h;
    for (;;) {
        if (k == p->k && p != h) return 0;
        par = p;
        if (k < p->k) { if (p->lthread) break; p = p->l; }
        else { if (p->rthread) break; p = p->r; }
    }
    N *n = mknode(k);
    if (k < par->k) {
        n->l = par->l; n->r = par;
        par->l = n; par->lthread = 0;
    } else {
        n->r = par->r; n->l = par;
        par->r = n; par->rthread = 0;
    }
    return 1;
}
static void case_a(N *h, N *par, N *p) {
    (void)h;
    if (p == par->l) { par->lthread = 1; par->l = p->l; }
    else { par->rthread = 1; par->r = p->r; }
    free(p);
}
static void case_b(N *par, N *p) {
    N *child = p->lthread ? p->r : p->l;
    N *s = succ(p), *pr = pred(p);
    if (p == par->l) par->l = child; else par->r = child;
    if (!p->lthread) pr->r = s;    /* has a left child: its rightmost node threads to s */
    else s->l = pr;                /* has a right child: its leftmost node threads to pr */
    free(p);
}
static int delete(N *h, int k) {
    N *par = h, *p = h->l;
    if (h->lthread) return 0;
    for (;;) {
        if (k == p->k) break;
        par = p;
        if (k < p->k) { if (p->lthread) return 0; p = p->l; }
        else { if (p->rthread) return 0; p = p->r; }
    }
    if (!p->lthread && !p->rthread) {
        N *ps = p, *s = p->r;
        while (!s->lthread) { ps = s; s = s->l; }
        p->k = s->k;
        par = ps; p = s;
    }
    if (p->lthread && p->rthread) case_a(h, par, p);
    else case_b(par, p);
    return 1;
}
static int has(N *h, int k) {
    if (h->lthread) return 0;
    N *p = h->l;
    for (;;) {
        if (k == p->k) return 1;
        if (k < p->k) { if (p->lthread) return 0; p = p->l; }
        else { if (p->rthread) return 0; p = p->r; }
    }
}
static int count_struct(N *x, N *h, long lo, long hi) {
    /* follow real links only */
    int c = 1;
    check(x->k > lo && x->k < hi, "bounds");
    if (!x->lthread) c += count_struct(x->l, h, lo, x->k);
    if (!x->rthread) c += count_struct(x->r, h, x->k, hi);
    return c;
}
static int height(N *x) {
    if (!x) return 0;
    int a = x->lthread ? 0 : height(x->l), b = x->rthread ? 0 : height(x->r);
    return 1 + (a > b ? a : b);
}
static void verify(N *h) {
    int structural = h->lthread ? 0 : count_struct(h->l, h, -1, INF);
    check(structural == mn, "structural count");
    /* forward walk by threads */
    int n = 0;
    if (!h->lthread) {
        N *p = h->l;
        while (!p->lthread) p = p->l;
        for (; p != h; p = succ(p)) { check(n < mn, "walk overrun"); outk[n++] = p->k; }
    }
    check(n == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "forward threads equal model");
    /* backward walk */
    n = 0;
    if (!h->lthread) {
        N *p = h->l;
        while (!p->rthread) p = p->r;
        for (int i = mn - 1; i >= 0; i--, p = pred(p)) check(p != h && p->k == mod[i], "backward threads");
        check(p == h, "backward ends at header");
    }
    (void)n;
}
static void freeall(N *h) {
    if (!h->lthread) {
        N *p = h->l;
        while (!p->lthread) p = p->l;
        while (p != h) { N *nx = succ(p); free(p); p = nx; }
    }
    free(h);
}

int main(void) {
    N *h = header_new();
    int ins_n = 0, del_n = 0, maxh = 0;
    verify(h);
    for (int op = 0; op < 4000; op++) {
        int k = (int)(rnd() % 300);
        if (rnd() % 100 < 55) {
            int a = insert(h, k);
            check(a == minsert(k), "insert result"); ins_n += a;
        } else {
            int a = delete(h, k);
            check(a == mdelete(k), "delete result"); del_n += a;
        }
        verify(h);
        check(has(h, k) == mhas(k), "search");
        int ht = h->lthread ? 0 : height(h->l);
        if (ht > maxh) maxh = ht;
    }
    printf("inserts=%d deletes=%d size=%d maxheight=%d\n", ins_n, del_n, mn, maxh);
    /* stackless traversal in both directions */
    unsigned fsum = 0, bsum = 0;
    int first = -1, last = -1;
    if (!h->lthread) {
        N *p = h->l;
        while (!p->lthread) p = p->l;
        first = p->k;
        for (; p != h; p = succ(p)) fsum = fsum * 31u + (unsigned)p->k;
        p = h->l;
        while (!p->rthread) p = p->r;
        last = p->k;
        for (; p != h; p = pred(p)) bsum = bsum * 31u + (unsigned)p->k;
    }
    printf("first=%d last=%d forward hash=%u backward hash=%u\n", first, last, fsum, bsum);
    /* delete all in ascending order: always exercises the leftmost node */
    int cnt = 0;
    while (mn) {
        int k = mod[0];
        check(delete(h, k), "drain"); mdelete(k); cnt++;
        verify(h);
    }
    printf("drained %d keys in ascending order\n", cnt);
    freeall(h);
    return 0;
}
