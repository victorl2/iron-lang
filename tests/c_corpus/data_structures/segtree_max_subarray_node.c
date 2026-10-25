/*
 * title: Segment tree of maximum subarray nodes
 * topic: data_structures
 * covers: non-commutative monoid, prefix/suffix/best/sum nodes, recursive query merge order, Kadane cross-check
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
    long sum, pref, suf, best;
} Node;

#define MAXN 256
static Node tr[4 * MAXN];

static long max2(long a, long b) { return a > b ? a : b; }

static Node leaf(long v) {
    Node n = {v, v, v, v};
    return n;
}

/* order matters: left segment first */
static Node join(Node a, Node b) {
    Node n;
    n.sum = a.sum + b.sum;
    n.pref = max2(a.pref, a.sum + b.pref);
    n.suf = max2(b.suf, b.sum + a.suf);
    n.best = max2(max2(a.best, b.best), a.suf + b.pref);
    return n;
}

static void build(int o, int l, int r, const long *a) {
    if (l == r) {
        tr[o] = leaf(a[l]);
        return;
    }
    int m = (l + r) / 2;
    build(2 * o, l, m, a);
    build(2 * o + 1, m + 1, r, a);
    tr[o] = join(tr[2 * o], tr[2 * o + 1]);
}

static void update(int o, int l, int r, int i, long v) {
    if (l == r) {
        tr[o] = leaf(v);
        return;
    }
    int m = (l + r) / 2;
    if (i <= m)
        update(2 * o, l, m, i, v);
    else
        update(2 * o + 1, m + 1, r, i, v);
    tr[o] = join(tr[2 * o], tr[2 * o + 1]);
}

static Node query(int o, int l, int r, int ql, int qr) {
    if (ql <= l && r <= qr)
        return tr[o];
    int m = (l + r) / 2;
    if (qr <= m)
        return query(2 * o, l, m, ql, qr);
    if (ql > m)
        return query(2 * o + 1, m + 1, r, ql, qr);
    return join(query(2 * o, l, m, ql, qr), query(2 * o + 1, m + 1, r, ql, qr));
}

static long kadane(const long *a, int l, int r) {
    long best = a[l], cur = a[l];
    for (int i = l + 1; i <= r; i++) {
        cur = max2(a[i], cur + a[i]);
        best = max2(best, cur);
    }
    return best;
}

int main(void) {
    int n = 250;
    long a[MAXN];
    for (int i = 0; i < n; i++)
        a[i] = (long)rnd(41) - 20;
    build(1, 0, n - 1, a);
    long checksum = 0;
    int negative_only = 0;
    for (int step = 0; step < 5000; step++) {
        if (rnd(3) == 0) {
            int i = (int)rnd((unsigned)n);
            a[i] = (long)rnd(41) - 20;
            update(1, 0, n - 1, i, a[i]);
            continue;
        }
        int l = (int)rnd((unsigned)n), r = (int)rnd((unsigned)n);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        Node g = query(1, 0, n - 1, l, r);
        long want = kadane(a, l, r);
        check(g.best == want, "max subarray");
        long s = 0, bp = a[l], bs = a[r];
        for (int i = l; i <= r; i++) {
            s += a[i];
            bp = max2(bp, s);
        }
        s = 0;
        for (int i = r; i >= l; i--) {
            s += a[i];
            bs = max2(bs, s);
        }
        check(g.pref == bp && g.suf == bs, "prefix and suffix");
        checksum = (checksum * 257 + g.best + 3 * g.pref + 5 * g.suf) % 1000000007L;
        negative_only += g.best < 0;
    }
    printf("checksum=%ld all-negative ranges=%d\n", checksum, negative_only);
    Node all = query(1, 0, n - 1, 0, n - 1);
    printf("whole array: sum=%ld pref=%ld suf=%ld best=%ld\n", all.sum, all.pref, all.suf, all.best);
    long neg[5] = {-5, -2, -9, -1, -7};
    build(1, 0, 4, neg);
    check(tr[1].best == -1, "all negative best is largest element");
    printf("all-negative sample best=%ld\n", tr[1].best);
    return 0;
}
