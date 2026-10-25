/*
 * title: Persistent segment tree for range k-th smallest
 * topic: data_structures
 * covers: persistent segment tree, path copying, coordinate compression, version difference descent, node pool
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

#define MAXN 2000
#define POOL (MAXN * 14 + 10)

typedef struct {
    int l, r, cnt;
} PNode;

static PNode pool[POOL]; /* node 0 is the shared empty tree */
static int used = 1;

static int insert(int prev, int lo, int hi, int pos) {
    int cur = used++;
    check(cur < POOL, "pool");
    pool[cur] = pool[prev];
    pool[cur].cnt++;
    if (lo < hi) {
        int mid = (lo + hi) / 2;
        if (pos <= mid)
            pool[cur].l = insert(pool[prev].l, lo, mid, pos);
        else
            pool[cur].r = insert(pool[prev].r, mid + 1, hi, pos);
    }
    return cur;
}

/* k-th (1-based) smallest compressed value in version b minus version a */
static int kth(int a, int b, int lo, int hi, int k) {
    if (lo == hi)
        return lo;
    int mid = (lo + hi) / 2;
    int left = pool[pool[b].l].cnt - pool[pool[a].l].cnt;
    if (k <= left)
        return kth(pool[a].l, pool[b].l, lo, mid, k);
    return kth(pool[a].r, pool[b].r, mid + 1, hi, k - left);
}

/* elements with compressed value <= pos */
static int count_le(int a, int b, int lo, int hi, int pos) {
    if (pos < lo)
        return 0;
    if (hi <= pos)
        return pool[b].cnt - pool[a].cnt;
    int mid = (lo + hi) / 2;
    return count_le(pool[a].l, pool[b].l, lo, mid, pos) +
           count_le(pool[a].r, pool[b].r, mid + 1, hi, pos);
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    int n = MAXN;
    static int a[MAXN], sorted[MAXN], comp[MAXN];
    for (int i = 0; i < n; i++)
        a[i] = (int)rnd(1000000) - 500000;
    memcpy(sorted, a, sizeof a);
    qsort(sorted, (size_t)n, sizeof(int), cmp_int);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (i == 0 || sorted[i] != sorted[i - 1])
            sorted[m++] = sorted[i];
    for (int i = 0; i < n; i++) {
        int lo = 0, hi = m - 1;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (sorted[mid] < a[i])
                lo = mid + 1;
            else
                hi = mid;
        }
        comp[i] = lo;
    }
    static int root[MAXN + 1];
    root[0] = 0;
    for (int i = 0; i < n; i++)
        root[i + 1] = insert(root[i], 0, m - 1, comp[i]);
    printf("n=%d distinct=%d nodes used=%d\n", n, m, used);
    long ksum = 0, csum = 0;
    static int tmp[MAXN];
    for (int q = 0; q < 1200; q++) {
        int l = (int)rnd((unsigned)n), r = (int)rnd((unsigned)n);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        int len = r - l + 1;
        memcpy(tmp, a + l, sizeof(int) * (size_t)len);
        qsort(tmp, (size_t)len, sizeof(int), cmp_int);
        int k = 1 + (int)rnd((unsigned)len);
        int got = sorted[kth(root[l], root[r + 1], 0, m - 1, k)];
        check(got == tmp[k - 1], "kth in range");
        ksum += got;
        int probe = (int)rnd(1000000) - 500000;
        int want = 0;
        for (int i = 0; i < len; i++)
            want += tmp[i] <= probe;
        int pos = -1; /* largest compressed index with sorted[pos] <= probe */
        int lo = 0, hi = m - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (sorted[mid] <= probe) {
                pos = mid;
                lo = mid + 1;
            } else
                hi = mid - 1;
        }
        int gc = count_le(root[l], root[r + 1], 0, m - 1, pos);
        check(gc == want, "count le");
        csum += gc;
    }
    printf("kth sum=%ld count sum=%ld\n", ksum, csum);
    printf("median of whole array=%d min=%d max=%d\n", sorted[kth(root[0], root[n], 0, m - 1, (n + 1) / 2)],
           sorted[kth(root[0], root[n], 0, m - 1, 1)], sorted[kth(root[0], root[n], 0, m - 1, n)]);
    return 0;
}
