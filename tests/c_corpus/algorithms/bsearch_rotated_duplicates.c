/*
 * title: Rotated array with duplicates
 * topic: algorithms
 * covers: binary search degeneration, ambiguity shrinking, worst-case linear steps, step counting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 88172645u;
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

static long steps;

static int min_value(const int *a, int n) {
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        steps++;
        if (a[mid] > a[hi])
            lo = mid + 1;
        else if (a[mid] < a[hi])
            hi = mid;
        else
            hi--; /* cannot tell which side holds the pivot */
    }
    return a[lo];
}

static int contains(const int *a, int n, int key) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        steps++;
        if (a[mid] == key)
            return 1;
        if (a[lo] == a[mid] && a[mid] == a[hi]) {
            lo++;
            hi--;
        } else if (a[lo] <= a[mid]) {
            if (a[lo] <= key && key < a[mid])
                hi = mid - 1;
            else
                lo = mid + 1;
        } else {
            if (a[mid] < key && key <= a[hi])
                lo = mid + 1;
            else
                hi = mid - 1;
        }
    }
    return 0;
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    enum { N = 64 };
    int a[N], r[N];
    int checked = 0;
    for (int trial = 0; trial < 300; trial++) {
        int n = 1 + (int)(rnd() % N);
        int range = 1 + (int)(rnd() % 6);
        for (int i = 0; i < n; i++)
            a[i] = (int)(rnd() % (unsigned)range);
        qsort(a, (size_t)n, sizeof a[0], cmp_int);
        int rot = (int)(rnd() % (unsigned)n);
        for (int i = 0; i < n; i++)
            r[i] = a[(i + rot) % n];
        if (min_value(r, n) != a[0])
            fail("min_value");
        for (int k = -1; k <= range; k++) {
            int want = 0;
            for (int i = 0; i < n; i++)
                if (r[i] == k)
                    want = 1;
            if (contains(r, n, k) != want)
                fail("contains");
            checked++;
        }
    }
    printf("random checks: %d\n", checked);

    /* Adversarial: all equal but one small value hiding somewhere. */
    for (int pos = 0; pos < 3; pos++) {
        int n = 1000, arr[1000];
        for (int i = 0; i < n; i++)
            arr[i] = 5;
        int at = pos == 0 ? 0 : pos == 1 ? 500 : 999;
        arr[at] = 1;
        steps = 0;
        int m = min_value(arr, n);
        printf("hidden min at %d: found %d in %ld steps\n", at, m, steps);
    }
    /* Benign: strictly rotated distinct values stay logarithmic. */
    int arr[1024];
    for (int i = 0; i < 1024; i++)
        arr[i] = (i + 300) % 1024;
    steps = 0;
    printf("distinct rotated: min %d in %ld steps\n", min_value(arr, 1024), steps);
    return 0;
}
