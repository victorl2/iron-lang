/*
 * title: Ball tree with k-nearest and radius queries
 * topic: data_structures
 * covers: ball tree, bounding spheres, widest-dimension median split, knn search, integer isqrt pruning, brute-force cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NP 2000
#define DIM 3
#define LEAF 8

typedef struct Ball {
    int c[DIM];
    int radius;              /* integer upper bound on the distance from c to any point inside */
    int lo, hi;              /* range in the permutation array */
    struct Ball *l, *r;
} Ball;

static int P[NP][DIM];
static int perm[NP];
static long visited, dist_evals;

static unsigned long long rs = 0x314159265358ULL * 13;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static long d2(const int *a, const int *b) {
    dist_evals++;
    long s = 0;
    for (int i = 0; i < DIM; i++) { long d = a[i] - b[i]; s += d * d; }
    return s;
}
static long isqrt_floor(long v) { long r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }

static int sort_dim;
static int cmp_perm(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    if (P[x][sort_dim] != P[y][sort_dim]) return P[x][sort_dim] < P[y][sort_dim] ? -1 : 1;
    return x < y ? -1 : x > y;
}
static Ball *build(int lo, int hi) {
    Ball *b = calloc(1, sizeof *b);
    b->lo = lo; b->hi = hi;
    long sum[DIM] = {0}; int mn[DIM], mx[DIM];
    for (int d = 0; d < DIM; d++) { mn[d] = 1 << 30; mx[d] = -(1 << 30); }
    for (int i = lo; i < hi; i++) for (int d = 0; d < DIM; d++) {
        int v = P[perm[i]][d]; sum[d] += v; if (v < mn[d]) mn[d] = v; if (v > mx[d]) mx[d] = v;
    }
    for (int d = 0; d < DIM; d++) b->c[d] = (int)(sum[d] / (hi - lo));
    long r2 = 0;
    for (int i = lo; i < hi; i++) { long v = d2(P[perm[i]], b->c); if (v > r2) r2 = v; }
    long r = isqrt_floor(r2); if (r * r < r2) r++; /* ceiling */
    b->radius = (int)r;
    if (hi - lo <= LEAF) return b;
    int wd = 0;
    for (int d = 1; d < DIM; d++) if (mx[d] - mn[d] > mx[wd] - mn[wd]) wd = d;
    sort_dim = wd;
    qsort(perm + lo, (size_t)(hi - lo), sizeof(int), cmp_perm);
    int mid = (lo + hi) / 2;
    b->l = build(lo, mid);
    b->r = build(mid, hi);
    return b;
}
static void destroy(Ball *b) { if (b) { destroy(b->l); destroy(b->r); free(b); } }

typedef struct { long d[8]; int id[8]; int n, k; } Best;
static int worse(long d1, int i1, long d2v, int i2) { return d1 != d2v ? d1 > d2v : i1 > i2; }
static long tau(const Best *b) { return b->n < b->k ? (1L << 60) : b->d[b->n - 1]; }
static void offer(Best *b, long d, int id) {
    if (b->n == b->k && !worse(b->d[b->n - 1], b->id[b->n - 1], d, id)) return;
    int i = b->n < b->k ? b->n++ : b->n - 1;
    while (i > 0 && worse(b->d[i - 1], b->id[i - 1], d, id)) { b->d[i] = b->d[i - 1]; b->id[i] = b->id[i - 1]; i--; }
    b->d[i] = d; b->id[i] = id;
}
/* lower bound on squared distance from q to anything inside the ball */
static long lower2(const Ball *b, const int *q) {
    long dq = isqrt_floor(d2(q, b->c)) - b->radius;
    return dq > 0 ? dq * dq : 0;
}
static void knn(const Ball *b, const int *q, Best *best) {
    visited++;
    if (lower2(b, q) > tau(best)) return;
    if (!b->l) {
        for (int i = b->lo; i < b->hi; i++) offer(best, d2(q, P[perm[i]]), perm[i]);
        return;
    }
    long dl = d2(q, b->l->c), dr = d2(q, b->r->c);
    if (dl <= dr) { knn(b->l, q, best); knn(b->r, q, best); }
    else { knn(b->r, q, best); knn(b->l, q, best); }
}
static int range_count(const Ball *b, const int *q, long r2) {
    visited++;
    if (lower2(b, q) > r2) return 0;
    if (!b->l) { int c = 0; for (int i = b->lo; i < b->hi; i++) if (d2(q, P[perm[i]]) <= r2) c++; return c; }
    return range_count(b->l, q, r2) + range_count(b->r, q, r2);
}
/* verify: every point lies within its ball, children nest inside parent range */
static int check_ball(const Ball *b) {
    int nodes = 1;
    for (int i = b->lo; i < b->hi; i++) {
        long v = d2(P[perm[i]], b->c);
        check(v <= (long)b->radius * b->radius, "point inside ball radius");
    }
    if (b->l) { check(b->l->lo == b->lo && b->r->hi == b->hi && b->l->hi == b->r->lo, "children partition"); nodes += check_ball(b->l) + check_ball(b->r); }
    return nodes;
}

int main(void) {
    /* clusters in a 1000^3 box */
    int centers[10][DIM];
    for (int i = 0; i < 10; i++) for (int d = 0; d < DIM; d++) centers[i][d] = (int)(rnd() % 1000);
    for (int i = 0; i < NP; i++) {
        int c = (int)(rnd() % 10);
        for (int d = 0; d < DIM; d++) { int v = centers[c][d] + (int)(rnd() % 80) - 40; P[i][d] = v; }
        perm[i] = i;
    }
    Ball *root = build(0, NP);
    int nodes = check_ball(root);
    printf("points %d, nodes %d, root radius %d, root center (%d,%d,%d)\n", NP, nodes, root->radius, root->c[0], root->c[1], root->c[2]);
    int ks[] = { 1, 4, 8 };
    for (int ki = 0; ki < 3; ki++) {
        int k = ks[ki];
        long vis = 0, evals = 0, sumd = 0;
        for (int q = 0; q < 150; q++) {
            int qp[DIM];
            int base = (int)(rnd() % NP);
            for (int d = 0; d < DIM; d++) qp[d] = (q % 3 == 0) ? (int)(rnd() % 1000) : P[base][d] + (int)(rnd() % 21) - 10;
            Best b; b.n = 0; b.k = k;
            visited = 0; dist_evals = 0;
            knn(root, qp, &b);
            vis += visited; evals += dist_evals;
            Best r; r.n = 0; r.k = k;
            for (int i = 0; i < NP; i++) offer(&r, d2(qp, P[i]), i);
            for (int i = 0; i < k; i++) { check(b.d[i] == r.d[i] && b.id[i] == r.id[i], "knn matches brute force"); sumd += b.d[i]; }
        }
        printf("k=%d: avg nodes visited %ld, avg distance evals %ld (brute %d), sum of squared distances %ld\n", k, vis / 150, evals / 150, NP, sumd);
    }
    for (int rr = 10; rr <= 80; rr *= 2) {
        long tot = 0;
        for (int q = 0; q < 100; q++) {
            int qp[DIM];
            int base = (int)(rnd() % NP);
            for (int d = 0; d < DIM; d++) qp[d] = P[base][d];
            visited = 0;
            int got = range_count(root, qp, (long)rr * rr);
            int want = 0;
            for (int i = 0; i < NP; i++) if (d2(qp, P[i]) <= (long)rr * rr) want++;
            check(got == want, "range count matches brute force");
            tot += got;
        }
        printf("radius %2d: total hits over 100 queries %ld\n", rr, tot);
    }
    destroy(root);
    return 0;
}
