/*
 * title: Deterministic linear-time selection (median of medians)
 * topic: algorithms
 * covers: BFPRT selection, groups of five, recursive pivot choice, worst-case linear bound, adversarial ordered inputs
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 13579u;
static unsigned rng(void) {
    st = st * 1664525u + 1013904223u;
    return st >> 8;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long cmps;

static void insertion(int *a, int n) {
    for (int i = 1; i < n; i++) {
        int x = a[i], j = i - 1;
        while (j >= 0 && (cmps++, a[j] > x)) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = x;
    }
}

static int select_k(int *a, int n, int k);

static int pivot_mom(int *a, int n) {
    if (n <= 5) {
        insertion(a, n);
        return a[n / 2];
    }
    int groups = (n + 4) / 5;
    for (int g = 0; g < groups; g++) {
        int lo = g * 5, len = n - lo < 5 ? n - lo : 5;
        insertion(a + lo, len);
        int med = a[lo + len / 2];
        int t = a[g];
        a[g] = med;
        a[lo + len / 2] = t;
    }
    return select_k(a, groups, groups / 2);
}

/* Returns the k-th smallest (0-based) of a[0..n); reorders a. */
static int select_k(int *a, int n, int k) {
    for (;;) {
        if (n <= 5) {
            insertion(a, n);
            return a[k];
        }
        int p = pivot_mom(a, n);
        /* three-way partition around p using a scratch copy */
        int *tmp = malloc(sizeof(int) * (size_t)n);
        check(tmp != NULL, "alloc");
        int lt = 0, eq = 0;
        for (int i = 0; i < n; i++) {
            cmps++;
            lt += a[i] < p;
            eq += a[i] == p;
        }
        int il = 0, ie = lt, ig = lt + eq;
        for (int i = 0; i < n; i++) {
            if (a[i] < p)
                tmp[il++] = a[i];
            else if (a[i] == p)
                tmp[ie++] = a[i];
            else
                tmp[ig++] = a[i];
        }
        memcpy(a, tmp, sizeof(int) * (size_t)n);
        free(tmp);
        if (k < lt) {
            n = lt;
        } else if (k < lt + eq) {
            return p;
        } else {
            a += lt + eq;
            k -= lt + eq;
            n -= lt + eq;
        }
    }
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    static const int sizes[] = {1, 5, 6, 26, 100, 1000, 10000};
    for (int s = 0; s < 7; s++) {
        int n = sizes[s];
        int *base = malloc(sizeof(int) * (size_t)n), *a = malloc(sizeof(int) * (size_t)n);
        int *ref = malloc(sizeof(int) * (size_t)n);
        check(base && a && ref, "alloc");
        for (int shape = 0; shape < 3; shape++) {
            for (int i = 0; i < n; i++)
                base[i] = shape == 0 ? (int)(rng() % 100000) : shape == 1 ? i : n - i;
            memcpy(ref, base, sizeof(int) * (size_t)n);
            qsort(ref, (size_t)n, sizeof(int), cmp_int);
            long worst = 0;
            for (int t = 0; t < 5; t++) {
                int k = (int)(rng() % (unsigned)n);
                memcpy(a, base, sizeof(int) * (size_t)n);
                cmps = 0;
                int v = select_k(a, n, k);
                check(v == ref[k], "k-th matches sorted");
                if (cmps > worst)
                    worst = cmps;
            }
            check(worst <= 30L * n + 30, "linear bound");
            printf("n=%-6d %-9s worst comparisons over 5 ranks=%-7ld per element=%.1f\n", n,
                   shape == 0 ? "random" : shape == 1 ? "ascending" : "descending", worst, (double)worst / n);
        }
        free(base);
        free(a);
        free(ref);
    }
    return 0;
}
