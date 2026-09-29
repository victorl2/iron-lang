/*
 * title: Lower and upper bound binary search
 * topic: algorithms
 * covers: binary search, half-open intervals, lower_bound, upper_bound, insertion point, brute-force cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned long long rs = 0x9E3779B97F4A7C15ull;
static unsigned rnd(void) {
    rs ^= rs >> 12;
    rs ^= rs << 25;
    rs ^= rs >> 27;
    return (unsigned)((rs * 0x2545F4914F6CDD1Dull) >> 32);
}

static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

/* first index i in [0,n) with a[i] >= key, or n */
static int lower_bound(const int *a, int n, int key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* first index i with a[i] > key, or n */
static int upper_bound(const int *a, int n, int key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] <= key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    enum { N = 500 };
    int a[N];
    for (int i = 0; i < N; i++)
        a[i] = (int)(rnd() % 300);
    qsort(a, N, sizeof a[0], cmp_int);

    long sum_lb = 0, sum_ub = 0, present = 0, absent = 0;
    for (int key = -5; key < 306; key++) {
        int lb = lower_bound(a, N, key), ub = upper_bound(a, N, key);
        int blb = 0, bub = 0;
        for (int i = 0; i < N; i++) {
            if (a[i] < key)
                blb++;
            if (a[i] <= key)
                bub++;
        }
        if (lb != blb || ub != bub)
            fail("bound mismatch");
        if (ub > lb)
            present++;
        else
            absent++;
        sum_lb += lb;
        sum_ub += ub;
    }
    printf("keys present: %ld, absent: %ld\n", present, absent);
    printf("sum lower_bound: %ld\n", sum_lb);
    printf("sum upper_bound: %ld\n", sum_ub);

    /* count of a value range [lo, hi] via two bounds */
    int ranges[4][2] = {{0, 49}, {100, 199}, {250, 299}, {-10, 1000}};
    for (int r = 0; r < 4; r++) {
        int cnt = upper_bound(a, N, ranges[r][1]) - lower_bound(a, N, ranges[r][0]);
        int b = 0;
        for (int i = 0; i < N; i++)
            if (a[i] >= ranges[r][0] && a[i] <= ranges[r][1])
                b++;
        if (cnt != b)
            fail("range count");
        printf("count in [%d,%d] = %d\n", ranges[r][0], ranges[r][1], cnt);
    }
    /* empty array and single element */
    int one[1] = {7};
    printf("empty lb=%d ub=%d\n", lower_bound(one, 0, 3), upper_bound(one, 0, 3));
    printf("single lb(7)=%d ub(7)=%d lb(8)=%d ub(6)=%d\n", lower_bound(one, 1, 7),
           upper_bound(one, 1, 7), lower_bound(one, 1, 8), upper_bound(one, 1, 6));
    return 0;
}
