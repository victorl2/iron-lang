/*
 * title: Weak heap with reverse bits and distinguished ancestors
 * topic: data_structures
 * covers: weak heap, reverse bit array, distinguished ancestor, join operation, comparison counting versus binary heap
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 1122334455ull;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

enum { CAP = 4200 };
typedef struct {
    int a[CAP];
    unsigned char r[CAP];
    int n;
    long cmp;
} Weak;

/* distinguished ancestor of j: climb while j is a "right" child of its parent's orientation */
static int gparent(const Weak *w, int j) {
    while ((j & 1) == w->r[j >> 1])
        j >>= 1;
    return j >> 1;
}

/* i is the distinguished ancestor of j; keep the smaller value in i */
static void join(Weak *w, int i, int j) {
    w->cmp++;
    if (w->a[j] < w->a[i]) {
        int t = w->a[i];
        w->a[i] = w->a[j];
        w->a[j] = t;
        w->r[j] ^= 1;
    }
}

static void wpush(Weak *w, int x) {
    check(w->n < CAP, "capacity");
    int j = w->n++;
    w->a[j] = x;
    if ((j & 1) == 0)
        w->r[j >> 1] = 0;
    while (j > 0) {
        int i = gparent(w, j);
        w->cmp++;
        if (w->a[i] <= w->a[j])
            break;
        int t = w->a[i];
        w->a[i] = w->a[j];
        w->a[j] = t;
        w->r[j] ^= 1;
        j = i;
    }
}

static int wpop(Weak *w) {
    int top = w->a[0];
    w->n--;
    if (w->n == 0)
        return top;
    w->a[0] = w->a[w->n];
    int y = 1;
    while (2 * y + w->r[y] < w->n)
        y = 2 * y + w->r[y];
    while (y > 0) {
        join(w, 0, y);
        y >>= 1;
    }
    return top;
}

static void winv(const Weak *w) {
    for (int j = 1; j < w->n; j++)
        check(w->a[gparent(w, j)] <= w->a[j], "distinguished ancestor is not larger");
}

/* baseline: binary heap sort comparisons (classic sift-down on max-heap) */
static long binary_heapsort_cmps(int *a, int n) {
    long cmp = 0;
    for (int i = n / 2 - 1; i >= 0; i--) {
        int x = a[i], k = i;
        for (;;) {
            int c = 2 * k + 1;
            if (c >= n)
                break;
            if (c + 1 < n) {
                cmp++;
                if (a[c + 1] > a[c])
                    c++;
            }
            cmp++;
            if (a[c] <= x)
                break;
            a[k] = a[c];
            k = c;
        }
        a[k] = x;
    }
    for (int end = n - 1; end > 0; end--) {
        int x = a[end];
        a[end] = a[0];
        int k = 0;
        for (;;) {
            int c = 2 * k + 1;
            if (c >= end)
                break;
            if (c + 1 < end) {
                cmp++;
                if (a[c + 1] > a[c])
                    c++;
            }
            cmp++;
            if (a[c] <= x)
                break;
            a[k] = a[c];
            k = c;
        }
        a[k] = x;
    }
    return cmp;
}

int main(void) {
    static Weak w;
    int model[CAP], mn = 0;
    long sum = 0;
    for (int op = 0; op < 9000; op++) {
        if ((rng() % 100 < 55 || mn == 0) && mn < 2000) {
            int v = (int)(rng() % 3000);
            model[mn++] = v;
            wpush(&w, v);
        } else {
            int bi = 0;
            for (int i = 1; i < mn; i++)
                if (model[i] < model[bi])
                    bi = i;
            int want = model[bi];
            model[bi] = model[--mn];
            int got = wpop(&w);
            check(got == want, "pop equals model");
            sum += got;
        }
        check(w.n == mn, "size");
        winv(&w);
    }
    printf("size=%d popsum=%ld\n", w.n, sum);

    /* sort by draining; compare comparison counts with binary heapsort */
    int sizes[3] = {100, 1000, 4000};
    for (int s = 0; s < 3; s++) {
        int n = sizes[s];
        int *data = malloc(sizeof(int) * (size_t)n), *copy = malloc(sizeof(int) * (size_t)n);
        for (int i = 0; i < n; i++)
            copy[i] = data[i] = (int)(rng() % 1000000);
        static Weak z;
        z.n = 0;
        z.cmp = 0;
        for (int i = 0; i < n; i++)
            wpush(&z, data[i]);
        long ins_cmp = z.cmp;
        int prev = -1;
        for (int i = 0; i < n; i++) {
            int v = wpop(&z);
            check(v >= prev, "weak heap drains sorted");
            prev = v;
        }
        long bin = binary_heapsort_cmps(copy, n);
        for (int i = 1; i < n; i++)
            check(copy[i - 1] <= copy[i], "heapsort sorted");
        printf("n=%-5d weak: insert=%-6ld total=%-7ld binary heapsort total=%ld\n", n, ins_cmp, z.cmp, bin);
        free(data);
        free(copy);
    }
    return 0;
}
