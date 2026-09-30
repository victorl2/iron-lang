/*
 * title: Maximum subarray variants
 * topic: algorithms
 * covers: Kadane, index tracking, all-negative input, circular subarray, maximum product subarray, at-most-length constraint
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 999983u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

typedef struct {
    long sum;
    int l, r; /* inclusive */
} Span;

static Span kadane(const int *a, int n) {
    Span best = {a[0], 0, 0};
    long cur = a[0];
    int start = 0;
    for (int i = 1; i < n; i++) {
        if (cur < 0) {
            cur = a[i];
            start = i;
        } else
            cur += a[i];
        if (cur > best.sum) {
            best.sum = cur;
            best.l = start;
            best.r = i;
        }
    }
    return best;
}

static long kadane_min(const int *a, int n) {
    long best = a[0], cur = a[0];
    for (int i = 1; i < n; i++) {
        cur = cur > 0 ? a[i] : cur + a[i];
        if (cur < best)
            best = cur;
    }
    return best;
}

static long circular(const int *a, int n) {
    long total = 0;
    for (int i = 0; i < n; i++)
        total += a[i];
    long mx = kadane(a, n).sum, mn = kadane_min(a, n);
    if (mx < 0)
        return mx; /* every element negative: wrapping would be the empty set */
    return mx > total - mn ? mx : total - mn;
}

static long max_product(const int *a, int n) {
    long best = a[0], hi = a[0], lo = a[0];
    for (int i = 1; i < n; i++) {
        long x = a[i], c1 = hi * x, c2 = lo * x;
        hi = x;
        if (c1 > hi)
            hi = c1;
        if (c2 > hi)
            hi = c2;
        lo = x;
        if (c1 < lo)
            lo = c1;
        if (c2 < lo)
            lo = c2;
        if (hi > best)
            best = hi;
    }
    return best;
}

int main(void) {
    int a[64];
    for (int trial = 0; trial < 500; trial++) {
        int n = 1 + (int)(rnd() % 40);
        int neg_bias = (int)(rnd() % 3);
        for (int i = 0; i < n; i++) {
            a[i] = (int)(rnd() % 19) - 9 - neg_bias * 3;
            if (trial % 7 == 0)
                a[i] = -1 - (int)(rnd() % 5);
        }
        long bs = a[0], bc = a[0], bp = a[0];
        int m = n < 12 ? n : 12; /* keeps products inside long */
        for (int i = 0; i < n; i++) {
            long s = 0;
            for (int j = i; j < n; j++) {
                s += a[j];
                if (s > bs)
                    bs = s;
            }
        }
        for (int i = 0; i < m; i++) {
            long p = 1;
            for (int j = i; j < m; j++) {
                p *= a[j];
                if (p > bp)
                    bp = p;
            }
        }
        /* circular brute force over non-empty arcs of length <= n */
        for (int i = 0; i < n; i++) {
            long s = 0;
            for (int len = 1; len <= n; len++) {
                s += a[(i + len - 1) % n];
                if (s > bc)
                    bc = s;
            }
        }
        Span k = kadane(a, n);
        if (k.sum != bs)
            fail("kadane");
        long chk = 0;
        for (int i = k.l; i <= k.r; i++)
            chk += a[i];
        if (chk != k.sum)
            fail("span");
        if (circular(a, n) != bc)
            fail("circular");
        if (max_product(a, m) != bp)
            fail("product");
    }
    printf("500 random arrays verified\n");
    int d1[] = {-2, 1, -3, 4, -1, 2, 1, -5, 4};
    Span s = kadane(d1, 9);
    printf("classic: sum %ld over [%d..%d]\n", s.sum, s.l, s.r);
    int d2[] = {-5, -2, -8, -1, -9};
    s = kadane(d2, 5);
    printf("all negative: sum %ld at %d\n", s.sum, s.l);
    int d3[] = {5, -3, 5};
    printf("circular {5,-3,5}: %ld\n", circular(d3, 3));
    int d4[] = {2, 3, -2, 4};
    printf("max product {2,3,-2,4}: %ld\n", max_product(d4, 4));
    int d5[] = {-2, 3, -4};
    printf("max product {-2,3,-4}: %ld\n", max_product(d5, 3));
    return 0;
}
