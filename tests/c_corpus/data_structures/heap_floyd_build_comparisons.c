/*
 * title: Floyd heap construction versus repeated insertion
 * topic: data_structures
 * covers: heapify, Floyd build, comparison bounds, input orderings, invariant checks
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 88172645463325252ull;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long cmps;

static void sift_down(int *a, int n, int i) {
    int x = a[i];
    for (;;) {
        int c = 2 * i + 1;
        if (c >= n)
            break;
        if (c + 1 < n) {
            cmps++;
            if (a[c + 1] < a[c])
                c++;
        }
        cmps++;
        if (a[c] >= x)
            break;
        a[i] = a[c];
        i = c;
    }
    a[i] = x;
}

static void build_floyd(int *a, int n) {
    for (int i = n / 2 - 1; i >= 0; i--)
        sift_down(a, n, i);
}

static void build_insert(int *a, int n) {
    for (int k = 1; k < n; k++) {
        int x = a[k], i = k;
        while (i > 0) {
            int p = (i - 1) / 2;
            cmps++;
            if (a[p] <= x)
                break;
            a[i] = a[p];
            i = p;
        }
        a[i] = x;
    }
}

static int is_heap(const int *a, int n) {
    for (int i = 1; i < n; i++)
        if (a[(i - 1) / 2] > a[i])
            return 0;
    return 1;
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

static void fill(int *a, int n, int kind) {
    for (int i = 0; i < n; i++) {
        if (kind == 0)
            a[i] = (int)(rng() % 100000);
        else if (kind == 1)
            a[i] = i;
        else
            a[i] = n - i;
    }
}

int main(void) {
    const char *kinds[3] = {"random", "ascending", "descending"};
    int sizes[] = {1, 2, 3, 7, 15, 100, 1000, 4095};
    int *a = malloc(sizeof(int) * 4096), *b = malloc(sizeof(int) * 4096), *s = malloc(sizeof(int) * 4096);
    for (int kind = 0; kind < 3; kind++) {
        printf("%s\n", kinds[kind]);
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
            int n = sizes[si];
            fill(a, n, kind);
            memcpy(b, a, sizeof(int) * n);
            memcpy(s, a, sizeof(int) * n);
            qsort(s, n, sizeof(int), cmp_int);
            cmps = 0;
            build_floyd(a, n);
            long f = cmps;
            cmps = 0;
            build_insert(b, n);
            long ins = cmps;
            check(is_heap(a, n) && is_heap(b, n), "both builds give heaps");
            qsort(a, n, sizeof(int), cmp_int);
            qsort(b, n, sizeof(int), cmp_int);
            check(memcmp(a, s, sizeof(int) * n) == 0, "floyd keeps multiset");
            check(memcmp(b, s, sizeof(int) * n) == 0, "insert keeps multiset");
            check(f <= 2L * n, "Floyd needs at most 2n comparisons");
            printf("  n=%-5d floyd=%-6ld insert=%-6ld\n", n, f, ins);
        }
    }
    free(a);
    free(b);
    free(s);
    return 0;
}
