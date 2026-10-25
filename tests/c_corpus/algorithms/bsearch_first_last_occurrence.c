/*
 * title: First and last occurrence with duplicates
 * topic: algorithms
 * covers: binary search variants, leftmost/rightmost match, run-length of duplicates, sentinel results
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 2463534242u;
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

/* Leftmost index with a[i]==k, else -1; keeps searching left after a hit. */
static int first_occ(const int *a, int n, int k) {
    int lo = 0, hi = n - 1, res = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] == k) {
            res = mid;
            hi = mid - 1;
        } else if (a[mid] < k)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return res;
}

static int last_occ(const int *a, int n, int k) {
    int lo = 0, hi = n - 1, res = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] == k) {
            res = mid;
            lo = mid + 1;
        } else if (a[mid] < k)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return res;
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    enum { N = 400, V = 120 };
    int a[N];
    for (int i = 0; i < N; i++) {
        /* skewed distribution creates long runs */
        unsigned r = rnd() % 100;
        a[i] = (r < 30) ? 17 : (r < 45) ? 63 : (int)(rnd() % V);
    }
    qsort(a, N, sizeof a[0], cmp_int);

    int longest_run = 0, longest_val = -1, distinct = 0;
    long total = 0;
    for (int k = 0; k < V; k++) {
        int f = first_occ(a, N, k), l = last_occ(a, N, k);
        int bf = -1, bl = -1;
        for (int i = 0; i < N; i++)
            if (a[i] == k) {
                if (bf < 0)
                    bf = i;
                bl = i;
            }
        if (f != bf || l != bl)
            fail("occurrence mismatch");
        if (f >= 0) {
            int run = l - f + 1;
            distinct++;
            total += run;
            if (run > longest_run) {
                longest_run = run;
                longest_val = k;
            }
        }
    }
    if (total != N)
        fail("run totals");
    printf("distinct values: %d\n", distinct);
    printf("longest run: value %d x %d\n", longest_val, longest_run);
    printf("17 at [%d,%d]\n", first_occ(a, N, 17), last_occ(a, N, 17));
    printf("63 at [%d,%d]\n", first_occ(a, N, 63), last_occ(a, N, 63));
    printf("missing 500: [%d,%d]\n", first_occ(a, N, 500), last_occ(a, N, 500));
    printf("empty: [%d,%d]\n", first_occ(a, 0, 1), last_occ(a, 0, 1));
    return 0;
}
