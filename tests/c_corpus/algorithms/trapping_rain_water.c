/*
 * title: Trapping rain water four ways
 * topic: algorithms
 * covers: two pointers with running maxima, prefix/suffix maxima, monotonic stack, per-cell definition, agreement across methods
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 1u << 20 | 7u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

static long by_cells(const int *h, int n) {
    long total = 0;
    for (int i = 0; i < n; i++) {
        int l = 0, r = 0;
        for (int j = 0; j <= i; j++)
            if (h[j] > l)
                l = h[j];
        for (int j = i; j < n; j++)
            if (h[j] > r)
                r = h[j];
        total += (l < r ? l : r) - h[i];
    }
    return total;
}

static long by_prefix(const int *h, int n) {
    int *lm = malloc((size_t)n * sizeof(int)), *rm = malloc((size_t)n * sizeof(int));
    if (!lm || !rm)
        fail("alloc");
    long total = 0;
    for (int i = 0; i < n; i++)
        lm[i] = (i && lm[i - 1] > h[i]) ? lm[i - 1] : h[i];
    for (int i = n - 1; i >= 0; i--)
        rm[i] = (i < n - 1 && rm[i + 1] > h[i]) ? rm[i + 1] : h[i];
    for (int i = 0; i < n; i++)
        total += (lm[i] < rm[i] ? lm[i] : rm[i]) - h[i];
    free(lm);
    free(rm);
    return total;
}

static long by_two_pointers(const int *h, int n) {
    int l = 0, r = n - 1, lmax = 0, rmax = 0;
    long total = 0;
    while (l < r) {
        if (h[l] < h[r]) {
            if (h[l] >= lmax)
                lmax = h[l];
            else
                total += lmax - h[l];
            l++;
        } else {
            if (h[r] >= rmax)
                rmax = h[r];
            else
                total += rmax - h[r];
            r--;
        }
    }
    return total;
}

static long by_stack(const int *h, int n) {
    int *stk = malloc((size_t)n * sizeof(int));
    if (!stk)
        fail("alloc");
    int top = 0;
    long total = 0;
    for (int i = 0; i < n; i++) {
        while (top > 0 && h[i] > h[stk[top - 1]]) {
            int mid = stk[--top];
            if (top == 0)
                break;
            int left = stk[top - 1];
            int bound = (h[left] < h[i] ? h[left] : h[i]) - h[mid];
            total += (long)bound * (i - left - 1);
        }
        stk[top++] = i;
    }
    free(stk);
    return total;
}

int main(void) {
    int h[128];
    long checksum = 0;
    for (int trial = 0; trial < 400; trial++) {
        int n = 1 + (int)(rnd() % 100);
        int cap = 1 + (int)(rnd() % 12);
        for (int i = 0; i < n; i++)
            h[i] = (int)(rnd() % (unsigned)(cap + 1));
        long a = by_cells(h, n), b = by_prefix(h, n), c = by_two_pointers(h, n), d = by_stack(h, n);
        if (a != b || b != c || c != d)
            fail("methods disagree");
        checksum += a;
    }
    printf("400 random terrains, water checksum %ld\n", checksum);

    int classic[] = {0, 1, 0, 2, 1, 0, 1, 3, 2, 1, 2, 1};
    printf("classic: %ld\n", by_two_pointers(classic, 12));
    int bowl[] = {5, 4, 3, 2, 1, 2, 3, 4, 5};
    printf("bowl: %ld\n", by_stack(bowl, 9));
    int hill[] = {1, 2, 3, 4, 5};
    printf("monotone hill: %ld\n", by_prefix(hill, 5));
    int wells[] = {9, 0, 9, 0, 0, 9, 1, 8};
    printf("wells: %ld\n", by_cells(wells, 8));
    return 0;
}
