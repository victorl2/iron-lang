/*
 * title: Vantage-point tree k-nearest search
 * topic: data_structures
 * covers: vp-tree, metric space, median split, k-nearest neighbours, range query, hamming metric, pruning statistics
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define NP 1500

typedef struct VP {
    int point;          /* index into pts */
    int mu;             /* median distance */
    struct VP *in, *out;
} VP;

static uint32_t pts[NP];
static int npts;
static long dcalls;

static uint64_t rs = 0x2718281828459ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* hamming distance between 32-bit fingerprints (a true metric) */
static int dist(uint32_t a, uint32_t b) {
    dcalls++;
    uint32_t x = a ^ b; int c = 0;
    while (x) { c += (int)(x & 1u); x >>= 1; }
    return c;
}
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static VP *build(int *idx, int n) {
    if (n == 0) return NULL;
    VP *v = calloc(1, sizeof *v);
    /* vantage point: pseudo-random choice, swap to front */
    int pick = (int)(rnd() % (unsigned)n);
    int t = idx[0]; idx[0] = idx[pick]; idx[pick] = t;
    v->point = idx[0];
    if (n == 1) return v;
    int ds[NP];
    for (int i = 1; i < n; i++) ds[i - 1] = dist(pts[idx[0]], pts[idx[i]]);
    int sorted[NP];
    memcpy(sorted, ds, sizeof(int) * (size_t)(n - 1));
    qsort(sorted, (size_t)(n - 1), sizeof(int), cmp_int);
    v->mu = sorted[(n - 1) / 2];
    int a[NP], b[NP], na = 0, nb = 0;
    for (int i = 1; i < n; i++) { if (ds[i - 1] <= v->mu) a[na++] = idx[i]; else b[nb++] = idx[i]; }
    v->in = build(a, na);
    v->out = build(b, nb);
    return v;
}
static void destroy(VP *v) { if (v) { destroy(v->in); destroy(v->out); free(v); } }

/* bounded max-heap of (dist, index) pairs, k small: sorted insertion array */
typedef struct { int d[16], id[16], n, k; } Best;
static int worse(int d1, int i1, int d2, int i2) { return d1 != d2 ? d1 > d2 : i1 > i2; } /* (d1,i1) is worse than (d2,i2) */
static int tau(const Best *b) { return b->n < b->k ? 1 << 30 : b->d[b->n - 1]; }
static void offer(Best *b, int d, int id) {
    if (b->n == b->k && !worse(b->d[b->n - 1], b->id[b->n - 1], d, id)) return;
    int i = b->n < b->k ? b->n++ : b->n - 1;
    while (i > 0 && worse(b->d[i - 1], b->id[i - 1], d, id)) { b->d[i] = b->d[i - 1]; b->id[i] = b->id[i - 1]; i--; }
    b->d[i] = d; b->id[i] = id;
}
static void knn(const VP *v, uint32_t q, Best *b) {
    if (!v) return;
    int d = dist(q, pts[v->point]);
    offer(b, d, v->point);
    if (!v->in && !v->out) return;
    /* ties at the same distance must still be visited: use <= on both sides of the bound */
    if (d <= v->mu) {
        knn(v->in, q, b);
        if (d + tau(b) >= v->mu + 1 || b->n < b->k) knn(v->out, q, b);
    } else {
        knn(v->out, q, b);
        if (d - tau(b) <= v->mu || b->n < b->k) knn(v->in, q, b);
    }
}
static int range(const VP *v, uint32_t q, int r, int *out, int cnt) {
    if (!v) return cnt;
    int d = dist(q, pts[v->point]);
    if (d <= r) out[cnt++] = v->point;
    if (d - r <= v->mu) cnt = range(v->in, q, r, out, cnt);
    if (d + r > v->mu) cnt = range(v->out, q, r, out, cnt);
    return cnt;
}
static int depth(const VP *v) { if (!v) return 0; int a = depth(v->in), b = depth(v->out); return 1 + (a > b ? a : b); }

int main(void) {
    /* clustered fingerprints: 12 centers, each perturbed by a few bit flips */
    uint32_t centers[12];
    for (int i = 0; i < 12; i++) centers[i] = ((uint32_t)rnd() << 16) ^ (uint32_t)rnd();
    for (int i = 0; i < NP; i++) {
        uint32_t x = centers[rnd() % 12];
        int flips = (int)(rnd() % 6);
        for (int f = 0; f < flips; f++) x ^= 1u << (rnd() % 32);
        pts[i] = x;
    }
    npts = NP;
    static int idx[NP];
    for (int i = 0; i < NP; i++) idx[i] = i;
    dcalls = 0;
    VP *root = build(idx, NP);
    printf("points %d, tree depth %d, build distance calls %ld\n", NP, depth(root), dcalls);
    int ks[] = { 1, 3, 8 };
    for (int ki = 0; ki < 3; ki++) {
        int k = ks[ki];
        long tc = 0; long sumd = 0;
        for (int q = 0; q < 100; q++) {
            uint32_t qp = (q % 2) ? pts[rnd() % NP] ^ (1u << (rnd() % 32)) : (uint32_t)rnd() * 2654435761u;
            Best b; b.n = 0; b.k = k;
            dcalls = 0;
            knn(root, qp, &b);
            tc += dcalls;
            Best r; r.n = 0; r.k = k;
            for (int i = 0; i < NP; i++) offer(&r, dist(qp, pts[i]), i);
            check(b.n == r.n, "k results");
            for (int i = 0; i < k; i++) { check(b.d[i] == r.d[i] && b.id[i] == r.id[i], "knn matches brute force incl ties"); sumd += b.d[i]; }
        }
        printf("k=%d: 100 queries, avg tree distance calls %ld (brute %d), sum of neighbour distances %ld\n", k, tc / 100, NP, sumd);
    }
    for (int r = 0; r <= 6; r += 3) {
        long total = 0, tc = 0;
        for (int q = 0; q < 50; q++) {
            uint32_t qp = pts[rnd() % NP] ^ (uint32_t)(rnd() & 0x101);
            int got[NP]; dcalls = 0;
            int ng = range(root, qp, r, got, 0);
            tc += dcalls;
            int want = 0;
            for (int i = 0; i < NP; i++) if (dist(qp, pts[i]) <= r) want++;
            check(ng == want, "range count vs brute");
            total += ng;
        }
        printf("range r=%d: 50 queries, %ld hits, avg tree calls %ld\n", r, total, tc / 50);
    }
    destroy(root);
    return 0;
}
