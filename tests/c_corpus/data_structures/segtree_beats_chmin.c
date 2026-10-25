/*
 * title: Segment tree beats: range chmin with sum and max
 * topic: data_structures
 * covers: segment tree beats, maximum and strict second maximum, max count, tag clamp, amortized chmin, brute force
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

#define MAXN 300
#define NEG (-1000000000L)

typedef struct {
    long mx, se, sum;
    int cnt;
} Node;

static Node t[4 * MAXN];
static long descents;

static void pull(int o) {
    Node *a = &t[2 * o], *b = &t[2 * o + 1];
    t[o].sum = a->sum + b->sum;
    if (a->mx > b->mx) {
        t[o].mx = a->mx;
        t[o].cnt = a->cnt;
        t[o].se = a->se > b->mx ? a->se : b->mx;
    } else if (a->mx < b->mx) {
        t[o].mx = b->mx;
        t[o].cnt = b->cnt;
        t[o].se = b->se > a->mx ? b->se : a->mx;
    } else {
        t[o].mx = a->mx;
        t[o].cnt = a->cnt + b->cnt;
        t[o].se = a->se > b->se ? a->se : b->se;
    }
}

/* lower every maximum of the node to x, requires se < x < mx */
static void clamp(int o, long x) {
    if (x < t[o].mx) {
        t[o].sum -= (t[o].mx - x) * t[o].cnt;
        t[o].mx = x;
    }
}

static void push(int o) {
    clamp(2 * o, t[o].mx);
    clamp(2 * o + 1, t[o].mx);
}

static void build(int o, int l, int r, const long *a) {
    if (l == r) {
        t[o].mx = t[o].sum = a[l];
        t[o].se = NEG;
        t[o].cnt = 1;
        return;
    }
    int m = (l + r) / 2;
    build(2 * o, l, m, a);
    build(2 * o + 1, m + 1, r, a);
    pull(o);
}

static void chmin(int o, int l, int r, int ql, int qr, long x) {
    if (qr < l || r < ql || t[o].mx <= x)
        return;
    if (ql <= l && r <= qr && t[o].se < x) {
        clamp(o, x);
        return;
    }
    descents++;
    push(o);
    int m = (l + r) / 2;
    chmin(2 * o, l, m, ql, qr, x);
    chmin(2 * o + 1, m + 1, r, ql, qr, x);
    pull(o);
}

static long qsum(int o, int l, int r, int ql, int qr) {
    if (qr < l || r < ql)
        return 0;
    if (ql <= l && r <= qr)
        return t[o].sum;
    push(o);
    int m = (l + r) / 2;
    return qsum(2 * o, l, m, ql, qr) + qsum(2 * o + 1, m + 1, r, ql, qr);
}

static long qmax(int o, int l, int r, int ql, int qr) {
    if (qr < l || r < ql)
        return NEG;
    if (ql <= l && r <= qr)
        return t[o].mx;
    push(o);
    int m = (l + r) / 2;
    long a = qmax(2 * o, l, m, ql, qr), b = qmax(2 * o + 1, m + 1, r, ql, qr);
    return a > b ? a : b;
}

int main(void) {
    int n = 257;
    long a[MAXN];
    for (int i = 0; i < n; i++)
        a[i] = (long)rnd(100000);
    build(1, 0, n - 1, a);
    long ssum = 0, msum = 0;
    int nc = 0, nq = 0;
    for (int step = 0; step < 6000; step++) {
        int l = (int)rnd((unsigned)n), r = (int)rnd((unsigned)n);
        if (l > r) {
            int x = l;
            l = r;
            r = x;
        }
        unsigned op = rnd(5);
        if (op < 2) {
            long x = (long)rnd(100000);
            chmin(1, 0, n - 1, l, r, x);
            for (int i = l; i <= r; i++)
                if (a[i] > x)
                    a[i] = x;
            nc++;
        } else if (op == 2 && rnd(4) == 0) {
            /* occasionally rebuild with fresh large values so chmin keeps having work */
            for (int i = l; i <= r; i++)
                a[i] = (long)rnd(100000);
            build(1, 0, n - 1, a);
        } else {
            long ws = 0, wm = a[l];
            for (int i = l; i <= r; i++) {
                ws += a[i];
                if (a[i] > wm)
                    wm = a[i];
            }
            check(qsum(1, 0, n - 1, l, r) == ws, "range sum");
            check(qmax(1, 0, n - 1, l, r) == wm, "range max");
            ssum += ws % 100003;
            msum += wm;
            nq++;
        }
    }
    printf("chmin ops=%d queries=%d\n", nc, nq);
    printf("sum digest=%ld max digest=%ld descents=%ld\n", ssum, msum, descents);
    printf("root: max=%ld second=%ld count=%d sum=%ld\n", t[1].mx, t[1].se, t[1].cnt, t[1].sum);
    return 0;
}
