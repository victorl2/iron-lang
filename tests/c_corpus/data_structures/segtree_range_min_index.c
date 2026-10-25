/*
 * title: Segment tree returning range minimum with index and count
 * topic: data_structures
 * covers: argmin, leftmost tie-break, count of minima, struct monoid, point update, brute force
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
    int val;
    int idx; /* leftmost position holding val */
    int cnt; /* how many positions in the range hold val */
} Cell;

#define N 200
static Cell tree[2 * 256];
static int size_;

static Cell merge(Cell a, Cell b) {
    if (a.val < b.val)
        return a;
    if (b.val < a.val)
        return b;
    Cell c = {a.val, a.idx < b.idx ? a.idx : b.idx, a.cnt + b.cnt};
    return c;
}

static void build(const int *a, int n) {
    size_ = 1;
    while (size_ < n)
        size_ *= 2;
    for (int i = 0; i < size_; i++) {
        Cell c = {i < n ? a[i] : 2000000000, i, 1};
        tree[size_ + i] = c;
    }
    for (int i = size_ - 1; i >= 1; i--)
        tree[i] = merge(tree[2 * i], tree[2 * i + 1]);
}

static void set(int i, int v) {
    i += size_;
    tree[i].val = v;
    for (i >>= 1; i >= 1; i >>= 1)
        tree[i] = merge(tree[2 * i], tree[2 * i + 1]);
}

/* leftmost/rightmost order matters for tie-breaking only via idx compare, so merge is symmetric */
static Cell query(int l, int r) { /* inclusive */
    Cell res = {2000000000, N + 1, 0};
    for (l += size_, r += size_ + 1; l < r; l >>= 1, r >>= 1) {
        if (l & 1)
            res = merge(res, tree[l++]);
        if (r & 1)
            res = merge(res, tree[--r]);
    }
    return res;
}

int main(void) {
    int a[N];
    for (int i = 0; i < N; i++)
        a[i] = (int)rnd(30); /* small range forces many ties */
    build(a, N);
    long idx_sum = 0, cnt_sum = 0;
    int ties = 0;
    for (int step = 0; step < 8000; step++) {
        if (rnd(4) == 0) {
            int i = (int)rnd(N);
            a[i] = (int)rnd(30);
            set(i, a[i]);
            continue;
        }
        int l = (int)rnd(N), r = (int)rnd(N);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        int bv = a[l], bi = l, bc = 0;
        for (int i = l; i <= r; i++) {
            if (a[i] < bv) {
                bv = a[i];
                bi = i;
                bc = 0;
            }
            if (a[i] == bv)
                bc++;
        }
        Cell g = query(l, r);
        check(g.val == bv && g.idx == bi && g.cnt == bc, "argmin triple");
        idx_sum += g.idx;
        cnt_sum += g.cnt;
        ties += g.cnt > 1;
    }
    printf("index sum=%ld count sum=%ld ranges with ties=%d\n", idx_sum, cnt_sum, ties);
    Cell all = query(0, N - 1);
    printf("global min=%d at %d occurring %d times\n", all.val, all.idx, all.cnt);
    /* sorting by repeated extraction: selection sort via the tree */
    int order[N];
    long weighted = 0;
    for (int k = 0; k < N; k++) {
        Cell c = query(0, N - 1);
        order[k] = c.idx;
        weighted += (long)(k + 1) * c.val;
        set(c.idx, 1000000);
    }
    for (int k = 1; k < N; k++) {
        check(a[order[k - 1]] < a[order[k]] || (a[order[k - 1]] == a[order[k]] && order[k - 1] < order[k]),
              "stable extraction order");
    }
    printf("selection order head: %d %d %d %d %d weighted=%ld\n", order[0], order[1], order[2],
           order[3], order[4], weighted);
    return 0;
}
