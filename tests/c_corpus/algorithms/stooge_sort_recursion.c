/*
 * title: Stooge sort and recursion cost of slow sorts
 * topic: algorithms
 * covers: stooge sort, slowsort, recursion call counting, exponent fit, ceiling-two-thirds arithmetic
 * deps: libc, libm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 1010101u;
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

static long calls, swaps;

static void swp(int *a, int *b) {
    int t = *a;
    *a = *b;
    *b = t;
    swaps++;
}

static void stooge(int *a, int lo, int hi) {
    calls++;
    if (a[lo] > a[hi])
        swp(&a[lo], &a[hi]);
    if (hi - lo + 1 > 2) {
        int t = (hi - lo + 1) / 3;
        stooge(a, lo, hi - t);
        stooge(a, lo + t, hi);
        stooge(a, lo, hi - t);
    }
}

static void slowsort(int *a, int lo, int hi) {
    calls++;
    if (lo >= hi)
        return;
    int m = lo + (hi - lo) / 2;
    slowsort(a, lo, m);
    slowsort(a, m + 1, hi);
    if (a[m] > a[hi])
        swp(&a[m], &a[hi]);
    slowsort(a, lo, hi - 1);
}

int main(void) {
    static const int sizes[] = {3, 6, 9, 12, 18, 27, 40, 60};
    long prev_calls = 0;
    int prev_n = 0;
    printf("stooge sort: T(n) = 3 T(2n/3) + O(1), exponent log(3)/log(1.5) = %.3f\n", log(3.0) / log(1.5));
    for (int s = 0; s < 8; s++) {
        int n = sizes[s];
        int *a = malloc(sizeof(int) * (size_t)n), *b = malloc(sizeof(int) * (size_t)n);
        check(a && b, "alloc");
        for (int i = 0; i < n; i++)
            a[i] = b[i] = (int)(rng() % 100);
        calls = swaps = 0;
        stooge(a, 0, n - 1);
        for (int i = 1; i < n; i++)
            check(a[i - 1] <= a[i], "stooge sorted");
        long sc = calls, ss = swaps;
        calls = swaps = 0;
        if (n <= 27) {
            slowsort(b, 0, n - 1);
            for (int i = 1; i < n; i++)
                check(b[i - 1] <= b[i], "slowsort sorted");
            check(memcmp(a, b, sizeof(int) * (size_t)n) == 0, "same result");
        }
        double slope = 0;
        if (prev_n)
            slope = log((double)sc / (double)prev_calls) / log((double)n / prev_n);
        printf("n=%-3d stooge calls=%-9ld swaps=%-7ld local exponent=%.1f", n, sc, ss, slope);
        if (n <= 27)
            printf(" slowsort calls=%ld", calls);
        printf("\n");
        prev_calls = sc;
        prev_n = n;
        free(a);
        free(b);
    }
    return 0;
}
