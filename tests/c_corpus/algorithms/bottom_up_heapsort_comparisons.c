/*
 * title: Bottom-up heapsort versus classic heapsort comparisons
 * topic: algorithms
 * covers: heapsort, Floyd's leaf-to-root sift, comparison counting, n log n bound, ternary helper
 * deps: libc, libm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long st = 4101842887655102017ULL;
static unsigned rng(void) {
    st ^= st >> 12;
    st ^= st << 25;
    st ^= st >> 27;
    return (unsigned)((st * 2685821657736338717ULL) >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long cmps;

static int lt(int a, int b) {
    cmps++;
    return a < b;
}

static void classic_sift(int *a, int n, int i) {
    for (;;) {
        int c = 2 * i + 1;
        if (c >= n)
            return;
        if (c + 1 < n && lt(a[c], a[c + 1]))
            c++;
        if (!lt(a[i], a[c]))
            return;
        int t = a[i];
        a[i] = a[c];
        a[c] = t;
        i = c;
    }
}

/* Bottom-up: descend along the larger-child path to a leaf (1 compare per level), then climb back. */
static void bottom_up_sift(int *a, int n, int i) {
    int x = a[i];
    int j = i;
    for (;;) {
        int c = 2 * j + 1;
        if (c >= n)
            break;
        if (c + 1 < n && lt(a[c], a[c + 1]))
            c++;
        j = c;
    }
    while (j > i && lt(a[j], x))
        j = (j - 1) / 2;
    /* rotate path: put x at j, shift ancestors up */
    int y = a[j];
    a[j] = x;
    while (j > i) {
        j = (j - 1) / 2;
        int t = a[j];
        a[j] = y;
        y = t;
    }
}

typedef void (*Sift)(int *, int, int);

static long hsort(int *a, int n, Sift sift) {
    cmps = 0;
    for (int i = n / 2 - 1; i >= 0; i--)
        sift(a, n, i);
    for (int e = n - 1; e > 0; e--) {
        int t = a[0];
        a[0] = a[e];
        a[e] = t;
        sift(a, e, 0);
    }
    return cmps;
}

int main(void) {
    static const int sizes[] = {15, 100, 1000, 10000};
    for (int s = 0; s < 4; s++) {
        int n = sizes[s];
        int *base = malloc(sizeof(int) * (size_t)n), *a = malloc(sizeof(int) * (size_t)n);
        check(base && a, "alloc");
        for (int i = 0; i < n; i++)
            base[i] = (int)(rng() % 1000000);
        memcpy(a, base, sizeof(int) * (size_t)n);
        long c1 = hsort(a, n, classic_sift);
        for (int i = 1; i < n; i++)
            check(a[i - 1] <= a[i], "classic sorted");
        int *b = malloc(sizeof(int) * (size_t)n);
        check(b != NULL, "alloc");
        memcpy(b, base, sizeof(int) * (size_t)n);
        long c2 = hsort(b, n, bottom_up_sift);
        for (int i = 1; i < n; i++)
            check(b[i - 1] <= b[i], "bottom-up sorted");
        check(memcmp(a, b, sizeof(int) * (size_t)n) == 0, "same result");
        double nlogn = n * log2((double)n);
        printf("n=%-6d classic=%-7ld bottom-up=%-7ld ratio(classic/nlogn)=%.2f ratio(bu/nlogn)=%.2f\n", n, c1, c2,
               (double)c1 / nlogn, (double)c2 / nlogn);
        check((double)c2 < 1.3 * nlogn + 2 * n, "n log n bound");
        free(base);
        free(a);
        free(b);
    }
    return 0;
}
