/*
 * title: Quickselect for k-th smallest and percentiles
 * topic: algorithms
 * covers: selection, quickselect, Hoare partition, nth_element semantics, partition invariant, cross-check by sorting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 7777777u;
static unsigned rng(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long cmps;

/* After return, a[k] is the k-th smallest; a[<k] <= a[k] <= a[>k]. */
static int quickselect(int *a, int n, int k) {
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        int p = a[lo + (int)(rng() % (unsigned)(hi - lo + 1))];
        int i = lo, j = hi;
        while (i <= j) {
            while (cmps++, a[i] < p)
                i++;
            while (cmps++, a[j] > p)
                j--;
            if (i <= j) {
                int t = a[i];
                a[i] = a[j];
                a[j] = t;
                i++;
                j--;
            }
        }
        if (k <= j)
            hi = j;
        else if (k >= i)
            lo = i;
        else
            break;
    }
    return a[k];
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    enum { N = 10001 };
    static int base[N], a[N], sorted[N];
    for (int i = 0; i < N; i++)
        base[i] = (int)(rng() % 100000);
    memcpy(sorted, base, sizeof sorted);
    qsort(sorted, N, sizeof(int), cmp_int);

    static const int ks[] = {0, 1, 100, 2500, 5000, 7500, 9999, 10000};
    for (int t = 0; t < 8; t++) {
        memcpy(a, base, sizeof a);
        cmps = 0;
        int v = quickselect(a, N, ks[t]);
        check(v == sorted[ks[t]], "matches sorted order");
        for (int i = 0; i < ks[t]; i++)
            check(a[i] <= v, "left side <=");
        for (int i = ks[t] + 1; i < N; i++)
            check(a[i] >= v, "right side >=");
        printf("k=%-5d value=%-6d comparisons=%ld\n", ks[t], v, cmps);
    }
    /* percentiles */
    static const int pct[] = {1, 10, 25, 50, 75, 90, 99};
    printf("percentiles:");
    for (int t = 0; t < 7; t++) {
        memcpy(a, base, sizeof a);
        int k = (int)((long)(N - 1) * pct[t] / 100);
        int v = quickselect(a, N, k);
        check(v == sorted[k], "percentile");
        printf(" p%d=%d", pct[t], v);
    }
    printf("\n");
    /* many duplicates */
    for (int i = 0; i < N; i++)
        a[i] = (int)(rng() % 5);
    memcpy(sorted, a, sizeof sorted);
    qsort(sorted, N, sizeof(int), cmp_int);
    cmps = 0;
    int v = quickselect(a, N, N / 2);
    check(v == sorted[N / 2], "duplicates median");
    printf("dup median=%d comparisons=%ld\n", v, cmps);
    return 0;
}
