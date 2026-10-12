/*
 * title: Bitonic sorting network
 * topic: algorithms
 * covers: sorting networks, bitonic merge, compare-exchange, power-of-two sizes, XOR partner indexing, depth count
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 909090u;
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

static int comparators, layers;

static void cx(int *a, int i, int j, int up) {
    comparators++;
    if ((a[i] > a[j]) == up) {
        int t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

/* Iterative bitonic sort using the classic k / j double loop. */
static void bitonic(int *a, int n) {
    comparators = 0;
    layers = 0;
    for (int k = 2; k <= n; k <<= 1) {
        for (int j = k >> 1; j > 0; j >>= 1) {
            layers++;
            for (int i = 0; i < n; i++) {
                int l = i ^ j;
                if (l > i)
                    cx(a, i, l, (i & k) == 0);
            }
        }
    }
}

/* Recursive formulation, to cross-check the network shape. */
static int rec_cmps;

static void merge_rec(int *a, int lo, int n, int up) {
    if (n <= 1)
        return;
    int m = n / 2;
    for (int i = lo; i < lo + m; i++) {
        rec_cmps++;
        if ((a[i] > a[i + m]) == up) {
            int t = a[i];
            a[i] = a[i + m];
            a[i + m] = t;
        }
    }
    merge_rec(a, lo, m, up);
    merge_rec(a, lo + m, m, up);
}

static void sort_rec(int *a, int lo, int n, int up) {
    if (n <= 1)
        return;
    int m = n / 2;
    sort_rec(a, lo, m, 1);
    sort_rec(a, lo + m, m, 0);
    merge_rec(a, lo, n, up);
}

int main(void) {
    for (int n = 2; n <= 1024; n *= 2) {
        int *a = malloc(sizeof(int) * (size_t)n), *b = malloc(sizeof(int) * (size_t)n);
        check(a && b, "alloc");
        for (int i = 0; i < n; i++)
            a[i] = b[i] = (int)(rng() % 10000);
        bitonic(a, n);
        rec_cmps = 0;
        sort_rec(b, 0, n, 1);
        for (int i = 1; i < n; i++) {
            check(a[i - 1] <= a[i], "iterative sorted");
            check(b[i - 1] <= b[i], "recursive sorted");
        }
        check(memcmp(a, b, sizeof(int) * (size_t)n) == 0, "same output");
        check(rec_cmps == comparators, "same comparator count");
        int lg = 0;
        while ((1 << lg) < n)
            lg++;
        check(layers == lg * (lg + 1) / 2, "depth = lg(lg+1)/2");
        printf("n=%-5d layers=%-3d comparators=%-6d min=%d max=%d\n", n, layers, comparators, a[0], a[n - 1]);
        free(a);
        free(b);
    }
    return 0;
}
