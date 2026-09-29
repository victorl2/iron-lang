/*
 * title: K-th smallest pair distance
 * topic: algorithms
 * covers: binary search on answer, two-pointer counting of pairs within distance, sorted values, duplicate distances
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 700007u;
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
static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

/* number of pairs i<j with a[j]-a[i] <= d, using a sliding left pointer */
static long pairs_within(const int *a, int n, int d) {
    long cnt = 0;
    int left = 0;
    for (int r = 0; r < n; r++) {
        while (a[r] - a[left] > d)
            left++;
        cnt += r - left;
    }
    return cnt;
}

static int kth_distance(const int *a, int n, long k) {
    int lo = 0, hi = a[n - 1] - a[0];
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (pairs_within(a, n, mid) >= k)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

int main(void) {
    int a[120];
    static int dist[120 * 120];
    for (int trial = 0; trial < 60; trial++) {
        int n = 2 + (int)(rnd() % 60);
        int range = 1 + (int)(rnd() % 500);
        for (int i = 0; i < n; i++)
            a[i] = (int)(rnd() % (unsigned)range);
        qsort(a, (size_t)n, sizeof(int), cmp_int);
        int m = 0;
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++)
                dist[m++] = a[j] - a[i];
        qsort(dist, (size_t)m, sizeof(int), cmp_int);
        for (int q = 0; q < 5; q++) {
            long k = 1 + (long)(rnd() % (unsigned)m);
            if (kth_distance(a, n, k) != dist[k - 1])
                fail("kth distance");
        }
        long k = m;
        if (kth_distance(a, n, 1) != dist[0] || kth_distance(a, n, k) != dist[m - 1])
            fail("extremes");
    }
    printf("60 random arrays verified\n");

    int d1[] = {1, 3, 1};
    int d2[] = {1, 6, 1};
    int d3[] = {62, 100, 4};
    qsort(d1, 3, sizeof(int), cmp_int);
    qsort(d2, 3, sizeof(int), cmp_int);
    qsort(d3, 3, sizeof(int), cmp_int);
    printf("[1,3,1] k=1: %d\n", kth_distance(d1, 3, 1));
    printf("[1,6,1] k=3: %d\n", kth_distance(d2, 3, 3));
    printf("[62,100,4] k=2: %d\n", kth_distance(d3, 3, 2));

    static int big[20000];
    for (int i = 0; i < 20000; i++)
        big[i] = (int)(rnd() % 1000000);
    qsort(big, 20000, sizeof(int), cmp_int);
    long total = 20000L * 19999 / 2;
    printf("n=20000 pairs=%ld\n", total);
    printf("median pair distance: %d\n", kth_distance(big, 20000, total / 2));
    printf("1%% pair distance: %d\n", kth_distance(big, 20000, total / 100));
    return 0;
}
