/*
 * title: Batcher odd-even merge sort network
 * topic: algorithms
 * covers: sorting networks, odd-even merge, comparator lists, layer scheduling, exhaustive 0-1 test
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int i, j;
} Cmpr;

static Cmpr net[4096];
static int ncmp;

static void add(int i, int j) {
    if (ncmp >= 4096) {
        fprintf(stderr, "network too large\n");
        exit(1);
    }
    net[ncmp].i = i;
    net[ncmp].j = j;
    ncmp++;
}

/* Merge two sorted halves of a[lo..lo+n) where elements are r apart. */
static void oe_merge(int lo, int n, int r) {
    int m = r * 2;
    if (m < n) {
        oe_merge(lo, n, m);
        oe_merge(lo + r, n, m);
        for (int i = lo + r; i + r < lo + n; i += m)
            add(i, i + r);
    } else {
        add(lo, lo + r);
    }
}

static void oe_sort(int lo, int n) {
    if (n > 1) {
        int m = n / 2;
        oe_sort(lo, m);
        oe_sort(lo + m, m);
        oe_merge(lo, n, 1);
    }
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static int depth_of(int n) {
    int level[64] = {0}, maxd = 0;
    for (int k = 0; k < ncmp; k++) {
        int d = (level[net[k].i] > level[net[k].j] ? level[net[k].i] : level[net[k].j]) + 1;
        level[net[k].i] = level[net[k].j] = d;
        if (d > maxd)
            maxd = d;
    }
    (void)n;
    return maxd;
}

static int sorts_all_binary(int n) {
    for (unsigned mask = 0; mask < (1u << n); mask++) {
        unsigned v[32];
        for (int i = 0; i < n; i++)
            v[i] = (mask >> i) & 1u;
        for (int k = 0; k < ncmp; k++)
            if (v[net[k].i] > v[net[k].j]) {
                unsigned t = v[net[k].i];
                v[net[k].i] = v[net[k].j];
                v[net[k].j] = t;
            }
        for (int i = 1; i < n; i++)
            if (v[i - 1] > v[i])
                return 0;
    }
    return 1;
}

int main(void) {
    static const int expect_cmp[] = {1, 5, 19, 63}; /* known sizes for n = 2,4,8,16 */
    int idx = 0;
    for (int n = 2; n <= 16; n *= 2, idx++) {
        ncmp = 0;
        oe_sort(0, n);
        check(ncmp == expect_cmp[idx], "known comparator count");
        check(sorts_all_binary(n), "0-1 principle exhaustive check");
        printf("n=%-2d comparators=%-3d depth=%d exhaustive_ok=1\n", n, ncmp, depth_of(n));
    }
    /* print the 8-input network */
    ncmp = 0;
    oe_sort(0, 8);
    printf("n=8 network:");
    for (int k = 0; k < ncmp; k++)
        printf(" (%d,%d)", net[k].i, net[k].j);
    printf("\n");
    /* apply to a permutation */
    int p[8] = {5, 2, 7, 0, 6, 1, 4, 3};
    for (int k = 0; k < ncmp; k++)
        if (p[net[k].i] > p[net[k].j]) {
            int t = p[net[k].i];
            p[net[k].i] = p[net[k].j];
            p[net[k].j] = t;
        }
    for (int i = 0; i < 8; i++)
        check(p[i] == i, "permutation sorted");
    printf("applied to permutation: ok\n");
    return 0;
}
