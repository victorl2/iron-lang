/*
 * title: Fibonacci search
 * topic: algorithms
 * covers: Fibonacci search, addition-only index arithmetic, padding by virtual infinity, probe counts, comparison with binary search
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

static long cmps;

static int fib_search(const int *a, int n, int key) {
    long f2 = 0, f1 = 1, f = f2 + f1; /* F(k-2), F(k-1), F(k) */
    while (f < n) {
        f2 = f1;
        f1 = f;
        f = f2 + f1;
    }
    int offset = -1;
    while (f > 1) {
        long i = offset + f2;
        if (i > n - 1)
            i = n - 1;
        cmps++;
        if (a[i] < key) {
            f = f1;
            f1 = f2;
            f2 = f - f1;
            offset = (int)i;
        } else if (a[i] > key) {
            f = f2;
            f1 = f1 - f2;
            f2 = f - f1;
        } else
            return (int)i;
    }
    if (f1 && offset + 1 < n) {
        cmps++;
        if (a[offset + 1] == key)
            return offset + 1;
    }
    return -1;
}

static int bin_search(const int *a, int n, int key) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        cmps++;
        if (a[mid] == key)
            return mid;
        if (a[mid] < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

int main(void) {
    static int a[3000];
    for (int i = 0; i < 3000; i++)
        a[i] = 4 * i + 1;
    int sizes[] = {1, 2, 3, 4, 5, 8, 13, 21, 22, 100, 233, 1000, 2584, 3000};
    for (int s = 0; s < 14; s++) {
        int n = sizes[s];
        long cf = 0, cb = 0;
        int found = 0;
        for (int k = 0; k < 4 * n + 3; k++) {
            cmps = 0;
            int r = fib_search(a, n, k);
            cf += cmps;
            cmps = 0;
            int b = bin_search(a, n, k);
            cb += cmps;
            if (r != b)
                fail("fib vs binary");
            if (r >= 0)
                found++;
        }
        printf("n=%-5d found=%-5d fib_cmps=%-7ld bin_cmps=%ld\n", n, found, cf, cb);
    }
    return 0;
}
