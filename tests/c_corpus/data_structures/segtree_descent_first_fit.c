/*
 * title: Segment tree descent for first-at-least and first-fit packing
 * topic: data_structures
 * covers: max segment tree, tree descent, first index at least x from a start, last index, first-fit bin packing
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

#define MAXN 512
static int mx[4 * MAXN];
static int n_;

static void build(int o, int l, int r, const int *a) {
    if (l == r) {
        mx[o] = a[l];
        return;
    }
    int m = (l + r) / 2;
    build(2 * o, l, m, a);
    build(2 * o + 1, m + 1, r, a);
    mx[o] = mx[2 * o] > mx[2 * o + 1] ? mx[2 * o] : mx[2 * o + 1];
}

static void set(int o, int l, int r, int i, int v) {
    if (l == r) {
        mx[o] = v;
        return;
    }
    int m = (l + r) / 2;
    if (i <= m)
        set(2 * o, l, m, i, v);
    else
        set(2 * o + 1, m + 1, r, i, v);
    mx[o] = mx[2 * o] > mx[2 * o + 1] ? mx[2 * o] : mx[2 * o + 1];
}

static long visited;

/* first index >= from with value >= x, or -1 */
static int first_ge(int o, int l, int r, int from, int x) {
    visited++;
    if (r < from || mx[o] < x)
        return -1;
    if (l == r)
        return l;
    int m = (l + r) / 2;
    int res = first_ge(2 * o, l, m, from, x);
    if (res >= 0)
        return res;
    return first_ge(2 * o + 1, m + 1, r, from, x);
}

/* last index <= upto with value >= x, or -1 */
static int last_ge(int o, int l, int r, int upto, int x) {
    if (l > upto || mx[o] < x)
        return -1;
    if (l == r)
        return l;
    int m = (l + r) / 2;
    int res = last_ge(2 * o + 1, m + 1, r, upto, x);
    if (res >= 0)
        return res;
    return last_ge(2 * o, l, m, upto, x);
}

int main(void) {
    int n = 300;
    n_ = n;
    int a[MAXN];
    for (int i = 0; i < n; i++)
        a[i] = (int)rnd(1000);
    build(1, 0, n - 1, a);
    long fsum = 0, lsum = 0;
    int misses = 0;
    for (int step = 0; step < 6000; step++) {
        if (rnd(4) == 0) {
            int i = (int)rnd((unsigned)n);
            a[i] = (int)rnd(1000);
            set(1, 0, n - 1, i, a[i]);
            continue;
        }
        int p = (int)rnd((unsigned)n);
        int x = (int)rnd(1000);
        int want = -1;
        for (int i = p; i < n; i++)
            if (a[i] >= x) {
                want = i;
                break;
            }
        check(first_ge(1, 0, n - 1, p, x) == want, "first_ge");
        int want2 = -1;
        for (int i = p; i >= 0; i--)
            if (a[i] >= x) {
                want2 = i;
                break;
            }
        check(last_ge(1, 0, n - 1, p, x) == want2, "last_ge");
        fsum += want;
        lsum += want2;
        misses += want < 0;
    }
    printf("first sum=%ld last sum=%ld misses=%d nodes visited=%ld\n", fsum, lsum, misses, visited);

    /* first-fit packing of items into bins of capacity 100 */
    int cap[MAXN];
    int bins = 200;
    for (int i = 0; i < bins; i++)
        cap[i] = 100;
    int naive[MAXN];
    memcpy(naive, cap, sizeof cap);
    build(1, 0, bins - 1, cap);
    int placed = 0, rejected = 0;
    long where_sum = 0;
    for (int i = 0; i < 1500; i++) {
        int sz = 5 + (int)rnd(60);
        int pos = first_ge(1, 0, bins - 1, 0, sz);
        int want = -1;
        for (int j = 0; j < bins; j++)
            if (naive[j] >= sz) {
                want = j;
                break;
            }
        check(pos == want, "first fit position");
        if (pos < 0) {
            rejected++;
            continue;
        }
        naive[pos] -= sz;
        cap[pos] = naive[pos];
        set(1, 0, bins - 1, pos, cap[pos]);
        placed++;
        where_sum += pos;
    }
    int used = 0;
    for (int j = 0; j < bins; j++)
        used += naive[j] < 100;
    printf("placed=%d rejected=%d bins used=%d position sum=%ld\n", placed, rejected, used, where_sum);
    printf("largest remaining capacity=%d\n", mx[1]);
    return 0;
}
