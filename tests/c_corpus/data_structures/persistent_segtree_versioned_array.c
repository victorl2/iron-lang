/*
 * title: Persistent array with versions, branching and structural diff
 * topic: data_structures
 * covers: path-copying segment tree, version branching, subtree sharing, structural diff by node identity, per-version sums
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

#define N 64
#define MAXV 400
#define POOL 40000

typedef struct {
    int l, r;
    long sum;
} PNode;

static PNode pool[POOL];
static int used = 0;

static int mk_leaf(long v) {
    check(used < POOL, "pool");
    pool[used].l = pool[used].r = -1;
    pool[used].sum = v;
    return used++;
}

static int mk_inner(int l, int r) {
    check(used < POOL, "pool");
    pool[used].l = l;
    pool[used].r = r;
    pool[used].sum = pool[l].sum + pool[r].sum;
    return used++;
}

static int build(int lo, int hi, const long *a) {
    if (lo == hi)
        return mk_leaf(a[lo]);
    int mid = (lo + hi) / 2;
    int l = build(lo, mid, a);
    int r = build(mid + 1, hi, a);
    return mk_inner(l, r);
}

static int update(int node, int lo, int hi, int i, long v) {
    if (lo == hi)
        return mk_leaf(v);
    int mid = (lo + hi) / 2;
    if (i <= mid) {
        int l = update(pool[node].l, lo, mid, i, v);
        return mk_inner(l, pool[node].r);
    }
    int r = update(pool[node].r, mid + 1, hi, i, v);
    return mk_inner(pool[node].l, r);
}

static long get(int node, int lo, int hi, int i) {
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (i <= mid) {
            node = pool[node].l;
            hi = mid;
        } else {
            node = pool[node].r;
            lo = mid + 1;
        }
    }
    return pool[node].sum;
}

static long range_sum(int node, int lo, int hi, int ql, int qr) {
    if (qr < lo || hi < ql)
        return 0;
    if (ql <= lo && hi <= qr)
        return pool[node].sum;
    int mid = (lo + hi) / 2;
    return range_sum(pool[node].l, lo, mid, ql, qr) + range_sum(pool[node].r, mid + 1, hi, ql, qr);
}

static long diff_visits;

/* number of positions where two versions differ; shared nodes are skipped */
static int diff(int x, int y, int lo, int hi) {
    diff_visits++;
    if (x == y)
        return 0;
    if (lo == hi)
        return pool[x].sum != pool[y].sum;
    int mid = (lo + hi) / 2;
    return diff(pool[x].l, pool[y].l, lo, mid) + diff(pool[x].r, pool[y].r, mid + 1, hi);
}

int main(void) {
    static long full[MAXV][N]; /* brute force copy of every version */
    static int root[MAXV];
    int nv = 0;
    long init[N];
    for (int i = 0; i < N; i++)
        init[i] = (long)rnd(100);
    memcpy(full[0], init, sizeof init);
    root[0] = build(0, N - 1, init);
    nv = 1;
    int base_nodes = used;
    int forks = 0;
    for (int step = 0; step < 350; step++) {
        int parent = (int)rnd((unsigned)nv); /* any old version can be branched */
        int i = (int)rnd(N);
        long v = (long)rnd(1000);
        memcpy(full[nv], full[parent], sizeof init);
        full[nv][i] = v;
        root[nv] = update(root[parent], 0, N - 1, i, v);
        forks += parent != nv - 1;
        nv++;
    }
    printf("versions=%d forks from non-latest=%d nodes=%d (initial %d)\n", nv, forks, used, base_nodes);
    long ssum = 0;
    for (int q = 0; q < 2000; q++) {
        int v = (int)rnd((unsigned)nv);
        int l = (int)rnd(N), r = (int)rnd(N);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        long want = 0;
        for (int i = l; i <= r; i++)
            want += full[v][i];
        check(range_sum(root[v], 0, N - 1, l, r) == want, "version range sum");
        check(get(root[v], 0, N - 1, l) == full[v][l], "version point read");
        ssum += want;
    }
    printf("range sum digest=%ld\n", ssum);
    long dsum = 0;
    for (int q = 0; q < 300; q++) {
        int x = (int)rnd((unsigned)nv), y = (int)rnd((unsigned)nv);
        int want = 0;
        for (int i = 0; i < N; i++)
            want += full[x][i] != full[y][i];
        check(diff(root[x], root[y], 0, N - 1) == want, "structural diff");
        dsum += want;
    }
    printf("diff total=%ld nodes visited=%ld (full scans would be %d)\n", dsum, diff_visits, 300 * (2 * N - 1));
    check(diff(root[5], root[5], 0, N - 1) == 0, "self diff");
    printf("version 0 vs last differ at %d positions\n", diff(root[0], root[nv - 1], 0, N - 1));
    return 0;
}
