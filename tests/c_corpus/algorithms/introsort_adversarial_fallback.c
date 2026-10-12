/*
 * title: Introsort with heapsort fallback on adversarial input
 * topic: algorithms
 * covers: introsort, depth limit, median-of-three killer sequence, heapsort fallback, insertion sort finish
 * deps: libc, libm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 8675309u;
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

static int fallbacks;
static long cmps;

#define LT(x, y) (cmps++, (x) < (y))

static void sift(int *a, int base, int n, int i) {
    for (;;) {
        int c = 2 * i + 1;
        if (c >= n)
            return;
        if (c + 1 < n && LT(a[base + c], a[base + c + 1]))
            c++;
        if (!LT(a[base + i], a[base + c]))
            return;
        int t = a[base + i];
        a[base + i] = a[base + c];
        a[base + c] = t;
        i = c;
    }
}

static void heapsort_range(int *a, int lo, int n) {
    for (int i = n / 2 - 1; i >= 0; i--)
        sift(a, lo, n, i);
    for (int e = n - 1; e > 0; e--) {
        int t = a[lo];
        a[lo] = a[lo + e];
        a[lo + e] = t;
        sift(a, lo, e, 0);
    }
}

static int med3(int *a, int i, int j, int k) {
    if (LT(a[i], a[j]))
        return LT(a[j], a[k]) ? j : (LT(a[i], a[k]) ? k : i);
    return LT(a[i], a[k]) ? i : (LT(a[j], a[k]) ? k : j);
}

static void intro(int *a, int lo, int hi, int depth, int allow_fallback) {
    while (hi - lo > 16) {
        if (allow_fallback && depth == 0) {
            fallbacks++;
            heapsort_range(a, lo, hi - lo + 1);
            return;
        }
        depth--;
        int m = med3(a, lo, lo + (hi - lo) / 2, hi);
        int p = a[m];
        int i = lo, j = hi;
        while (i <= j) {
            while (LT(a[i], p))
                i++;
            while (LT(p, a[j]))
                j--;
            if (i <= j) {
                int t = a[i];
                a[i] = a[j];
                a[j] = t;
                i++;
                j--;
            }
        }
        intro(a, lo, j, depth, allow_fallback);
        lo = i;
    }
}

static void isort(int *a, int n) {
    for (int i = 1; i < n; i++) {
        int x = a[i], j = i - 1;
        while (j >= 0 && LT(x, a[j])) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = x;
    }
}

static void introsort(int *a, int n, int allow_fallback) {
    int depth = 2 * (int)floor(log2((double)n));
    intro(a, 0, n - 1, depth, allow_fallback);
    isort(a, n);
}

/* Musser's median-of-3 killer: forces quadratic behaviour in naive median-of-3 quicksort. */
static void killer(int *a, int n) {
    int k = n / 2;
    for (int i = 1; i <= k; i++) {
        if (i % 2 == 1) {
            a[i - 1] = i;
            a[i] = k + i;
        }
        a[k + i - 1] = 2 * i;
    }
}

int main(void) {
    enum { N = 2000 };
    static int base[N], a[N];
    for (int i = 0; i < N; i++)
        base[i] = (int)(rng() % 1000000);

    for (int shape = 0; shape < 3; shape++) {
        if (shape == 0)
            for (int i = 0; i < N; i++)
                base[i] = (int)(rng() % 1000000);
        else if (shape == 1)
            killer(base, N);
        else
            for (int i = 0; i < N; i++)
                base[i] = N - i;
        static const char *nm[] = {"random", "killer", "reversed"};
        long c[2];
        int fb[2];
        for (int mode = 0; mode < 2; mode++) {
            memcpy(a, base, sizeof a);
            cmps = 0;
            fallbacks = 0;
            introsort(a, N, mode);
            for (int i = 1; i < N; i++)
                check(a[i - 1] <= a[i], "sorted");
            c[mode] = cmps;
            fb[mode] = fallbacks;
        }
        printf("%-8s no-limit cmps=%-7ld | introsort cmps=%-7ld fallbacks=%d\n", nm[shape], c[0], c[1], fb[1]);
        check(fb[0] == 0, "no fallback when disabled");
    }
    return 0;
}
