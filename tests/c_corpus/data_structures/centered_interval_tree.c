/*
 * title: Centered interval tree with stabbing and overlap queries
 * topic: data_structures
 * covers: centered interval tree, median center split, left-sorted and right-sorted lists, stabbing query, overlap query, static build
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

typedef struct {
    int lo, hi, id;
} Iv;

typedef struct CNode {
    int center;
    Iv *by_lo; /* intervals containing center, ascending lo */
    Iv *by_hi; /* the same intervals, descending hi */
    int n;
    struct CNode *left, *right;
} CNode;

static int cmp_lo(const void *a, const void *b) {
    const Iv *x = a, *y = b;
    if (x->lo != y->lo)
        return x->lo < y->lo ? -1 : 1;
    return x->id - y->id;
}

static int cmp_hi_desc(const void *a, const void *b) {
    const Iv *x = a, *y = b;
    if (x->hi != y->hi)
        return x->hi > y->hi ? -1 : 1;
    return x->id - y->id;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int nodes, depth_max;

static CNode *build(Iv *iv, int n, int depth) {
    if (n == 0)
        return NULL;
    if (depth > depth_max)
        depth_max = depth;
    /* center: median of all endpoints */
    int *pts = malloc(sizeof(int) * (size_t)(2 * n));
    check(pts != NULL, "alloc");
    for (int i = 0; i < n; i++) {
        pts[2 * i] = iv[i].lo;
        pts[2 * i + 1] = iv[i].hi;
    }
    qsort(pts, (size_t)(2 * n), sizeof(int), cmp_int);
    int center = pts[n];
    free(pts);
    CNode *c = calloc(1, sizeof *c);
    check(c != NULL, "alloc");
    nodes++;
    c->center = center;
    Iv *L = malloc(sizeof(Iv) * (size_t)n), *R = malloc(sizeof(Iv) * (size_t)n);
    Iv *M = malloc(sizeof(Iv) * (size_t)n);
    check(L && R && M, "alloc");
    int nl = 0, nr = 0, nm = 0;
    for (int i = 0; i < n; i++) {
        if (iv[i].hi < center)
            L[nl++] = iv[i];
        else if (iv[i].lo > center)
            R[nr++] = iv[i];
        else
            M[nm++] = iv[i];
    }
    check(nm > 0, "progress");
    c->n = nm;
    c->by_lo = malloc(sizeof(Iv) * (size_t)nm);
    c->by_hi = malloc(sizeof(Iv) * (size_t)nm);
    check(c->by_lo && c->by_hi, "alloc");
    memcpy(c->by_lo, M, sizeof(Iv) * (size_t)nm);
    memcpy(c->by_hi, M, sizeof(Iv) * (size_t)nm);
    qsort(c->by_lo, (size_t)nm, sizeof(Iv), cmp_lo);
    qsort(c->by_hi, (size_t)nm, sizeof(Iv), cmp_hi_desc);
    c->left = build(L, nl, depth + 1);
    c->right = build(R, nr, depth + 1);
    free(L);
    free(R);
    free(M);
    return c;
}

static void destroy(CNode *c) {
    if (!c)
        return;
    destroy(c->left);
    destroy(c->right);
    free(c->by_lo);
    free(c->by_hi);
    free(c);
}

static long scanned;

static void stab(const CNode *c, int x, int *out, int *cnt) {
    while (c) {
        if (x < c->center) {
            for (int i = 0; i < c->n && c->by_lo[i].lo <= x; i++) {
                out[(*cnt)++] = c->by_lo[i].id;
                scanned++;
            }
            c = c->left;
        } else if (x > c->center) {
            for (int i = 0; i < c->n && c->by_hi[i].hi >= x; i++) {
                out[(*cnt)++] = c->by_hi[i].id;
                scanned++;
            }
            c = c->right;
        } else {
            for (int i = 0; i < c->n; i++)
                out[(*cnt)++] = c->by_lo[i].id;
            return;
        }
    }
}

/* all intervals overlapping [a,b]: recurse on both sides when the query straddles the center */
static void overlap(const CNode *c, int a, int b, int *out, int *cnt) {
    if (!c)
        return;
    if (b < c->center) {
        for (int i = 0; i < c->n && c->by_lo[i].lo <= b; i++)
            out[(*cnt)++] = c->by_lo[i].id;
        overlap(c->left, a, b, out, cnt);
    } else if (a > c->center) {
        for (int i = 0; i < c->n && c->by_hi[i].hi >= a; i++)
            out[(*cnt)++] = c->by_hi[i].id;
        overlap(c->right, a, b, out, cnt);
    } else {
        for (int i = 0; i < c->n; i++)
            out[(*cnt)++] = c->by_lo[i].id;
        overlap(c->left, a, b, out, cnt);
        overlap(c->right, a, b, out, cnt);
    }
}

int main(void) {
    enum { NI = 600 };
    static Iv iv[NI];
    for (int i = 0; i < NI; i++) {
        iv[i].lo = (int)rnd(5000);
        iv[i].hi = iv[i].lo + (rnd(5) == 0 ? (int)rnd(1500) : (int)rnd(60));
        iv[i].id = i;
    }
    Iv *copy = malloc(sizeof iv);
    check(copy != NULL, "alloc");
    memcpy(copy, iv, sizeof iv);
    CNode *root = build(copy, NI, 0);
    free(copy);
    printf("intervals=%d nodes=%d depth=%d\n", NI, nodes, depth_max);
    static int got[NI * 2];
    long ssum = 0, osum = 0;
    for (int q = 0; q < 1500; q++) {
        int x = (int)rnd(6500);
        int cnt = 0;
        stab(root, x, got, &cnt);
        int want[NI], nw = 0;
        for (int i = 0; i < NI; i++)
            if (iv[i].lo <= x && x <= iv[i].hi)
                want[nw++] = i;
        qsort(got, (size_t)cnt, sizeof(int), cmp_int);
        check(cnt == nw, "stab count");
        for (int i = 0; i < nw; i++)
            check(got[i] == want[i], "stab ids");
        ssum += cnt;
        int a = (int)rnd(6000), b = a + (int)rnd(300);
        cnt = 0;
        overlap(root, a, b, got, &cnt);
        nw = 0;
        for (int i = 0; i < NI; i++)
            if (iv[i].lo <= b && a <= iv[i].hi)
                want[nw++] = i;
        qsort(got, (size_t)cnt, sizeof(int), cmp_int);
        check(cnt == nw, "overlap count");
        for (int i = 0; i < nw; i++)
            check(got[i] == want[i], "overlap ids");
        osum += cnt;
    }
    printf("stab results=%ld overlap results=%ld scanned=%ld\n", ssum, osum, scanned);
    destroy(root);
    return 0;
}
