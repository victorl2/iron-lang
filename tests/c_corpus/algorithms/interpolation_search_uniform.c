/*
 * title: Interpolation search vs binary search
 * topic: algorithms
 * covers: interpolation search, probe counting, uniform versus clustered data, division-by-zero guards
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned long long rs = 424242ull;
static unsigned rnd(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 33);
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

static long probes;

static int interp(const long *a, int n, long key) {
    int lo = 0, hi = n - 1;
    while (lo <= hi && key >= a[lo] && key <= a[hi]) {
        int pos;
        if (a[hi] == a[lo])
            pos = lo;
        else
            pos = lo + (int)(((double)(key - a[lo]) * (double)(hi - lo)) / (double)(a[hi] - a[lo]));
        if (pos < lo)
            pos = lo;
        if (pos > hi)
            pos = hi;
        probes++;
        if (a[pos] == key)
            return pos;
        if (a[pos] < key)
            lo = pos + 1;
        else
            hi = pos - 1;
    }
    return -1;
}

static int bin(const long *a, int n, long key) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        probes++;
        if (a[mid] == key)
            return mid;
        if (a[mid] < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static void run(const char *name, const long *a, int n) {
    long pi = 0, pb = 0;
    int found = 0;
    for (int q = 0; q < 2000; q++) {
        long key = (q % 2) ? a[(int)(rnd() % (unsigned)n)] : a[(int)(rnd() % (unsigned)n)] + 1;
        probes = 0;
        int i = interp(a, n, key);
        pi += probes;
        probes = 0;
        int b = bin(a, n, key);
        pb += probes;
        if ((i < 0) != (b < 0) || (i >= 0 && a[i] != key))
            fail("disagree");
        if (i >= 0)
            found++;
    }
    printf("%-10s found=%d interp_probes=%ld binary_probes=%ld winner=%s\n", name, found, pi, pb,
           pi < pb ? "interpolation" : "binary");
}

int main(void) {
    enum { N = 4096 };
    static long a[N];

    /* near-uniform: evenly spaced with jitter */
    long v = 0;
    for (int i = 0; i < N; i++) {
        v += 5 + (long)(rnd() % 4);
        a[i] = v;
    }
    run("uniform", a, N);

    /* exponential growth: interpolation degrades */
    long e = 1;
    for (int i = 0; i < 62; i++) {
        a[i] = e;
        e = e * 2 + (long)(rnd() % 2);
    }
    run("doubling", a, 62);

    /* clustered: two dense clusters far apart */
    for (int i = 0; i < N; i++)
        a[i] = (i < N / 2) ? i * 2 : 1000000 + (i - N / 2) * 3;
    run("clusters", a, N);

    /* all equal keys */
    for (int i = 0; i < 100; i++)
        a[i] = 9;
    printf("all-equal: %d %d\n", interp(a, 100, 9) >= 0, interp(a, 100, 10));
    return 0;
}
