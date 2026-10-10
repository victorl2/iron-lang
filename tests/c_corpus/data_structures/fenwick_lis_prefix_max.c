/*
 * title: Fenwick prefix-max tree for LIS length, count and weight
 * topic: data_structures
 * covers: prefix-max Fenwick, pair values (length,count), longest increasing subsequence counting, max-weight increasing subsequence, O(n^2) DP cross-check
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
    int len;
    long cnt;
} Pair;

/* prefix combine: longer wins, equal lengths add counts */
static Pair combine(Pair a, Pair b) {
    if (a.len != b.len)
        return a.len > b.len ? a : b;
    Pair r = {a.len, a.cnt + b.cnt};
    return r;
}

typedef struct {
    int n;
    Pair *t;
} PairTree;

static void pt_init(PairTree *f, int n) {
    f->n = n;
    f->t = calloc((size_t)n + 1, sizeof(Pair));
    check(f->t != NULL, "alloc");
}

static void pt_update(PairTree *f, int i, Pair v) {
    for (; i <= f->n; i += i & -i)
        f->t[i] = combine(f->t[i], v);
}

static Pair pt_query(const PairTree *f, int i) {
    Pair r = {0, 1};
    for (; i > 0; i -= i & -i)
        r = combine(r, f->t[i]);
    return r;
}

/* max-weight variant */
static long wt[2048];

static void w_update(int n, int i, long v) {
    for (; i <= n; i += i & -i)
        if (wt[i] < v)
            wt[i] = v;
}

static long w_query(int i) {
    long r = 0;
    for (; i > 0; i -= i & -i)
        if (wt[i] > r)
            r = wt[i];
    return r;
}

int main(void) {
    int trials[][2] = {{1, 5}, {10, 4}, {60, 8}, {300, 20}, {600, 50}, {800, 800}};
    for (int ti = 0; ti < 6; ti++) {
        int n = trials[ti][0], range = trials[ti][1];
        int *a = malloc(sizeof(int) * (size_t)n);
        long *w = malloc(sizeof(long) * (size_t)n);
        check(a && w, "alloc");
        for (int i = 0; i < n; i++) {
            a[i] = 1 + (int)rnd((unsigned)range);
            w[i] = 1 + (long)rnd(100);
        }
        PairTree pt;
        pt_init(&pt, range);
        memset(wt, 0, sizeof wt);
        long best_w = 0;
        for (int i = 0; i < n; i++) {
            Pair p = pt_query(&pt, a[i] - 1); /* strictly smaller values */
            Pair here = {p.len + 1, p.cnt};
            pt_update(&pt, a[i], here);
            long cw = w_query(a[i] - 1) + w[i];
            w_update(range, a[i], cw);
            if (cw > best_w)
                best_w = cw;
        }
        Pair all = pt_query(&pt, range);
        /* brute force DP */
        int *len = malloc(sizeof(int) * (size_t)n);
        long *ways = malloc(sizeof(long) * (size_t)n);
        long *bw = malloc(sizeof(long) * (size_t)n);
        check(len && ways && bw, "alloc");
        int bl = 0;
        long bc = 0, bbw = 0;
        for (int i = 0; i < n; i++) {
            len[i] = 1;
            ways[i] = 1;
            bw[i] = w[i];
            for (int j = 0; j < i; j++)
                if (a[j] < a[i]) {
                    if (len[j] + 1 > len[i]) {
                        len[i] = len[j] + 1;
                        ways[i] = ways[j];
                    } else if (len[j] + 1 == len[i])
                        ways[i] += ways[j];
                    if (bw[j] + w[i] > bw[i])
                        bw[i] = bw[j] + w[i];
                }
            if (len[i] > bl) {
                bl = len[i];
                bc = ways[i];
            } else if (len[i] == bl)
                bc += ways[i];
            if (bw[i] > bbw)
                bbw = bw[i];
        }
        check(all.len == bl && all.cnt == bc, "LIS length and count");
        check(best_w == bbw, "max weight increasing subsequence");
        printf("n=%-3d range=%-3d LIS=%d count=%ld best weight=%ld\n", n, range, all.len, all.cnt, best_w);
        free(a);
        free(w);
        free(len);
        free(ways);
        free(bw);
        free(pt.t);
    }
    return 0;
}
