/*
 * title: Cocktail shaker sort with shrinking bounds
 * topic: algorithms
 * covers: bidirectional bubble sort, early exit, pass counting, last-swap bounds
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 2463534242u;
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

typedef struct {
    int passes;
    long swaps;
} Stats;

/* Plain bubble sort with early exit, for comparison. */
static Stats bubble(int *a, int n) {
    Stats s = {0, 0};
    int swapped = 1;
    while (swapped) {
        swapped = 0;
        s.passes++;
        for (int i = 1; i < n; i++)
            if (a[i - 1] > a[i]) {
                int t = a[i - 1];
                a[i - 1] = a[i];
                a[i] = t;
                swapped = 1;
                s.swaps++;
            }
    }
    return s;
}

/* Cocktail sort: remember the last swap position to shrink both ends. */
static Stats cocktail(int *a, int n) {
    Stats s = {0, 0};
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        int last = lo;
        s.passes++;
        for (int i = lo; i < hi; i++)
            if (a[i] > a[i + 1]) {
                int t = a[i];
                a[i] = a[i + 1];
                a[i + 1] = t;
                last = i;
                s.swaps++;
            }
        hi = last;
        if (lo >= hi)
            break;
        last = hi;
        s.passes++;
        for (int i = hi; i > lo; i--)
            if (a[i - 1] > a[i]) {
                int t = a[i - 1];
                a[i - 1] = a[i];
                a[i] = t;
                last = i;
                s.swaps++;
            }
        lo = last;
    }
    return s;
}

static void fill(int *a, int n, int shape) {
    for (int i = 0; i < n; i++) {
        switch (shape) {
        case 0: a[i] = (int)(rng() % 1000); break;
        case 1: a[i] = i; break;
        case 2: a[i] = n - i; break;
        case 3: a[i] = (i == 0) ? n : i; break;           /* max at front: turtle-free */
        case 4: a[i] = (i == n - 1) ? 0 : i + 1; break;   /* min at back: turtle */
        default: a[i] = i % 7; break;
        }
    }
}

int main(void) {
    static const char *names[] = {"random", "sorted", "reversed", "max-first", "min-last", "mod7"};
    enum { N = 120 };
    int a[N], b[N];
    for (int shape = 0; shape < 6; shape++) {
        fill(a, N, shape);
        for (int i = 0; i < N; i++)
            b[i] = a[i];
        Stats sb = bubble(a, N);
        Stats sc = cocktail(b, N);
        for (int i = 1; i < N; i++) {
            check(a[i - 1] <= a[i], "bubble sorted");
            check(b[i - 1] <= b[i], "cocktail sorted");
        }
        for (int i = 0; i < N; i++)
            check(a[i] == b[i], "same result");
        check(sb.swaps == sc.swaps, "swap count equals inversion count");
        printf("%-9s bubble passes=%3d cocktail passes=%3d swaps=%ld\n", names[shape], sb.passes,
               sc.passes, sc.swaps);
    }
    int tiny[1] = {5};
    Stats z = cocktail(tiny, 1);
    check(z.passes == 0 && tiny[0] == 5, "single element");
    printf("done\n");
    return 0;
}
