/*
 * title: Three-way partition quicksort on duplicate-heavy data
 * topic: algorithms
 * covers: Dutch national flag, Bentley-McIlroy style equal keys, string keys, comparison counting, recursion
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 555u;
static unsigned rng(void) {
    st = st * 1664525u + 1013904223u;
    return st >> 4;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long cmps;

static int cmp(int a, int b) {
    cmps++;
    return (a > b) - (a < b);
}

static void swp(int *a, int i, int j) {
    int t = a[i];
    a[i] = a[j];
    a[j] = t;
}

/* Dijkstra three-way: [lo,lt) < p, [lt,i) == p, (gt,hi] > p */
static void qs3(int *a, int lo, int hi) {
    while (lo < hi) {
        int m = lo + (hi - lo) / 2;
        /* median of three pivot */
        int x = a[lo], y = a[m], z = a[hi];
        int p = (x < y) ? (y < z ? y : (x < z ? z : x)) : (x < z ? x : (y < z ? z : y));
        int lt = lo, i = lo, gt = hi;
        while (i <= gt) {
            int c = cmp(a[i], p);
            if (c < 0)
                swp(a, lt++, i++);
            else if (c > 0)
                swp(a, i, gt--);
            else
                i++;
        }
        if (lt - lo < hi - gt) {
            qs3(a, lo, lt - 1);
            lo = gt + 1;
        } else {
            qs3(a, gt + 1, hi);
            hi = lt - 1;
        }
    }
}

/* Two-way (Hoare-like, stopping on equal keys) for contrast, no median. */
static void qs2(int *a, int lo, int hi) {
    while (lo < hi) {
        int p = a[lo + (hi - lo) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (cmp(a[i], p) < 0)
                i++;
            while (cmp(a[j], p) > 0)
                j--;
            if (i <= j) {
                swp(a, i, j);
                i++;
                j--;
            }
        }
        if (j - lo < hi - i) {
            qs2(a, lo, j);
            lo = i;
        } else {
            qs2(a, i, hi);
            hi = j;
        }
    }
}

int main(void) {
    enum { N = 5000 };
    static int base[N], a[N], b[N];
    static const int distinct[] = {2, 5, 16, 100, 5000};
    for (int d = 0; d < 5; d++) {
        for (int i = 0; i < N; i++)
            base[i] = (int)(rng() % (unsigned)distinct[d]);
        memcpy(a, base, sizeof a);
        memcpy(b, base, sizeof b);
        cmps = 0;
        qs3(a, 0, N - 1);
        long c3 = cmps;
        cmps = 0;
        qs2(b, 0, N - 1);
        long c2 = cmps;
        for (int i = 1; i < N; i++) {
            check(a[i - 1] <= a[i], "3-way sorted");
            check(b[i - 1] <= b[i], "2-way sorted");
        }
        check(memcmp(a, b, sizeof a) == 0, "same output");
        int runs = 1;
        for (int i = 1; i < N; i++)
            runs += a[i] != a[i - 1];
        printf("distinct<=%-5d actual=%-5d 3way cmps=%-7ld 2way cmps=%ld\n", distinct[d], runs, c3, c2);
    }
    /* multiset preserved: histogram check */
    int hist[8] = {0}, hist2[8] = {0};
    static int c8[64];
    for (int i = 0; i < 64; i++) {
        c8[i] = (int)(rng() % 8);
        hist[c8[i]]++;
    }
    qs3(c8, 0, 63);
    for (int i = 0; i < 64; i++)
        hist2[c8[i]]++;
    check(memcmp(hist, hist2, sizeof hist) == 0, "histogram preserved");
    printf("hist:");
    for (int i = 0; i < 8; i++)
        printf(" %d", hist2[i]);
    printf("\n");
    return 0;
}
