/*
 * title: Generic k-sum by recursive reduction
 * topic: algorithms
 * covers: recursion, reduction to two pointers, early pruning by bounds, 64-bit sums, unique tuple counting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned long long rs = 0xABCDEF12345ull;
static unsigned rnd(void) {
    rs ^= rs >> 12;
    rs ^= rs << 25;
    rs ^= rs >> 27;
    return (unsigned)((rs * 0x2545F4914F6CDD1Dull) >> 33);
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}
static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

static long prunes;

/* count distinct value k-tuples in a[from..n) (sorted) with sum == target */
static long ksum(const int *a, int from, int n, int k, long target) {
    if (n - from < k)
        return 0;
    if (k == 2) {
        long cnt = 0;
        int lo = from, hi = n - 1;
        while (lo < hi) {
            long s = (long)a[lo] + a[hi];
            if (s < target)
                lo++;
            else if (s > target)
                hi--;
            else {
                cnt++;
                do
                    lo++;
                while (lo < hi && a[lo] == a[lo - 1]);
                do
                    hi--;
                while (lo < hi && a[hi] == a[hi + 1]);
            }
        }
        return cnt;
    }
    long cnt = 0;
    for (int i = from; i + k <= n; i++) {
        if (i > from && a[i] == a[i - 1])
            continue;
        /* smallest sum reachable with a[i] as the first element: later i only grow it */
        long mn = 0;
        for (int t = 0; t < k; t++)
            mn += a[i + t];
        if (mn > target) {
            prunes++;
            break;
        }
        cnt += ksum(a, i + 1, n, k - 1, target - a[i]);
    }
    return cnt;
}

static long brute(const int *a, int n, int k, long target) {
    /* enumerate index combinations, dedupe by requiring strictly lexicographically new value tuple */
    static int seen[20000][5];
    int nseen = 0;
    int idx[5];
    long cnt = 0;
    for (int i = 0; i < k; i++)
        idx[i] = i;
    if (n < k)
        return 0;
    for (;;) {
        long s = 0;
        for (int i = 0; i < k; i++)
            s += a[idx[i]];
        if (s == target) {
            int dup = 0;
            for (int q = 0; q < nseen && !dup; q++) {
                dup = 1;
                for (int i = 0; i < k; i++)
                    if (seen[q][i] != a[idx[i]])
                        dup = 0;
            }
            if (!dup) {
                for (int i = 0; i < k; i++)
                    seen[nseen][i] = a[idx[i]];
                nseen++;
                cnt++;
            }
        }
        int p = k - 1;
        while (p >= 0 && idx[p] == n - k + p)
            p--;
        if (p < 0)
            break;
        idx[p]++;
        for (int q = p + 1; q < k; q++)
            idx[q] = idx[q - 1] + 1;
    }
    return cnt;
}

int main(void) {
    int a[24];
    for (int k = 2; k <= 5; k++) {
        long total = 0;
        for (int trial = 0; trial < 25; trial++) {
            int n = k + (int)(rnd() % (unsigned)(20 - k));
            for (int i = 0; i < n; i++)
                a[i] = (int)(rnd() % 15) - 7;
            qsort(a, (size_t)n, sizeof a[0], cmp_int);
            long target = (long)(rnd() % 9) - 4;
            long g = ksum(a, 0, n, k, target), b = brute(a, n, k, target);
            if (g != b)
                fail("ksum vs brute");
            total += g;
        }
        printf("k=%d: 25 trials, unique tuples found %ld\n", k, total);
    }
    /* larger instance with big values, only the fast method */
    static int big[300];
    for (int i = 0; i < 300; i++)
        big[i] = (int)(rnd() % 2001) - 1000;
    qsort(big, 300, sizeof big[0], cmp_int);
    prunes = 0;
    printf("n=300 4-sum to 0: %ld\n", ksum(big, 0, 300, 4, 0));
    printf("n=300 3-sum to 500: %ld\n", ksum(big, 0, 300, 3, 500));
    printf("n=300 4-sum to -3900 (prunes used): %ld %s\n", ksum(big, 0, 300, 4, -3900),
           prunes > 0 ? "yes" : "no");
    return 0;
}
