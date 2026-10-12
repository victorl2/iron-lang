/*
 * title: Fenwick pair for range add and range sum
 * topic: data_structures
 * covers: two binary indexed trees, difference array algebra, range add, range sum, point query, brute force
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
    int n;
    long *b1, *b2; /* prefix(i) = sum(b1, i) * i - sum(b2, i) over the difference array */
} RangeFenwick;

static void rf_init(RangeFenwick *f, int n) {
    f->n = n;
    f->b1 = calloc((size_t)n + 2, sizeof(long));
    f->b2 = calloc((size_t)n + 2, sizeof(long));
    check(f->b1 && f->b2, "alloc");
}

static void rf_free(RangeFenwick *f) {
    free(f->b1);
    free(f->b2);
}

static void point(long *t, int n, int i, long v) {
    for (; i <= n; i += i & -i)
        t[i] += v;
}

static long pref(const long *t, int i) {
    long s = 0;
    for (; i > 0; i -= i & -i)
        s += t[i];
    return s;
}

/* add v to positions [l, r], 1-based inclusive */
static void rf_add(RangeFenwick *f, int l, int r, long v) {
    point(f->b1, f->n, l, v);
    point(f->b1, f->n, r + 1, -v);
    point(f->b2, f->n, l, v * (l - 1));
    point(f->b2, f->n, r + 1, -v * r);
}

static long rf_prefix(const RangeFenwick *f, int i) { return pref(f->b1, i) * i - pref(f->b2, i); }

static long rf_sum(const RangeFenwick *f, int l, int r) { return rf_prefix(f, r) - rf_prefix(f, l - 1); }

static long rf_get(const RangeFenwick *f, int i) { return pref(f->b1, i); } /* only valid if base is zero */

int main(void) {
    int sizes[] = {1, 2, 9, 64, 333};
    for (int si = 0; si < 5; si++) {
        int n = sizes[si];
        RangeFenwick f;
        rf_init(&f, n);
        long *ref = calloc((size_t)n + 2, sizeof(long));
        check(ref != NULL, "alloc");
        long digest = 0;
        int nadd = 0, nq = 0;
        for (int step = 0; step < 2500; step++) {
            int l = 1 + (int)rnd((unsigned)n), r = 1 + (int)rnd((unsigned)n);
            if (l > r) {
                int t = l;
                l = r;
                r = t;
            }
            unsigned op = rnd(3);
            if (op == 0) {
                long v = (long)rnd(2001) - 1000;
                rf_add(&f, l, r, v);
                for (int i = l; i <= r; i++)
                    ref[i] += v;
                nadd++;
            } else if (op == 1) {
                long want = 0;
                for (int i = l; i <= r; i++)
                    want += ref[i];
                long got = rf_sum(&f, l, r);
                check(got == want, "range sum");
                digest = (digest * 17 + got) % 1000000007L;
                nq++;
            } else {
                check(rf_get(&f, l) == ref[l], "point query");
            }
        }
        long total = 0;
        for (int i = 1; i <= n; i++)
            total += ref[i];
        check(rf_sum(&f, 1, n) == total, "total");
        printf("n=%-4d adds=%d queries=%d digest=%ld total=%ld\n", n, nadd, nq, digest, total);
        free(ref);
        rf_free(&f);
    }
    /* worked example from the derivation */
    RangeFenwick e;
    rf_init(&e, 8);
    rf_add(&e, 2, 5, 3);
    rf_add(&e, 4, 8, 2);
    long row[8];
    for (int i = 1; i <= 8; i++)
        row[i - 1] = rf_get(&e, i);
    printf("example:");
    for (int i = 0; i < 8; i++)
        printf(" %ld", row[i]);
    printf(" | sum[3..6]=%ld\n", rf_sum(&e, 3, 6));
    check(rf_sum(&e, 3, 6) == 3 + 5 + 5 + 2, "example sum");
    rf_free(&e);
    return 0;
}
