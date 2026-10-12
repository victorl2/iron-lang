/*
 * title: Exponential (galloping) search
 * topic: algorithms
 * covers: exponential search, unbounded sequences, doubling bound then binary search, function-pointer oracle, probe cost O(log i)
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

typedef long (*Oracle)(long); /* monotone increasing sequence defined for all i >= 0 */

static long calls;

static long seq_square(long i) {
    calls++;
    return i * i + 3;
}
static long seq_tri(long i) {
    calls++;
    return i * (i + 1) / 2 * 2 + i;
}
static long seq_fib_like(long i) {
    calls++;
    /* strictly increasing, roughly exponential: 3 * 2^(i/2) plus i */
    long v = 3;
    for (long k = 0; k < i / 2 && k < 40; k++)
        v *= 2;
    return v + i;
}

/* smallest i with f(i) >= target; doubles the probe until it overshoots */
static long first_at_least(Oracle f, long target) {
    if (f(0) >= target)
        return 0;
    long hi = 1;
    while (f(hi) < target)
        hi *= 2;
    long lo = hi / 2 + 1; /* f(hi/2) < target */
    while (lo < hi) {
        long mid = lo + (hi - lo) / 2;
        if (f(mid) >= target)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/* classic exponential search on a finite sorted array */
static int gallop(const int *a, int n, int key) {
    if (n == 0)
        return -1;
    if (a[0] == key)
        return 0;
    int i = 1;
    while (i < n && a[i] <= key)
        i *= 2;
    int lo = i / 2, hi = (i < n ? i : n - 1);
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
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
    struct {
        const char *name;
        Oracle f;
    } seqs[] = {{"i^2+3", seq_square}, {"tri", seq_tri}, {"fibish", seq_fib_like}};
    long targets[] = {0, 3, 4, 100, 12345, 5000000, 1000000000};
    for (int s = 0; s < 3; s++)
        for (int t = 0; t < 7; t++) {
            calls = 0;
            long r = first_at_least(seqs[s].f, targets[t]);
            long used = calls;
            if (seqs[s].f(r) < targets[t] || (r > 0 && seqs[s].f(r - 1) >= targets[t]))
                fail("first_at_least");
            printf("%-6s >= %-10ld at i=%-8ld oracle calls=%ld\n", seqs[s].name, targets[t], r,
                   used);
        }

    static int a[1000];
    for (int i = 0; i < 1000; i++)
        a[i] = 7 * i + 2;
    int ok = 0;
    for (int k = -3; k < 7010; k++) {
        int g = gallop(a, 1000, k);
        int want = (k >= 2 && (k - 2) % 7 == 0 && (k - 2) / 7 < 1000) ? (k - 2) / 7 : -1;
        if (g != want)
            fail("gallop");
        if (g >= 0)
            ok++;
    }
    printf("gallop hits: %d, empty: %d\n", ok, gallop(a, 0, 1));
    return 0;
}
