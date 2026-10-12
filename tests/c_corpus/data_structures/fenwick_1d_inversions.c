/*
 * title: Fenwick tree with linear build, lower bound and inversion counting
 * topic: data_structures
 * covers: binary indexed tree, O(n) construction, prefix sums, lower_bound search, inversion count, adjacent swaps distance
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
    long *t; /* 1-based */
} Fenwick;

static void fw_init(Fenwick *f, int n) {
    f->n = n;
    f->t = calloc((size_t)n + 1, sizeof(long));
    check(f->t != NULL, "alloc");
}

/* linear-time build from an array */
static void fw_build(Fenwick *f, const long *a, int n) {
    fw_init(f, n);
    for (int i = 1; i <= n; i++) {
        f->t[i] += a[i - 1];
        int j = i + (i & -i);
        if (j <= n)
            f->t[j] += f->t[i];
    }
}

static void fw_add(Fenwick *f, int i, long v) { /* i is 0-based */
    for (i++; i <= f->n; i += i & -i)
        f->t[i] += v;
}

static long fw_prefix(const Fenwick *f, int i) { /* sum of [0, i) */
    long s = 0;
    for (; i > 0; i -= i & -i)
        s += f->t[i];
    return s;
}

static long fw_range(const Fenwick *f, int l, int r) { return fw_prefix(f, r) - fw_prefix(f, l); }

/* smallest i (0-based) with prefix(i+1) >= target, for non-negative entries; n if none */
static int fw_lower_bound(const Fenwick *f, long target) {
    int pos = 0, step = 1;
    while (step * 2 <= f->n)
        step *= 2;
    for (; step > 0; step >>= 1)
        if (pos + step <= f->n && f->t[pos + step] < target) {
            pos += step;
            target -= f->t[pos];
        }
    return pos; /* 0-based index of the answer */
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void) {
    int n = 500;
    long a[500];
    for (int i = 0; i < n; i++)
        a[i] = (long)rnd(50);
    Fenwick f, g;
    fw_build(&f, a, n);
    fw_init(&g, n);
    for (int i = 0; i < n; i++)
        fw_add(&g, i, a[i]);
    for (int i = 1; i <= n; i++)
        check(f.t[i] == g.t[i], "linear build equals repeated add");
    long dsum = 0, lb = 0;
    for (int step = 0; step < 5000; step++) {
        if (rnd(3) == 0) {
            int i = (int)rnd((unsigned)n);
            long d = (long)rnd(21) - 10;
            if (a[i] + d < 0)
                d = -a[i];
            a[i] += d;
            fw_add(&f, i, d);
            continue;
        }
        int l = (int)rnd((unsigned)n + 1), r = (int)rnd((unsigned)n + 1);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        long want = 0;
        for (int i = l; i < r; i++)
            want += a[i];
        check(fw_range(&f, l, r) == want, "range sum");
        dsum += want;
        long total = fw_prefix(&f, n);
        long target = 1 + (long)rnd((unsigned)total);
        int idx = fw_lower_bound(&f, target);
        long run = 0;
        int want_idx = n;
        for (int i = 0; i < n; i++) {
            run += a[i];
            if (run >= target) {
                want_idx = i;
                break;
            }
        }
        check(idx == want_idx, "lower bound");
        lb += idx;
    }
    printf("range digest=%ld lower_bound digest=%ld\n", dsum, lb);
    free(f.t);
    free(g.t);

    /* inversion counting: rank-compress then count with the tree */
    int m = 1000;
    int p[1000], sorted[1000];
    for (int i = 0; i < m; i++)
        p[i] = (int)rnd(300); /* duplicates on purpose */
    memcpy(sorted, p, sizeof p);
    qsort(sorted, (size_t)m, sizeof(int), cmp_int);
    int u = 0;
    for (int i = 0; i < m; i++)
        if (i == 0 || sorted[i] != sorted[i - 1])
            sorted[u++] = sorted[i];
    Fenwick c;
    fw_init(&c, u);
    long inv = 0;
    for (int i = 0; i < m; i++) {
        int lo = 0, hi = u - 1;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (sorted[mid] < p[i])
                lo = mid + 1;
            else
                hi = mid;
        }
        inv += fw_prefix(&c, u) - fw_prefix(&c, lo + 1); /* strictly greater seen earlier */
        fw_add(&c, lo, 1);
    }
    long brute = 0;
    for (int i = 0; i < m; i++)
        for (int j = i + 1; j < m; j++)
            brute += p[i] > p[j];
    check(inv == brute, "inversion count");
    printf("array of %d values, %d distinct: inversions=%ld\n", m, u, inv);
    free(c.t);

    /* a permutation's inversions equal the minimum adjacent swaps to sort it */
    int perm[300];
    for (int i = 0; i < 300; i++)
        perm[i] = i;
    for (int i = 299; i > 0; i--) {
        int j = (int)rnd((unsigned)i + 1);
        int t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    Fenwick pf;
    fw_init(&pf, 300);
    long swaps = 0;
    for (int i = 0; i < 300; i++) {
        swaps += i - fw_prefix(&pf, perm[i] + 1);
        fw_add(&pf, perm[i], 1);
    }
    long bubble = 0;
    int w[300];
    memcpy(w, perm, sizeof w);
    for (int i = 0; i < 300; i++)
        for (int j = 0; j + 1 < 300 - i; j++)
            if (w[j] > w[j + 1]) {
                int t = w[j];
                w[j] = w[j + 1];
                w[j + 1] = t;
                bubble++;
            }
    check(swaps == bubble, "adjacent swap distance");
    printf("permutation of 300: adjacent swaps=%ld\n", swaps);
    free(pf.t);
    return 0;
}
