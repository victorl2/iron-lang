/*
 * title: In-place compaction with read and write pointers
 * topic: algorithms
 * covers: same-direction two pointers, stable remove, dedupe of sorted data, move zeroes, Dutch national flag, at-most-twice duplicates
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 13u * 1000003u;
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
static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

static int remove_value(int *a, int n, int val) {
    int w = 0;
    for (int r = 0; r < n; r++)
        if (a[r] != val)
            a[w++] = a[r];
    return w;
}

/* sorted input: keep each value at most `limit` times */
static int dedupe(int *a, int n, int limit) {
    int w = 0;
    for (int r = 0; r < n; r++)
        if (w < limit || a[r] != a[w - limit])
            a[w++] = a[r];
    return w;
}

/* stable: non-zeros keep relative order, zeros go to the back; returns writes performed */
static int move_zeroes(int *a, int n) {
    int w = 0, writes = 0;
    for (int r = 0; r < n; r++)
        if (a[r] != 0) {
            if (w != r) {
                int t = a[w];
                a[w] = a[r];
                a[r] = t;
                writes++;
            }
            w++;
        }
    return writes;
}

/* sort an array of 0/1/2 in one pass */
static void dutch_flag(int *a, int n) {
    int lo = 0, mid = 0, hi = n - 1;
    while (mid <= hi) {
        if (a[mid] == 0) {
            int t = a[lo];
            a[lo++] = a[mid];
            a[mid++] = t;
        } else if (a[mid] == 2) {
            int t = a[hi];
            a[hi--] = a[mid];
            a[mid] = t;
        } else
            mid++;
    }
}

static void show(const char *label, const int *a, int n) {
    printf("%-22s", label);
    for (int i = 0; i < n; i++)
        printf(" %d", a[i]);
    printf("\n");
}

int main(void) {
    int a[100], b[100];
    for (int trial = 0; trial < 400; trial++) {
        int n = (int)(rnd() % 80);
        for (int i = 0; i < n; i++)
            a[i] = (int)(rnd() % 6);
        memcpy(b, a, sizeof(int) * (size_t)n);
        int val = (int)(rnd() % 6);
        int w = remove_value(b, n, val), e = 0;
        for (int i = 0; i < n; i++)
            if (a[i] != val) {
                if (b[e] != a[i])
                    fail("remove order");
                e++;
            }
        if (w != e)
            fail("remove count");

        memcpy(b, a, sizeof(int) * (size_t)n);
        move_zeroes(b, n);
        int seen_zero = 0, nz = 0, ai = 0;
        for (int i = 0; i < n; i++) {
            if (b[i] == 0)
                seen_zero = 1;
            else if (seen_zero)
                fail("zero before nonzero");
            else {
                while (a[ai] == 0)
                    ai++;
                if (b[i] != a[ai++])
                    fail("stability");
                nz++;
            }
        }
        (void)nz;

        memcpy(b, a, sizeof(int) * (size_t)n);
        for (int i = 0; i < n; i++)
            b[i] %= 3;
        dutch_flag(b, n);
        for (int i = 1; i < n; i++)
            if (b[i - 1] > b[i])
                fail("flag order");

        memcpy(b, a, sizeof(int) * (size_t)n);
        qsort(b, (size_t)n, sizeof(int), cmp_int);
        for (int limit = 1; limit <= 3; limit++) {
            int c[100];
            memcpy(c, b, sizeof(int) * (size_t)n);
            int m = dedupe(c, n, limit);
            for (int v = 0; v < 6; v++) {
                int have = 0, want = 0;
                for (int i = 0; i < m; i++)
                    have += c[i] == v;
                for (int i = 0; i < n; i++)
                    want += b[i] == v;
                if (want > limit)
                    want = limit;
                if (have != want)
                    fail("dedupe");
            }
        }
    }
    printf("400 random arrays verified\n");

    int d1[] = {3, 2, 2, 3};
    int m = remove_value(d1, 4, 3);
    show("remove 3 from 3,2,2,3:", d1, m);
    int d2[] = {0, 1, 0, 3, 12};
    int wr = move_zeroes(d2, 5);
    show("move zeroes:", d2, 5);
    printf("swaps performed: %d\n", wr);
    int d3[] = {1, 1, 1, 2, 2, 3};
    m = dedupe(d3, 6, 2);
    show("at most twice:", d3, m);
    int d4[] = {2, 0, 2, 1, 1, 0};
    dutch_flag(d4, 6);
    show("dutch flag:", d4, 6);
    int d5[] = {0, 0, 1, 1, 1, 2, 2, 3, 3, 3, 3};
    m = dedupe(d5, 11, 1);
    show("unique:", d5, m);
    return 0;
}
