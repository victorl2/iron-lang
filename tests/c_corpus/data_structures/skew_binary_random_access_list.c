/*
 * title: Skew binary random-access list
 * topic: data_structures
 * covers: skew binary numbers, okasaki random-access list, forest of complete trees, persistent cons/head/tail, logarithmic lookup and update
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct T { int val; struct T *l, *r; } T;                 /* complete binary tree, preorder labelled */
typedef struct L { int size; T *tree; struct L *next; } L;        /* list of trees with sizes 2^k - 1 */

static void *pool[400000]; static int npool;
static void *al(size_t n) { void *p = calloc(1, n); pool[npool++] = p; return p; }

static unsigned long long rs = 0x5CE3BA5EULL * 12289;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static T *mkt(int v, T *l, T *r) { T *t = al(sizeof *t); t->val = v; t->l = l; t->r = r; return t; }
static L *mkl(int size, T *t, L *next) { L *l = al(sizeof *l); l->size = size; l->tree = t; l->next = next; return l; }
static int length(const L *l) { int n = 0; for (; l; l = l->next) n += l->size; return n; }

static L *cons(int v, L *xs) {
    if (xs && xs->next && xs->size == xs->next->size)
        return mkl(1 + xs->size + xs->next->size, mkt(v, xs->tree, xs->next->tree), xs->next->next);
    return mkl(1, mkt(v, NULL, NULL), xs);
}
static int head(const L *xs) { check(xs != NULL, "head of empty"); return xs->tree->val; }
static L *tail(const L *xs) {
    check(xs != NULL, "tail of empty");
    if (xs->size == 1) return xs->next;
    int h = xs->size / 2;
    return mkl(h, xs->tree->l, mkl(h, xs->tree->r, xs->next));
}
static int tree_lookup(const T *t, int size, int i) {
    while (i > 0) {
        int h = size / 2;
        if (i <= h) { t = t->l; i -= 1; } else { t = t->r; i -= 1 + h; }
        size = h;
    }
    return t->val;
}
static long steps;
static int lookup(const L *xs, int i) {
    while (xs) {
        steps++;
        if (i < xs->size) return tree_lookup(xs->tree, xs->size, i);
        i -= xs->size; xs = xs->next;
    }
    check(0, "index out of range");
    return 0;
}
static T *tree_update(const T *t, int size, int i, int v) {
    if (i == 0) return mkt(v, t->l, t->r);
    int h = size / 2;
    if (i <= h) return mkt(t->val, tree_update(t->l, h, i - 1, v), t->r);
    return mkt(t->val, t->l, tree_update(t->r, h, i - 1 - h, v));
}
static L *update(const L *xs, int i, int v) {
    check(xs != NULL, "update out of range");
    if (i < xs->size) return mkl(xs->size, tree_update(xs->tree, xs->size, i, v), xs->next);
    return mkl(xs->size, xs->tree, update(xs->next, i - xs->size, v));
}
static void repr(const L *xs, char *o) {
    o[0] = 0;
    for (; xs; xs = xs->next) { char t[16]; snprintf(t, sizeof t, "%d ", xs->size); strcat(o, t); }
}
static int canonical(const L *xs) {
    /* sizes are 2^k-1, non-decreasing, and only the first two may be equal */
    int prev = 0, first = 1;
    for (const L *p = xs; p; p = p->next) {
        int s = p->size + 1;
        if (s & (s - 1)) return 0;
        if (prev) {
            if (p->size < prev) return 0;
            if (p->size == prev && !first) return 0;
        }
        if (prev && p != xs->next) first = 0;
        prev = p->size;
    }
    return 1;
}
static void preorder(const T *t, int *o, int *n) { if (!t) return; o[(*n)++] = t->val; preorder(t->l, o, n); preorder(t->r, o, n); }
static void to_array(const L *xs, int *o) { int n = 0; for (; xs; xs = xs->next) preorder(xs->tree, o, &n); }

int main(void) {
    /* representation of lists of length 0..20 */
    printf("tree sizes by length:\n");
    L *xs = NULL;
    for (int n = 0; n <= 20; n++) {
        char buf[128]; repr(xs, buf);
        check(length(xs) == n && canonical(xs), "canonical skew binary form");
        printf("  n=%2d: %s\n", n, buf);
        xs = cons(n + 100, xs);
    }
    /* stress against an array model: persistent snapshots + random ops */
    static int model[3000]; int nm = 0;
    L *cur = NULL; L *snaps[8]; static int snapmodel[8][3000]; int snapn[8], nsnap = 0;
    long maxsteps = 0, conses = 0, tails = 0, updates = 0, lookups = 0;
    for (int step = 0; step < 5000; step++) {
        unsigned op = rnd() % 10;
        if (op < 4 && nm < 2900) { int v = (int)(rnd() % 1000000); cur = cons(v, cur); memmove(model + 1, model, sizeof(int) * (size_t)nm); model[0] = v; nm++; conses++; }
        else if (op < 6 && nm > 0) { check(head(cur) == model[0], "head"); cur = tail(cur); memmove(model, model + 1, sizeof(int) * (size_t)(nm - 1)); nm--; tails++; }
        else if (op < 8 && nm > 0) { int i = (int)(rnd() % (unsigned)nm), v = (int)(rnd() % 1000000); cur = update(cur, i, v); model[i] = v; updates++; }
        else if (nm > 0) {
            int i = (int)(rnd() % (unsigned)nm); steps = 0;
            check(lookup(cur, i) == model[i], "lookup");
            if (steps > maxsteps) maxsteps = steps;
            lookups++;
        }
        check(length(cur) == nm && canonical(cur), "length and canonical form");
        if (step % 600 == 300 && nsnap < 8) { snaps[nsnap] = cur; memcpy(snapmodel[nsnap], model, sizeof(int) * (size_t)nm); snapn[nsnap++] = nm; }
        if (step % 1000 == 999) { static int arr[3000]; to_array(cur, arr); check(memcmp(arr, model, sizeof(int) * (size_t)nm) == 0, "preorder flattening equals model"); }
    }
    for (int s = 0; s < nsnap; s++) {
        check(length(snaps[s]) == snapn[s], "snapshot length");
        for (int i = 0; i < snapn[s]; i += 7) check(lookup(snaps[s], i) == snapmodel[s][i], "old version intact");
    }
    char sh[512]; repr(cur, sh);
    printf("conses %ld tails %ld updates %ld lookups %ld (max list hops per lookup %ld), final length %d\n", conses, tails, updates, lookups, maxsteps, nm);
    printf("final tree sizes: %s\n", sh);
    printf("%d persistent snapshots verified, nodes allocated %d\n", nsnap, npool);
    for (int i = 0; i < npool; i++) free(pool[i]);
    return 0;
}
