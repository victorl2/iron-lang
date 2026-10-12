/*
 * title: Staircase search in a row and column sorted matrix
 * topic: algorithms
 * covers: saddleback search, O(m+n) walk, count of elements below threshold, sorted-both-ways matrix, brute-force cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned long long rs = 99991ull;
static unsigned rnd(void) {
    rs = rs * 6364136223846793005ull + 1;
    return (unsigned)(rs >> 40);
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

enum { R = 30, C = 40 };
static int g[R][C];

static long steps;

/* start at top-right: move left when too big, down when too small */
static int staircase(int key, int *pr, int *pc) {
    int r = 0, c = C - 1;
    while (r < R && c >= 0) {
        steps++;
        if (g[r][c] == key) {
            *pr = r;
            *pc = c;
            return 1;
        }
        if (g[r][c] > key)
            c--;
        else
            r++;
    }
    return 0;
}

/* number of cells with value <= x, in O(R + C) */
static int count_le(int x) {
    int cnt = 0, c = C - 1;
    for (int r = 0; r < R; r++) {
        while (c >= 0 && g[r][c] > x)
            c--;
        cnt += c + 1;
    }
    return cnt;
}

/* k-th smallest (1-based) by binary search on value + count_le */
static int kth_smallest(int k) {
    int lo = g[0][0], hi = g[R - 1][C - 1];
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (count_le(mid) >= k)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    /* g[r][c] = (r*5 + c*3) + noise that preserves monotonic rows and columns */
    for (int r = 0; r < R; r++)
        for (int c = 0; c < C; c++) {
            int up = r ? g[r - 1][c] : 0, left = c ? g[r][c - 1] : 0;
            int base = up > left ? up : left;
            g[r][c] = base + (int)(rnd() % 5);
        }
    for (int r = 0; r < R; r++)
        for (int c = 0; c < C; c++) {
            if (c && g[r][c] < g[r][c - 1])
                fail("row order");
            if (r && g[r][c] < g[r - 1][c])
                fail("col order");
        }
    int hi = g[R - 1][C - 1];
    int found = 0;
    steps = 0;
    for (int k = -1; k <= hi + 1; k++) {
        int r, c, w = 0;
        int f = staircase(k, &r, &c);
        for (int i = 0; i < R && !w; i++)
            for (int j = 0; j < C; j++)
                if (g[i][j] == k) {
                    w = 1;
                    break;
                }
        if (f != w || (f && g[r][c] != k))
            fail("staircase");
        found += f;
    }
    printf("max value %d, present keys %d, total walk steps %ld\n", hi, found, steps);
    printf("step bound per query (R+C)=%d\n", R + C);

    int all[R * C];
    for (int i = 0; i < R * C; i++)
        all[i] = g[i / C][i % C];
    qsort(all, R * C, sizeof all[0], cmp_int);
    int ks[] = {1, 2, 100, 600, 1199, 1200};
    for (int i = 0; i < 6; i++) {
        int v = kth_smallest(ks[i]);
        if (v != all[ks[i] - 1])
            fail("kth");
        printf("k=%-5d kth smallest=%d\n", ks[i], v);
    }
    printf("count_le(0)=%d count_le(median)=%d count_le(max)=%d\n", count_le(0),
           count_le(all[599]), count_le(hi));
    return 0;
}
