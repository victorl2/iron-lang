/*
 * title: Peak finding by binary search on slopes
 * topic: algorithms
 * covers: binary search without sortedness, local maxima, mountain arrays, bitonic max, invariant reasoning
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 777u;
static unsigned rnd(void) {
    st = st * 1664525u + 1013904223u;
    return st >> 8;
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

/* any index i with a[i] >= neighbours (out of range counts as -inf) */
static int find_peak(const int *a, int n) {
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] < a[mid + 1])
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int is_peak(const int *a, int n, int i) {
    return (i == 0 || a[i - 1] <= a[i]) && (i == n - 1 || a[i + 1] <= a[i]);
}

/* summit of a strictly rising then strictly falling array */
static int mountain_top(const int *a, int n) {
    int lo = 1, hi = n - 2;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] > a[mid + 1])
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/* search a mountain array for target: peak, then ascending side, then descending side */
static int mountain_search(const int *a, int n, int t) {
    int p = mountain_top(a, n);
    int lo = 0, hi = p;
    while (lo <= hi) {
        int m = lo + (hi - lo) / 2;
        if (a[m] == t)
            return m;
        if (a[m] < t)
            lo = m + 1;
        else
            hi = m - 1;
    }
    lo = p + 1;
    hi = n - 1;
    while (lo <= hi) {
        int m = lo + (hi - lo) / 2;
        if (a[m] == t)
            return m;
        if (a[m] > t)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return -1;
}

int main(void) {
    int a[100];
    int peak_sum = 0;
    for (int trial = 0; trial < 500; trial++) {
        int n = 1 + (int)(rnd() % 100);
        for (int i = 0; i < n; i++)
            a[i] = (int)(rnd() % 50);
        int p = find_peak(a, n);
        if (!is_peak(a, n, p))
            fail("not a peak");
        peak_sum += p;
    }
    printf("500 random arrays, peak index checksum %d\n", peak_sum);

    int hits = 0;
    for (int n = 3; n <= 60; n++) {
        int m[60];
        int top = 1 + (int)(rnd() % (unsigned)(n - 2));
        for (int i = 0; i < n; i++)
            m[i] = i <= top ? i * 2 : (top * 2 - 1) - (i - top) * 2 + 2 * top;
        /* rising by 2 to index top, then falling by 2 */
        for (int i = 0; i < n; i++)
            m[i] = i <= top ? 2 * i : 2 * top - 1 - 2 * (i - top - 1) - 2;
        for (int i = 1; i < n; i++)
            if (i <= top ? m[i] <= m[i - 1] : m[i] >= m[i - 1])
                fail("bad mountain");
        if (mountain_top(m, n) != top)
            fail("mountain_top");
        for (int t = -30; t < 130; t++) {
            int want = -1;
            for (int i = 0; i < n; i++)
                if (m[i] == t) {
                    want = i;
                    break;
                }
            int got = mountain_search(m, n, t);
            if (got != want)
                fail("mountain_search");
            if (got >= 0)
                hits++;
        }
    }
    printf("mountain search hits: %d\n", hits);

    int demo[] = {1, 3, 8, 12, 9, 4, 2};
    printf("demo top=%d value=%d\n", mountain_top(demo, 7), demo[mountain_top(demo, 7)]);
    printf("find 4 -> %d, 8 -> %d, 5 -> %d\n", mountain_search(demo, 7, 4),
           mountain_search(demo, 7, 8), mountain_search(demo, 7, 5));
    return 0;
}
