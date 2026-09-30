/*
 * title: Quicksort with Lomuto and Hoare partitioning
 * topic: algorithms
 * covers: quicksort, Lomuto partition, Hoare partition, swap counting, recursion on smaller half
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 20240101u;
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

static long swaps;
static int max_depth;

static void swp(int *a, int *b) {
    int t = *a;
    *a = *b;
    *b = t;
    swaps++;
}

static int lomuto(int *a, int lo, int hi) {
    int mid = lo + (hi - lo) / 2;
    swp(&a[mid], &a[hi]);
    int p = a[hi], i = lo;
    for (int j = lo; j < hi; j++)
        if (a[j] < p)
            swp(&a[i++], &a[j]);
    swp(&a[i], &a[hi]);
    return i; /* pivot final position */
}

static int hoare(int *a, int lo, int hi) {
    int p = a[lo + (hi - lo) / 2];
    int i = lo - 1, j = hi + 1;
    for (;;) {
        do
            i++;
        while (a[i] < p);
        do
            j--;
        while (a[j] > p);
        if (i >= j)
            return j;
        swp(&a[i], &a[j]);
    }
}

static void qs_lomuto(int *a, int lo, int hi, int d) {
    while (lo < hi) {
        if (d > max_depth)
            max_depth = d;
        int p = lomuto(a, lo, hi);
        /* recurse into the smaller side, loop on the larger */
        if (p - lo < hi - p) {
            qs_lomuto(a, lo, p - 1, d + 1);
            lo = p + 1;
        } else {
            qs_lomuto(a, p + 1, hi, d + 1);
            hi = p - 1;
        }
    }
}

static void qs_hoare(int *a, int lo, int hi, int d) {
    while (lo < hi) {
        if (d > max_depth)
            max_depth = d;
        int p = hoare(a, lo, hi);
        if (p - lo < hi - p) {
            qs_hoare(a, lo, p, d + 1);
            lo = p + 1;
        } else {
            qs_hoare(a, p + 1, hi, d + 1);
            hi = p;
        }
    }
}

int main(void) {
    enum { N = 4000 };
    static int base[N], a[N];
    static const char *shapes[] = {"random", "sorted", "reversed", "few-distinct", "all-equal", "organ-pipe"};
    for (int s = 0; s < 6; s++) {
        for (int i = 0; i < N; i++) {
            switch (s) {
            case 0: base[i] = (int)(rng() % 100000); break;
            case 1: base[i] = i; break;
            case 2: base[i] = N - i; break;
            case 3: base[i] = (int)(rng() % 4); break;
            case 4: base[i] = 7; break;
            default: base[i] = i < N / 2 ? i : N - i; break;
            }
        }
        long sw[2];
        int dp[2];
        for (int v = 0; v < 2; v++) {
            memcpy(a, base, sizeof a);
            swaps = 0;
            max_depth = 0;
            if (v == 0)
                qs_lomuto(a, 0, N - 1, 0);
            else
                qs_hoare(a, 0, N - 1, 0);
            for (int i = 1; i < N; i++)
                check(a[i - 1] <= a[i], "sorted");
            sw[v] = swaps;
            dp[v] = max_depth;
            check(max_depth <= 40, "depth bounded by log n thanks to smaller-half recursion");
        }
        printf("%-12s lomuto swaps=%-7ld depth=%-2d | hoare swaps=%-7ld depth=%d\n", shapes[s], sw[0], dp[0],
               sw[1], dp[1]);
    }
    return 0;
}
