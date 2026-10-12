/*
 * title: Jump search with optimal block size
 * topic: algorithms
 * covers: jump search, sqrt block size, linear scan within block, block-size sweep, comparison counting
 * deps: libc, libm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

static long cmps;

static int jump_search(const int *a, int n, int key, int step) {
    int prev = 0, cur = step;
    while (prev < n) {
        int last = (cur < n ? cur : n) - 1;
        cmps++;
        if (a[last] >= key)
            break;
        prev = cur;
        cur += step;
    }
    for (int i = prev; i < n && i < cur; i++) {
        cmps++;
        if (a[i] == key)
            return i;
        if (a[i] > key)
            return -1;
    }
    return -1;
}

int main(void) {
    enum { N = 10000 };
    static int a[N];
    for (int i = 0; i < N; i++)
        a[i] = 4 * i + (i % 3);
    int sq = (int)floor(sqrt((double)N));
    printf("sqrt block size for n=%d: %d\n", N, sq);

    int steps[] = {1, 2, 5, 10, 50, sq, 200, 1000, 5000, 10000};
    for (int s = 0; s < 10; s++) {
        long total = 0;
        int found = 0;
        for (int k = 0; k < 4 * N; k += 7) {
            cmps = 0;
            int r = jump_search(a, N, k, steps[s]);
            total += cmps;
            int want = -1;
            /* a is strictly increasing so a plain binary search is the truth */
            int lo = 0, hi = N - 1;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                if (a[mid] == k) {
                    want = mid;
                    break;
                }
                if (a[mid] < k)
                    lo = mid + 1;
                else
                    hi = mid - 1;
            }
            if (r != want)
                fail("jump result");
            if (r >= 0)
                found++;
        }
        printf("step=%-5d found=%-4d total comparisons=%ld\n", steps[s], found, total);
    }
    int tiny[3] = {1, 5, 9};
    printf("tiny: %d %d %d %d\n", jump_search(tiny, 3, 1, 2), jump_search(tiny, 3, 9, 2),
           jump_search(tiny, 3, 4, 2), jump_search(tiny, 0, 4, 2));
    return 0;
}
