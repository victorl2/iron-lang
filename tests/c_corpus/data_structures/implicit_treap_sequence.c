/*
 * title: Implicit treap with split, merge, lazy reversal and range add
 * topic: data_structures
 * covers: implicit treap, position-indexed split and merge, lazy propagation, range reversal, range add, range sum, cut and paste, rotate, array model
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

typedef struct N {
    long v, sum, add;
    unsigned pri;
    int size, rev;
    struct N *l, *r;
} N;

static int sz(N *t) { return t ? t->size : 0; }
static long sm(N *t) { return t ? t->sum : 0; }
static void apply_add(N *t, long a) {
    if (!t) return;
    t->v += a; t->sum += a * t->size; t->add += a;
}
static void push(N *t) {
    if (t->rev) {
        N *tmp = t->l; t->l = t->r; t->r = tmp;
        if (t->l) t->l->rev ^= 1;
        if (t->r) t->r->rev ^= 1;
        t->rev = 0;
    }
    if (t->add) {
        apply_add(t->l, t->add);
        apply_add(t->r, t->add);
        t->add = 0;
    }
}
static void upd(N *t) {
    t->size = 1 + sz(t->l) + sz(t->r);
    t->sum = t->v + sm(t->l) + sm(t->r);
}
static N *mk(long v) {
    N *n = xmalloc(sizeof *n);
    n->v = v; n->sum = v; n->add = 0; n->pri = rnd(); n->size = 1; n->rev = 0;
    n->l = n->r = NULL;
    return n;
}
/* first k elements go to *a, the rest to *b */
static void split(N *t, int k, N **a, N **b) {
    if (!t) { *a = *b = NULL; return; }
    push(t);
    if (sz(t->l) >= k) { split(t->l, k, a, &t->l); *b = t; }
    else { split(t->r, k - sz(t->l) - 1, &t->r, b); *a = t; }
    upd(t);
}
static N *merge(N *a, N *b) {
    if (!a) return b;
    if (!b) return a;
    if (a->pri > b->pri) { push(a); a->r = merge(a->r, b); upd(a); return a; }
    push(b); b->l = merge(a, b->l); upd(b);
    return b;
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

static int fn;
static long flat[MAXN];
static void flatten(N *t) {
    if (!t) return;
    push(t);
    flatten(t->l);
    flat[fn++] = t->v;
    flatten(t->r);
}
static int verify(N *t, unsigned maxpri) {
    if (!t) return 0;
    check(t->pri <= maxpri, "heap order");
    push(t);
    int c = 1 + verify(t->l, t->pri) + verify(t->r, t->pri);
    check(c == t->size, "size");
    check(t->sum == t->v + sm(t->l) + sm(t->r), "sum");
    return c;
}
static int depth(N *t) {
    if (!t) return 0;
    int a = depth(t->l), b = depth(t->r);
    return 1 + (a > b ? a : b);
}

static long ref[MAXN * 2];
static int rn;

int main(void) {
    N *root = NULL;
    long checks = 0, sums = 0;
    int maxd = 0;
    rn = 0;
    for (int op = 0; op < 5000; op++) {
        unsigned c = rnd() % 12;
        N *a, *b, *m;
        if (c < 5 && rn < 400) {
            int i = (int)(rnd() % (unsigned)(rn + 1));
            long v = (long)(rnd() % 100);
            split(root, i, &a, &b);
            root = merge(merge(a, mk(v)), b);
            memmove(ref + i + 1, ref + i, (size_t)(rn - i) * sizeof(long));
            ref[i] = v; rn++;
        } else if (c < 6 && rn > 0) {
            int l = (int)(rnd() % (unsigned)rn), r = l + 1 + (int)(rnd() % (unsigned)(rn - l < 4 ? rn - l : 4));
            split(root, l, &a, &b);
            split(b, r - l, &m, &b);
            freet(m);
            root = merge(a, b);
            memmove(ref + l, ref + r, (size_t)(rn - r) * sizeof(long));
            rn -= r - l;
        } else if (c < 8 && rn > 0) {                      /* reverse [l, r) */
            int l = (int)(rnd() % (unsigned)rn), r = l + 1 + (int)(rnd() % (unsigned)(rn - l));
            split(root, l, &a, &b);
            split(b, r - l, &m, &b);
            m->rev ^= 1;
            root = merge(merge(a, m), b);
            for (int i = l, j = r - 1; i < j; i++, j--) { long t = ref[i]; ref[i] = ref[j]; ref[j] = t; }
        } else if (c < 10 && rn > 0) {                      /* add x to [l, r) */
            int l = (int)(rnd() % (unsigned)rn), r = l + 1 + (int)(rnd() % (unsigned)(rn - l));
            long x = (long)(rnd() % 21) - 10;
            split(root, l, &a, &b);
            split(b, r - l, &m, &b);
            apply_add(m, x);
            root = merge(merge(a, m), b);
            for (int i = l; i < r; i++) ref[i] += x;
        } else if (c < 11 && rn > 1) {                     /* cut [l, r) and paste at position `to` of the rest */
            int l = (int)(rnd() % (unsigned)rn), r = l + 1 + (int)(rnd() % (unsigned)(rn - l));
            int rest = rn - (r - l), to = (int)(rnd() % (unsigned)(rest + 1));
            split(root, l, &a, &b);
            split(b, r - l, &m, &b);
            N *rest_t = merge(a, b), *x, *y;
            split(rest_t, to, &x, &y);
            root = merge(merge(x, m), y);
            long tmp[MAXN * 2];
            int tn = 0;
            for (int i = 0; i < l; i++) tmp[tn++] = ref[i];
            for (int i = r; i < rn; i++) tmp[tn++] = ref[i];
            long cut[MAXN];
            for (int i = l; i < r; i++) cut[i - l] = ref[i];
            int w = 0;
            for (int i = 0; i < to; i++) ref[w++] = tmp[i];
            for (int i = 0; i < r - l; i++) ref[w++] = cut[i];
            for (int i = to; i < tn; i++) ref[w++] = tmp[i];
        } else if (rn > 0) {                               /* rotate left by k: move a prefix to the back */
            int k = (int)(rnd() % (unsigned)rn);
            split(root, k, &a, &b);
            root = merge(b, a);
            long tmp[MAXN * 2];
            for (int i = 0; i < rn; i++) tmp[i] = ref[(i + k) % rn];
            memcpy(ref, tmp, sizeof(long) * (size_t)rn);
        }
        /* range sum query */
        if (rn > 0) {
            int l = (int)(rnd() % (unsigned)rn), r = l + 1 + (int)(rnd() % (unsigned)(rn - l));
            split(root, l, &a, &b);
            split(b, r - l, &m, &b);
            long want = 0;
            for (int i = l; i < r; i++) want += ref[i];
            check(sm(m) == want, "range sum");
            sums += want;
            root = merge(merge(a, m), b);
        }
        check(sz(root) == rn, "length");
        if (op % 25 == 0) {
            check(verify(root, 0xFFFFFFFFu) == rn, "structure");
            fn = 0; flatten(root);
            check(fn == rn && !memcmp(flat, ref, sizeof(long) * (size_t)rn), "contents");
            checks++;
            int d = depth(root);
            if (d > maxd) maxd = d;
        }
    }
    fn = 0; flatten(root);
    check(fn == rn && !memcmp(flat, ref, sizeof(long) * (size_t)rn), "final contents");
    printf("final length=%d, structural checks=%ld, max depth=%d, sum of queried ranges=%ld\n", rn, checks, maxd, sums);
    printf("first eight:");
    for (int i = 0; i < 8 && i < rn; i++) printf(" %ld", flat[i]);
    printf("\n");
    /* reversing the whole sequence twice is the identity */
    root->rev ^= 1;
    fn = 0; flatten(root);
    for (int i = 0; i < rn; i++) check(flat[i] == ref[rn - 1 - i], "full reverse");
    root->rev ^= 1;
    fn = 0; flatten(root);
    printf("full reverse checked; last=%ld first=%ld\n", flat[rn - 1], flat[0]);
    freet(root);
    return 0;
}
