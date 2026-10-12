/*
 * title: Eytzinger (BFS-order) implicit BST with branch-free lower bound
 * topic: data_structures
 * covers: implicit binary search tree, Eytzinger layout, in-order fill, branch-free descent, trailing-ones recovery of lower bound, duplicates, comparison counts versus binary search, exhaustive small sizes
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 2048

static unsigned long long rs = 88172645463325252ULL;
unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}
void check(int c, const char *w) {
    if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); }
}
void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) exit(2);
    return p;
}

#define NM 1100

static int E[NM + 1];       /* 1-based, children of i are 2i and 2i+1 */
static int rk[NM + 1];      /* sorted rank of the value stored at each slot */
static int cnt_iter;

static int fill(const int *a, int n, int i, int k) {
    if (i <= n) {
        k = fill(a, n, 2 * i, k);
        E[i] = a[k]; rk[i] = k; k++;
        k = fill(a, n, 2 * i + 1, k);
    }
    return k;
}
/* returns the sorted index of the first element >= x, or n if none */
static int lower_bound_eytz(int n, int x, int *iters) {
    int i = 1, it = 0;
    while (i <= n) {
        i = 2 * i + (E[i] < x);
        it++;
    }
    while (i & 1) i >>= 1;      /* strip the trailing right turns */
    i >>= 1;
    *iters = it;
    return i == 0 ? n : rk[i];
}
static int lower_bound_sorted(const int *a, int n, int x, int *iters) {
    int lo = 0, hi = n, it = 0;
    while (lo < hi) {
        int m = lo + (hi - lo) / 2;
        it++;
        if (a[m] < x) lo = m + 1; else hi = m;
    }
    *iters = it;
    return lo;
}
static int height(int n) { int h = 0; while (n) { h++; n >>= 1; } return h; }
static void inorder_e(int n, int i, int *out, int *k) {
    if (i > n) return;
    inorder_e(n, 2 * i, out, k);
    out[(*k)++] = E[i];
    inorder_e(n, 2 * i + 1, out, k);
}

int main(void) {
    static int a[NM], tmp[NM];
    /* every size from 1 to 200, with duplicates, every probe value */
    long probes = 0;
    for (int n = 1; n <= 200; n++) {
        int v = 0;
        for (int i = 0; i < n; i++) { v += (int)(rnd() % 3); a[i] = v; }     /* non-decreasing, many duplicates */
        int used = fill(a, n, 1, 0);
        check(used == n, "fill consumed all");
        int k = 0;
        inorder_e(n, 1, tmp, &k);
        check(k == n && !memcmp(tmp, a, sizeof(int) * (size_t)n), "inorder of layout is sorted array");
        for (int x = a[0] - 1; x <= a[n - 1] + 1; x++) {
            int it1, it2;
            int e = lower_bound_eytz(n, x, &it1), s = lower_bound_sorted(a, n, x, &it2);
            check(e == s, "lower bound matches binary search");
            check(it1 <= height(n), "descent length bounded by height");
            probes++;
        }
    }
    printf("exhaustive sizes 1..200: %ld probes agree with binary search\n", probes);
    /* larger array: iteration counts */
    int n = 1000;
    for (int i = 0; i < n; i++) a[i] = i * 3 + (int)(rnd() % 3);
    fill(a, n, 1, 0);
    long it_e = 0, it_b = 0;
    int found = 0;
    for (int q = 0; q < 5000; q++) {
        int x = (int)(rnd() % 3100), i1, i2;
        int e = lower_bound_eytz(n, x, &i1), s = lower_bound_sorted(a, n, x, &i2);
        check(e == s, "large lower bound");
        it_e += i1; it_b += i2;
        found += e < n && a[e] == x;
    }
    printf("n=%d height=%d: 5000 queries, eytzinger iterations=%ld, binary search iterations=%ld, exact hits=%d\n",
           n, height(n), it_e, it_b, found);
    /* layout facts: level sizes and where the smallest and largest values live */
    int mn_i = 1, mx_i = 1;
    while (2 * mn_i <= n) mn_i *= 2;
    for (int i = 1; i <= n; i++) { if (E[i] < E[mn_i]) mn_i = i; if (E[i] > E[mx_i]) mx_i = i; }
    printf("root value=%d, smallest at slot %d, largest at slot %d\n", E[1], mn_i, mx_i);
    int leaf_slots = 0;
    for (int i = 1; i <= n; i++) if (2 * i > n) leaf_slots++;
    printf("leaf slots=%d of %d\n", leaf_slots, n);
    check(leaf_slots == (n + 1) / 2, "half the slots are leaves");
    (void)cnt_iter;
    return 0;
}
