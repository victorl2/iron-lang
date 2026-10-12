/*
 * title: Merge sort tree for range counting and k-th
 * topic: data_structures
 * covers: merge sort tree, sorted node arrays, binary search per node, value-range counting, k-th smallest by value bisection
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

typedef struct {
    int *v;
    int len;
} Node;

#define MAXN 1024
static Node tr[4 * MAXN];
static long total_words;

static void build(int o, int l, int r, const int *a) {
    tr[o].len = r - l + 1;
    tr[o].v = malloc(sizeof(int) * (size_t)tr[o].len);
    check(tr[o].v != NULL, "alloc");
    total_words += tr[o].len;
    if (l == r) {
        tr[o].v[0] = a[l];
        return;
    }
    int m = (l + r) / 2;
    build(2 * o, l, m, a);
    build(2 * o + 1, m + 1, r, a);
    const int *x = tr[2 * o].v, *y = tr[2 * o + 1].v;
    int nx = tr[2 * o].len, ny = tr[2 * o + 1].len;
    int i = 0, j = 0, k = 0;
    while (i < nx && j < ny)
        tr[o].v[k++] = x[i] <= y[j] ? x[i++] : y[j++];
    while (i < nx)
        tr[o].v[k++] = x[i++];
    while (j < ny)
        tr[o].v[k++] = y[j++];
}

static void destroy(int o, int l, int r) {
    free(tr[o].v);
    if (l == r)
        return;
    int m = (l + r) / 2;
    destroy(2 * o, l, m);
    destroy(2 * o + 1, m + 1, r);
}

/* number of elements < x in a sorted array */
static int lower(const int *v, int len, int x) {
    int lo = 0, hi = len;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (v[mid] < x)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static long probes;

static int count_less(int o, int l, int r, int ql, int qr, int x) {
    if (qr < l || r < ql)
        return 0;
    if (ql <= l && r <= qr) {
        probes++;
        return lower(tr[o].v, tr[o].len, x);
    }
    int m = (l + r) / 2;
    return count_less(2 * o, l, m, ql, qr, x) + count_less(2 * o + 1, m + 1, r, ql, qr, x);
}

/* k-th smallest (1-based) in [ql,qr] by bisecting on the value */
static int kth(int n, int ql, int qr, int k) {
    int lo = 0, hi = 100000;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (count_less(1, 0, n - 1, ql, qr, mid + 1) >= k)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void) {
    int n = 700;
    int a[MAXN];
    for (int i = 0; i < n; i++) {
        int hi = (int)rnd(1000);
        int lo = (int)rnd(100);
        a[i] = hi * lo % 100000;
    }
    build(1, 0, n - 1, a);
    printf("n=%d stored words=%ld (n*levels)\n", n, total_words);
    long lsum = 0, ksum = 0, rsum = 0;
    for (int q = 0; q < 1500; q++) {
        int l = (int)rnd((unsigned)n), r = (int)rnd((unsigned)n);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        int x = (int)rnd(100001);
        int want = 0;
        for (int i = l; i <= r; i++)
            want += a[i] < x;
        int got = count_less(1, 0, n - 1, l, r, x);
        check(got == want, "count less");
        lsum += got;
        /* value range count [lo, hi] */
        int lo = (int)rnd(50000), hi = lo + (int)rnd(50000);
        int wr = 0;
        for (int i = l; i <= r; i++)
            wr += a[i] >= lo && a[i] <= hi;
        int gr = count_less(1, 0, n - 1, l, r, hi + 1) - count_less(1, 0, n - 1, l, r, lo);
        check(gr == wr, "range count");
        rsum += gr;
        if (q % 5 == 0) {
            int len = r - l + 1;
            int *tmp = malloc(sizeof(int) * (size_t)len);
            check(tmp != NULL, "alloc");
            memcpy(tmp, a + l, sizeof(int) * (size_t)len);
            qsort(tmp, (size_t)len, sizeof(int), cmp_int);
            int k = 1 + (int)rnd((unsigned)len);
            int gk = kth(n, l, r, k);
            check(gk == tmp[k - 1], "kth smallest");
            ksum += gk;
            free(tmp);
        }
    }
    printf("less sum=%ld range sum=%ld kth sum=%ld probes=%ld\n", lsum, rsum, ksum, probes);
    destroy(1, 0, n - 1);
    return 0;
}
