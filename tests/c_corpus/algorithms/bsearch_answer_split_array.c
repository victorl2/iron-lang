/*
 * title: Binary search on the answer: split array
 * topic: algorithms
 * covers: binary search on answer space, monotone feasibility predicate, greedy check, ship capacity, painters partition
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned long long rs = 0xDEADBEEFCAFEull;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 16);
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

/* can we cut a[0..n) into at most k contiguous parts each with sum <= cap? */
static int feasible(const int *a, int n, int k, long cap) {
    int parts = 1;
    long cur = 0;
    for (int i = 0; i < n; i++) {
        if (a[i] > cap)
            return 0;
        if (cur + a[i] > cap) {
            parts++;
            cur = 0;
        }
        cur += a[i];
    }
    return parts <= k;
}

static long min_largest_sum(const int *a, int n, int k) {
    long lo = 0, hi = 0;
    for (int i = 0; i < n; i++) {
        if (a[i] > lo)
            lo = a[i];
        hi += a[i];
    }
    while (lo < hi) {
        long mid = lo + (hi - lo) / 2;
        if (feasible(a, n, k, mid))
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/* O(k n^2) DP for cross-checking */
static long dp_min_largest(const int *a, int n, int k) {
    long pre[64] = {0};
    for (int i = 0; i < n; i++)
        pre[i + 1] = pre[i] + a[i];
    static long dp[9][64];
    for (int i = 0; i <= n; i++)
        dp[1][i] = pre[i];
    for (int j = 2; j <= k; j++)
        for (int i = 0; i <= n; i++) {
            long best = -1;
            for (int t = 0; t <= i; t++) {
                long left = dp[j - 1][t], right = pre[i] - pre[t];
                long v = left > right ? left : right;
                if (best < 0 || v < best)
                    best = v;
            }
            dp[j][i] = best;
        }
    return dp[k][n];
}

int main(void) {
    int a[64];
    for (int trial = 0; trial < 200; trial++) {
        int n = 1 + (int)(rnd() % 40);
        int k = 1 + (int)(rnd() % 8);
        for (int i = 0; i < n; i++)
            a[i] = 1 + (int)(rnd() % 50);
        if (min_largest_sum(a, n, k) != dp_min_largest(a, n, k))
            fail("dp mismatch");
    }
    printf("200 random cases match the DP\n");

    int loads[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    for (int days = 1; days <= 6; days++)
        printf("ship 1..10 in %d days: capacity %ld\n", days, min_largest_sum(loads, 10, days));

    int paint[] = {10, 20, 30, 40};
    printf("painters 2: %ld, 3: %ld\n", min_largest_sum(paint, 4, 2), min_largest_sum(paint, 4, 3));

    int big[64];
    long total = 0;
    for (int i = 0; i < 64; i++) {
        big[i] = 1 + (int)(rnd() % 1000);
        total += big[i];
    }
    printf("64 items total=%ld\n", total);
    for (int k = 1; k <= 64; k *= 2) {
        long c = min_largest_sum(big, 64, k);
        if (!feasible(big, 64, k, c) || (c > 0 && feasible(big, 64, k, c - 1)))
            fail("tightness");
        printf("k=%d -> %ld\n", k, c);
    }
    return 0;
}
