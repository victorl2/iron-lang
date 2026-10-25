/*
 * title: Sparse vectors as sorted index/value pairs with merge kernels
 * topic: data_structures
 * covers: sparse vector, sorted pairs, two-pointer dot product, axpy merge, galloping intersection, cosine-style norms, top-k by magnitude, dense oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIM 5000

static unsigned long long rs = 0x5AA5EC7ULL * 0x9E3779ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int i; long v; } Ent;
typedef struct { Ent *e; int n; } SVec;

static SVec gen(int nnz, long dense[DIM]) {
    memset(dense, 0, DIM * sizeof(long));
    for (int k = 0; k < nnz; k++) { int i = (int)(rnd() % DIM); long v = (long)(rnd() % 41) - 20; dense[i] = v; }
    SVec s; s.n = 0; s.e = malloc((size_t)(nnz + 1) * sizeof(Ent));
    for (int i = 0; i < DIM; i++) if (dense[i]) { s.e[s.n].i = i; s.e[s.n].v = dense[i]; s.n++; }
    return s;
}
static long dot_merge(const SVec *a, const SVec *b, long *steps) {
    long s = 0; int i = 0, j = 0;
    while (i < a->n && j < b->n) {
        (*steps)++;
        if (a->e[i].i == b->e[j].i) { s += a->e[i].v * b->e[j].v; i++; j++; }
        else if (a->e[i].i < b->e[j].i) i++; else j++;
    }
    return s;
}
/* first position in e[from..n) with index >= key using exponential then binary search */
static int gallop(const Ent *e, int n, int from, int key, long *steps) {
    int step = 1, lo = from, hi = from;
    while (hi < n && e[hi].i < key) { lo = hi + 1; hi += step; step *= 2; (*steps)++; }
    if (hi > n) hi = n;
    while (lo < hi) { int m = (lo + hi) / 2; (*steps)++; if (e[m].i < key) lo = m + 1; else hi = m; }
    return lo;
}
static long dot_gallop(const SVec *a, const SVec *b, long *steps) {   /* a is much shorter than b */
    long s = 0; int j = 0;
    for (int i = 0; i < a->n && j < b->n; i++) {
        j = gallop(b->e, b->n, j, a->e[i].i, steps);
        if (j < b->n && b->e[j].i == a->e[i].i) s += a->e[i].v * b->e[j].v;
    }
    return s;
}
static SVec axpy(long alpha, const SVec *x, const SVec *y) {   /* alpha*x + y */
    SVec o; o.e = malloc((size_t)(x->n + y->n + 1) * sizeof(Ent)); o.n = 0;
    int i = 0, j = 0;
    while (i < x->n || j < y->n) {
        Ent t;
        if (j >= y->n || (i < x->n && x->e[i].i < y->e[j].i)) { t.i = x->e[i].i; t.v = alpha * x->e[i].v; i++; }
        else if (i >= x->n || y->e[j].i < x->e[i].i) { t.i = y->e[j].i; t.v = y->e[j].v; j++; }
        else { t.i = x->e[i].i; t.v = alpha * x->e[i].v + y->e[j].v; i++; j++; }
        if (t.v != 0) o.e[o.n++] = t;
    }
    return o;
}
static int cmp_mag(const void *a, const void *b) {
    const Ent *x = a, *y = b;
    long mx = x->v < 0 ? -x->v : x->v, my = y->v < 0 ? -y->v : y->v;
    if (mx != my) return mx > my ? -1 : 1;
    return (x->i > y->i) - (x->i < y->i);
}

int main(void) {
    static long da[DIM], db[DIM], dc[DIM], dr[DIM];
    printf("%-8s %-8s %10s %10s %12s %12s\n", "nnz(a)", "nnz(b)", "dot", "l1(axpy)", "merge steps", "gallop steps");
    int cfg[4][2] = { {20, 20}, {30, 2000}, {5, 4000}, {1500, 1500} };
    for (int t = 0; t < 4; t++) {
        SVec a = gen(cfg[t][0], da), b = gen(cfg[t][1], db);
        long ref = 0; for (int i = 0; i < DIM; i++) ref += da[i] * db[i];
        long ms = 0, gs = 0;
        long d1 = dot_merge(&a, &b, &ms), d2 = a.n <= b.n ? dot_gallop(&a, &b, &gs) : dot_gallop(&b, &a, &gs);
        check(d1 == ref && d2 == ref, "dot products agree with dense");
        long alpha = 3 - t;
        SVec c = axpy(alpha, &a, &b);
        for (int i = 0; i < DIM; i++) dc[i] = alpha * da[i] + db[i];
        memset(dr, 0, sizeof dr);
        long l1 = 0;
        for (int k = 0; k < c.n; k++) { check(k == 0 || c.e[k - 1].i < c.e[k].i, "axpy sorted"); dr[c.e[k].i] = c.e[k].v; l1 += c.e[k].v < 0 ? -c.e[k].v : c.e[k].v; }
        check(memcmp(dr, dc, sizeof dr) == 0, "axpy agrees with dense");
        printf("%-8d %-8d %10ld %10ld %12ld %12ld\n", a.n, b.n, d1, l1, ms, gs);
        if (t == 3) {
            /* top-3 by magnitude of the axpy result, ties by lower index */
            qsort(c.e, (size_t)c.n, sizeof(Ent), cmp_mag);
            printf("top by magnitude:");
            for (int k = 0; k < 3; k++) printf(" [%d]=%ld", c.e[k].i, c.e[k].v);
            printf("\n");
            long na = 0, nb = 0; for (int k = 0; k < a.n; k++) na += a.e[k].v * a.e[k].v; for (int k = 0; k < b.n; k++) nb += b.e[k].v * b.e[k].v;
            /* Cauchy-Schwarz: dot^2 <= |a|^2 |b|^2 */
            check(d1 * d1 <= na * nb, "Cauchy-Schwarz");
            printf("|a|^2=%ld |b|^2=%ld\n", na, nb);
        }
        free(a.e); free(b.e); free(c.e);
    }
    return 0;
}
