/*
 * title: Two-dimensional Fenwick tree
 * topic: data_structures
 * covers: 2D binary indexed tree, rectangle sum by inclusion-exclusion, rectangle add with point query, non-square grids
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
    int h, w;
    long *t; /* (h+1) x (w+1), 1-based */
} F2;

static void f2_init(F2 *f, int h, int w) {
    f->h = h;
    f->w = w;
    f->t = calloc((size_t)(h + 1) * (size_t)(w + 1), sizeof(long));
    check(f->t != NULL, "alloc");
}

#define AT(f, i, j) ((f)->t[(size_t)(i) * (size_t)((f)->w + 1) + (size_t)(j)])

static void f2_add(F2 *f, int r, int c, long v) { /* 1-based */
    for (int i = r; i <= f->h; i += i & -i)
        for (int j = c; j <= f->w; j += j & -j)
            AT(f, i, j) += v;
}

static long f2_prefix(const F2 *f, int r, int c) {
    long s = 0;
    for (int i = r; i > 0; i -= i & -i)
        for (int j = c; j > 0; j -= j & -j)
            s += AT(f, i, j);
    return s;
}

static long f2_rect(const F2 *f, int r1, int c1, int r2, int c2) {
    return f2_prefix(f, r2, c2) - f2_prefix(f, r1 - 1, c2) - f2_prefix(f, r2, c1 - 1) +
           f2_prefix(f, r1 - 1, c1 - 1);
}

/* rectangle add + point query: store the 2D difference array */
static void f2_rect_add(F2 *f, int r1, int c1, int r2, int c2, long v) {
    f2_add(f, r1, c1, v);
    if (c2 + 1 <= f->w)
        f2_add(f, r1, c2 + 1, -v);
    if (r2 + 1 <= f->h)
        f2_add(f, r2 + 1, c1, -v);
    if (r2 + 1 <= f->h && c2 + 1 <= f->w)
        f2_add(f, r2 + 1, c2 + 1, v);
}

int main(void) {
    int dims[][2] = {{1, 1}, {1, 7}, {6, 1}, {8, 8}, {13, 29}, {40, 25}};
    for (int di = 0; di < 6; di++) {
        int h = dims[di][0], w = dims[di][1];
        F2 f, d;
        f2_init(&f, h, w);
        f2_init(&d, h, w);
        long *g = calloc((size_t)(h + 1) * (size_t)(w + 1), sizeof(long));
        long *gd = calloc((size_t)(h + 1) * (size_t)(w + 1), sizeof(long));
        check(g && gd, "alloc");
        long digest = 0;
        int nq = 0, nr = 0;
        for (int step = 0; step < 1500; step++) {
            int r1 = 1 + (int)rnd((unsigned)h), r2 = 1 + (int)rnd((unsigned)h);
            int c1 = 1 + (int)rnd((unsigned)w), c2 = 1 + (int)rnd((unsigned)w);
            if (r1 > r2) {
                int t = r1;
                r1 = r2;
                r2 = t;
            }
            if (c1 > c2) {
                int t = c1;
                c1 = c2;
                c2 = t;
            }
            unsigned op = rnd(4);
            if (op == 0) {
                long v = (long)rnd(201) - 100;
                f2_add(&f, r1, c1, v);
                g[(size_t)r1 * (size_t)(w + 1) + (size_t)c1] += v;
            } else if (op == 1) {
                long want = 0;
                for (int i = r1; i <= r2; i++)
                    for (int j = c1; j <= c2; j++)
                        want += g[(size_t)i * (size_t)(w + 1) + (size_t)j];
                long got = f2_rect(&f, r1, c1, r2, c2);
                check(got == want, "rectangle sum");
                digest = (digest * 13 + got) % 1000000007L;
                nq++;
            } else if (op == 2) {
                long v = (long)rnd(21) - 10;
                f2_rect_add(&d, r1, c1, r2, c2, v);
                for (int i = r1; i <= r2; i++)
                    for (int j = c1; j <= c2; j++)
                        gd[(size_t)i * (size_t)(w + 1) + (size_t)j] += v;
                nr++;
            } else {
                check(f2_prefix(&d, r1, c1) == gd[(size_t)r1 * (size_t)(w + 1) + (size_t)c1],
                      "point after rectangle add");
            }
        }
        printf("%dx%d sum-queries=%d rect-adds=%d digest=%ld whole=%ld\n", h, w, nq, nr, digest,
               f2_rect(&f, 1, 1, h, w));
        free(g);
        free(gd);
        free(f.t);
        free(d.t);
    }
    return 0;
}
