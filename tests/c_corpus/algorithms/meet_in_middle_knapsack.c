/*
 * title: Meet in the middle 0/1 knapsack with huge weights
 * topic: algorithms
 * covers: meet in the middle, Pareto-pruned half lists, binary search join, struct sorting with total-order comparator, DP cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned long long rs = 0xC0FFEEull;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 24);
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

typedef struct {
    long w, v;
} Item;

static int cmp_item(const void *x, const void *y) {
    const Item *a = x, *b = y;
    if (a->w != b->w)
        return a->w < b->w ? -1 : 1;
    if (a->v != b->v)
        return a->v > b->v ? -1 : 1; /* higher value first for equal weight */
    return 0;
}

static Item *half_list(const Item *it, int n, size_t *count) {
    size_t total = (size_t)1 << n;
    Item *l = malloc(total * sizeof(Item));
    if (!l)
        fail("alloc");
    for (size_t m = 0; m < total; m++) {
        Item s = {0, 0};
        for (int i = 0; i < n; i++)
            if (m >> i & 1) {
                s.w += it[i].w;
                s.v += it[i].v;
            }
        l[m] = s;
    }
    qsort(l, total, sizeof(Item), cmp_item);
    /* prune dominated entries: keep only strictly increasing value as weight grows */
    size_t k = 0;
    for (size_t i = 0; i < total; i++)
        if (k == 0 || l[i].v > l[k - 1].v)
            l[k++] = l[i];
    *count = k;
    return l;
}

static long knapsack_mitm(const Item *it, int n, long cap, size_t *sa, size_t *sb) {
    int h = n / 2;
    size_t na, nb;
    Item *A = half_list(it, h, &na), *B = half_list(it + h, n - h, &nb);
    long best = 0;
    for (size_t i = 0; i < na; i++) {
        if (A[i].w > cap)
            break;
        long room = cap - A[i].w;
        /* largest index in B with weight <= room */
        size_t lo = 0, hi = nb;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (B[mid].w <= room)
                lo = mid + 1;
            else
                hi = mid;
        }
        long v = A[i].v + (lo ? B[lo - 1].v : 0);
        if (v > best)
            best = v;
    }
    *sa = na;
    *sb = nb;
    free(A);
    free(B);
    return best;
}

static long knapsack_dp(const Item *it, int n, long cap) {
    long *dp = calloc((size_t)cap + 1, sizeof(long));
    if (!dp)
        fail("alloc");
    for (int i = 0; i < n; i++)
        for (long c = cap; c >= it[i].w; c--)
            if (dp[c - it[i].w] + it[i].v > dp[c])
                dp[c] = dp[c - it[i].w] + it[i].v;
    long r = dp[cap];
    free(dp);
    return r;
}

int main(void) {
    Item it[40];
    for (int trial = 0; trial < 25; trial++) {
        int n = 2 + (int)(rnd() % 16);
        long total = 0;
        for (int i = 0; i < n; i++) {
            it[i].w = 1 + (long)(rnd() % 60);
            it[i].v = 1 + (long)(rnd() % 100);
            total += it[i].w;
        }
        long cap = total / 3 + (long)(rnd() % 20);
        size_t sa, sb;
        long m = knapsack_mitm(it, n, cap, &sa, &sb), d = knapsack_dp(it, n, cap);
        if (m != d)
            fail("mitm vs dp");
        if (trial < 4)
            printf("trial %d: n=%d cap=%ld best=%ld\n", trial, n, cap, m);
    }
    printf("25 instances match the weight DP\n");

    /* weights up to 1e12: DP over capacity is impossible, MITM is fine */
    int n = 30;
    long total = 0;
    for (int i = 0; i < n; i++) {
        it[i].w = 100000000000L + (long)(rnd() % 900000) * 1000003L;
        it[i].v = 10 + (long)(rnd() % 1000);
        total += it[i].w;
    }
    size_t sa, sb;
    long best = knapsack_mitm(it, n, total / 2, &sa, &sb);
    printf("huge weights: best value %ld, kept lists %zu and %zu of 32768\n", best, sa, sb);
    return 0;
}
