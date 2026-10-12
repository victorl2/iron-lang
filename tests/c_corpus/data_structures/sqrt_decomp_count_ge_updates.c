/*
 * title: Sorted-block decomposition for count-at-least with point updates
 * topic: data_structures
 * covers: sqrt decomposition, per-block sorted copies, binary search in blocks, in-place block repair on update, brute force
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

#define N 1500
#define BS 39

static int a[N];
static int sorted[N]; /* each block of BS entries kept sorted */
static long ops_cost;

static int cmp_int(const void *x, const void *y) {
    int p = *(const int *)x, q = *(const int *)y;
    return (p > q) - (p < q);
}

static int blk_lo(int b) { return b * BS; }
static int blk_hi(int b) { return blk_lo(b) + BS < N ? blk_lo(b) + BS : N; }

static void build(void) {
    memcpy(sorted, a, sizeof a);
    for (int b = 0; b * BS < N; b++)
        qsort(sorted + blk_lo(b), (size_t)(blk_hi(b) - blk_lo(b)), sizeof(int), cmp_int);
}

/* replace a[i] by v, repairing the sorted copy by shifting: O(BS) */
static void assign(int i, int v) {
    int b = i / BS, lo = blk_lo(b), hi = blk_hi(b);
    int old = a[i];
    int p = lo;
    while (sorted[p] != old)
        p++;
    a[i] = v;
    if (v > old) {
        while (p + 1 < hi && sorted[p + 1] < v) {
            sorted[p] = sorted[p + 1];
            p++;
            ops_cost++;
        }
    } else {
        while (p > lo && sorted[p - 1] > v) {
            sorted[p] = sorted[p - 1];
            p--;
            ops_cost++;
        }
    }
    sorted[p] = v;
}

static int count_ge_block(int b, int x) {
    int lo = blk_lo(b), hi = blk_hi(b), first = hi;
    int l = lo, r = hi;
    while (l < r) {
        int mid = (l + r) / 2;
        if (sorted[mid] >= x)
            r = mid;
        else
            l = mid + 1;
        ops_cost++;
    }
    first = l;
    return hi - first;
}

static int count_ge(int l, int r, int x) { /* inclusive */
    int bl = l / BS, br = r / BS, res = 0;
    if (bl == br) {
        for (int i = l; i <= r; i++)
            res += a[i] >= x;
        return res;
    }
    for (int i = l; i < blk_hi(bl); i++)
        res += a[i] >= x;
    for (int b = bl + 1; b < br; b++)
        res += count_ge_block(b, x);
    for (int i = blk_lo(br); i <= r; i++)
        res += a[i] >= x;
    return res;
}

int main(void) {
    for (int i = 0; i < N; i++)
        a[i] = (int)rnd(10000);
    build();
    long digest = 0;
    int nq = 0, nu = 0;
    for (int step = 0; step < 6000; step++) {
        if (rnd(3) == 0) {
            int i = (int)rnd(N);
            int v = (int)rnd(10000);
            assign(i, v);
            nu++;
            continue;
        }
        int l = (int)rnd(N), r = (int)rnd(N);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        int x = (int)rnd(10000);
        int want = 0;
        for (int i = l; i <= r; i++)
            want += a[i] >= x;
        int got = count_ge(l, r, x);
        check(got == want, "count at least");
        digest += got;
        nq++;
    }
    for (int b = 0; b * BS < N; b++)
        for (int i = blk_lo(b) + 1; i < blk_hi(b); i++)
            check(sorted[i - 1] <= sorted[i], "block stays sorted");
    printf("updates=%d queries=%d digest=%ld work=%ld\n", nu, nq, digest, ops_cost);
    printf("threshold sweep over whole array:");
    for (int x = 0; x <= 10000; x += 2500)
        printf(" >=%d:%d", x, count_ge(0, N - 1, x));
    printf("\n");
    return 0;
}
