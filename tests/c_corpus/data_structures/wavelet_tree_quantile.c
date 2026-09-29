/*
 * title: Pointer wavelet tree over a value range
 * topic: data_structures
 * covers: wavelet tree, stable partition by value midpoint, prefix left-counts, k-th smallest in range, rank of value, range frequency, access
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

typedef struct WNode {
    int lo, hi;      /* value range covered by this node */
    int *left_cnt;   /* left_cnt[i] = elements among the first i that go left */
    struct WNode *l, *r;
} WNode;

static int nodes_live;

static WNode *build(int *a, int n, int lo, int hi, int *tmp) {
    WNode *w = calloc(1, sizeof *w);
    check(w != NULL, "alloc");
    nodes_live++;
    w->lo = lo;
    w->hi = hi;
    if (lo == hi || n == 0)
        return w;
    int mid = lo + (hi - lo) / 2;
    w->left_cnt = malloc(sizeof(int) * (size_t)(n + 1));
    check(w->left_cnt != NULL, "alloc");
    w->left_cnt[0] = 0;
    int nl = 0;
    for (int i = 0; i < n; i++) {
        if (a[i] <= mid)
            nl++;
        w->left_cnt[i + 1] = nl;
    }
    /* stable partition into tmp */
    int pl = 0, pr = nl;
    for (int i = 0; i < n; i++) {
        if (a[i] <= mid)
            tmp[pl++] = a[i];
        else
            tmp[pr++] = a[i];
    }
    memcpy(a, tmp, sizeof(int) * (size_t)n);
    w->l = build(a, nl, lo, mid, tmp);
    w->r = build(a + nl, n - nl, mid + 1, hi, tmp);
    return w;
}

static void destroy(WNode *w) {
    if (!w)
        return;
    destroy(w->l);
    destroy(w->r);
    free(w->left_cnt);
    free(w);
    nodes_live--;
}

/* k-th smallest (1-based) among positions [l, r) */
static int kth(const WNode *w, int l, int r, int k) {
    while (w->lo != w->hi) {
        int cl = w->left_cnt[l], cr = w->left_cnt[r];
        int inleft = cr - cl;
        if (k <= inleft) {
            l = cl;
            r = cr;
            w = w->l;
        } else {
            k -= inleft;
            l -= cl;
            r -= cr;
            w = w->r;
        }
    }
    return w->lo;
}

/* occurrences of value v among positions [l, r) */
static int rank_of(const WNode *w, int l, int r, int v) {
    while (w && w->lo != w->hi) {
        int mid = w->lo + (w->hi - w->lo) / 2;
        int cl = w->left_cnt ? w->left_cnt[l] : 0, cr = w->left_cnt ? w->left_cnt[r] : 0;
        if (v <= mid) {
            l = cl;
            r = cr;
            w = w->l;
        } else {
            l -= cl;
            r -= cr;
            w = w->r;
        }
    }
    return w ? r - l : 0;
}

/* elements with value <= x among positions [l, r) */
static int count_le(const WNode *w, int l, int r, int x) {
    if (!w || l >= r || x < w->lo)
        return 0;
    if (w->hi <= x)
        return r - l;
    int cl = w->left_cnt[l], cr = w->left_cnt[r];
    return count_le(w->l, cl, cr, x) + count_le(w->r, l - cl, r - cr, x);
}

static int access(const WNode *w, int i) {
    int l = i, r = i + 1;
    return kth(w, l, r, 1);
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void) {
    enum { N = 1500, VMAX = 400 };
    static int a[N], work[N], tmp[N];
    for (int i = 0; i < N; i++)
        a[i] = 1 + (int)rnd(VMAX);
    memcpy(work, a, sizeof a);
    WNode *w = build(work, N, 1, VMAX, tmp);
    for (int i = 0; i < N; i += 7)
        check(access(w, i) == a[i], "access");
    long ksum = 0, rsum = 0, csum = 0;
    for (int q = 0; q < 2000; q++) {
        int l = (int)rnd(N), r = (int)rnd(N + 1);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        if (l == r)
            continue;
        int len = r - l;
        int sorted[N];
        memcpy(sorted, a + l, sizeof(int) * (size_t)len);
        qsort(sorted, (size_t)len, sizeof(int), cmp_int);
        int k = 1 + (int)rnd((unsigned)len);
        check(kth(w, l, r, k) == sorted[k - 1], "kth");
        ksum += sorted[k - 1];
        int v = 1 + (int)rnd(VMAX);
        int want = 0;
        for (int i = 0; i < len; i++)
            want += sorted[i] == v;
        check(rank_of(w, l, r, v) == want, "rank");
        rsum += want;
        int x = (int)rnd(VMAX + 20);
        want = 0;
        for (int i = 0; i < len; i++)
            want += sorted[i] <= x;
        check(count_le(w, l, r, x) == want, "count_le");
        csum += want;
    }
    printf("n=%d values=1..%d nodes=%d\n", N, VMAX, nodes_live);
    printf("kth digest=%ld rank digest=%ld count digest=%ld\n", ksum, rsum, csum);
    int med = kth(w, 0, N, N / 2);
    printf("global median=%d min=%d max=%d\n", med, kth(w, 0, N, 1), kth(w, 0, N, N));
    destroy(w);
    check(nodes_live == 0, "no leak");
    return 0;
}
