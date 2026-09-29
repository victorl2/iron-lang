/*
 * title: Lazy segment tree: range add, range sum
 * topic: data_structures
 * covers: recursive segment tree, lazy propagation, push down, range add, range sum, brute force
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

#define MAXN 512
static long sum_[4 * MAXN];
static long lazy_[4 * MAXN];
static long a_[MAXN];
static long pushes = 0;

static void build(int o, int l, int r) {
    lazy_[o] = 0;
    if (l == r) {
        sum_[o] = a_[l];
        return;
    }
    int m = (l + r) / 2;
    build(2 * o, l, m);
    build(2 * o + 1, m + 1, r);
    sum_[o] = sum_[2 * o] + sum_[2 * o + 1];
}

static void apply(int o, int l, int r, long v) {
    sum_[o] += v * (r - l + 1);
    lazy_[o] += v;
}

static void push(int o, int l, int r) {
    if (lazy_[o] != 0) {
        int m = (l + r) / 2;
        apply(2 * o, l, m, lazy_[o]);
        apply(2 * o + 1, m + 1, r, lazy_[o]);
        lazy_[o] = 0;
        pushes++;
    }
}

static void update(int o, int l, int r, int ql, int qr, long v) {
    if (qr < l || r < ql)
        return;
    if (ql <= l && r <= qr) {
        apply(o, l, r, v);
        return;
    }
    push(o, l, r);
    int m = (l + r) / 2;
    update(2 * o, l, m, ql, qr, v);
    update(2 * o + 1, m + 1, r, ql, qr, v);
    sum_[o] = sum_[2 * o] + sum_[2 * o + 1];
}

static long query(int o, int l, int r, int ql, int qr) {
    if (qr < l || r < ql)
        return 0;
    if (ql <= l && r <= qr)
        return sum_[o];
    push(o, l, r);
    int m = (l + r) / 2;
    return query(2 * o, l, m, ql, qr) + query(2 * o + 1, m + 1, r, ql, qr);
}

static void run_size(int n, int ops, long *checksum, int *nq) {
    for (int i = 0; i < n; i++)
        a_[i] = (long)rnd(200) - 100;
    long *ref = malloc(sizeof(long) * (size_t)n);
    check(ref != NULL, "alloc");
    memcpy(ref, a_, sizeof(long) * (size_t)n);
    build(1, 0, n - 1);
    for (int step = 0; step < ops; step++) {
        int l = (int)rnd((unsigned)n);
        int r = (int)rnd((unsigned)n);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        if (rnd(2) == 0) {
            long v = (long)rnd(41) - 20;
            update(1, 0, n - 1, l, r, v);
            for (int i = l; i <= r; i++)
                ref[i] += v;
        } else {
            long want = 0;
            for (int i = l; i <= r; i++)
                want += ref[i];
            long got = query(1, 0, n - 1, l, r);
            check(got == want, "range sum");
            *checksum = (*checksum * 131 + got) % 998244353L;
            (*nq)++;
        }
    }
    for (int i = 0; i < n; i++)
        check(query(1, 0, n - 1, i, i) == ref[i], "point readback");
    free(ref);
}

int main(void) {
    int sizes[] = {1, 2, 3, 10, 97, 500};
    long checksum = 0;
    int nq = 0;
    for (int i = 0; i < 6; i++) {
        run_size(sizes[i], 1500, &checksum, &nq);
        printf("n=%d done, running checksum=%ld\n", sizes[i], checksum);
    }
    printf("queries=%d pushes=%ld\n", nq, pushes);
    /* whole-array add then whole sum touches only the root */
    for (int i = 0; i < 8; i++)
        a_[i] = i;
    build(1, 0, 7);
    update(1, 0, 7, 0, 7, 5);
    long p0 = pushes;
    check(query(1, 0, 7, 0, 7) == 28 + 40, "root only sum");
    check(pushes == p0, "no push needed for full range");
    printf("full-range sum=%ld\n", query(1, 0, 7, 0, 7));
    return 0;
}
