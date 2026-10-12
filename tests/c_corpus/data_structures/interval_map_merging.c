/*
 * title: Interval map with range assign and automatic coalescing
 * topic: data_structures
 * covers: interval map, sorted boundary array, binary search, range assign with split and merge, canonical form invariant, weighted range sum, brute force array
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

#define U 3000 /* positions [0, U) */

typedef struct {
    int start;
    int val;
} Seg; /* covers [start, next start) */

static Seg *segs;
static int nseg, cap;
static long shifts;

static void ensure(int need) {
    if (need > cap) {
        cap = cap ? cap * 2 : 16;
        while (cap < need)
            cap *= 2;
        segs = realloc(segs, sizeof(Seg) * (size_t)cap);
        check(segs != NULL, "alloc");
    }
}

/* index of the segment containing x */
static int find(int x) {
    int lo = 0, hi = nseg - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (segs[mid].start <= x)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static void insert_at(int idx, Seg s) {
    ensure(nseg + 1);
    memmove(segs + idx + 1, segs + idx, sizeof(Seg) * (size_t)(nseg - idx));
    shifts += nseg - idx;
    segs[idx] = s;
    nseg++;
}

static void erase_range(int from, int to) { /* erase [from, to) */
    if (from >= to)
        return;
    memmove(segs + from, segs + to, sizeof(Seg) * (size_t)(nseg - to));
    shifts += nseg - to;
    nseg -= to - from;
}

/* make sure a boundary exists at x (0 < x < U); returns its index */
static int split(int x) {
    int i = find(x);
    if (segs[i].start == x)
        return i;
    Seg s = {x, segs[i].val};
    insert_at(i + 1, s);
    return i + 1;
}

static void assign(int l, int r, int v) { /* [l, r) */
    int a = l == 0 ? 0 : split(l);
    int b = r == U ? nseg : split(r);
    /* after split(l), index b may have shifted only if l's split came first; recompute safely */
    if (r != U)
        b = find(r);
    erase_range(a + 1, b);
    segs[a].val = v;
    segs[a].start = l;
    /* coalesce with neighbours holding the same value */
    if (a + 1 < nseg && segs[a + 1].val == v)
        erase_range(a + 1, a + 2);
    if (a > 0 && segs[a - 1].val == v)
        erase_range(a, a + 1);
}

static int get(int x) { return segs[find(x)].val; }

static long weighted(int l, int r) { /* sum of value over [l, r) */
    long s = 0;
    int i = find(l);
    for (; i < nseg && segs[i].start < r; i++) {
        int lo = segs[i].start > l ? segs[i].start : l;
        int end = i + 1 < nseg ? segs[i + 1].start : U;
        int hi = end < r ? end : r;
        s += (long)segs[i].val * (hi - lo);
    }
    return s;
}

static void check_canonical(void) {
    check(nseg >= 1 && segs[0].start == 0, "starts at zero");
    for (int i = 1; i < nseg; i++) {
        check(segs[i].start > segs[i - 1].start, "boundaries increase");
        check(segs[i].val != segs[i - 1].val, "neighbours differ (coalesced)");
    }
    check(segs[nseg - 1].start < U, "last boundary inside universe");
}

int main(void) {
    static int ref[U];
    Seg init = {0, 0};
    ensure(1);
    segs[0] = init;
    nseg = 1;
    long wsum = 0, gsum = 0;
    int maxseg = 1, nassign = 0;
    for (int step = 0; step < 6000; step++) {
        int l = (int)rnd(U), len = 1 + (int)rnd(rnd(4) == 0 ? 600 : 40);
        int r = l + len > U ? U : l + len;
        unsigned op = rnd(5);
        if (op < 3) {
            int v = (int)rnd(6);
            assign(l, r, v);
            for (int i = l; i < r; i++)
                ref[i] = v;
            nassign++;
            check_canonical();
            if (nseg > maxseg)
                maxseg = nseg;
        } else if (op == 3) {
            int x = (int)rnd(U);
            check(get(x) == ref[x], "point get");
            gsum += get(x);
        } else {
            long want = 0;
            for (int i = l; i < r; i++)
                want += ref[i];
            check(weighted(l, r) == want, "weighted sum");
            wsum += want;
        }
    }
    /* the segment list must describe exactly the reference array */
    int pos = 0;
    for (int i = 0; i < nseg; i++) {
        int end = i + 1 < nseg ? segs[i + 1].start : U;
        for (; pos < end; pos++)
            check(ref[pos] == segs[i].val, "segment covers reference");
    }
    printf("assigns=%d max segments=%d final segments=%d\n", nassign, maxseg, nseg);
    printf("point digest=%ld weighted digest=%ld shifts=%ld\n", gsum, wsum, shifts);
    printf("first segments:");
    for (int i = 0; i < 6 && i < nseg; i++)
        printf(" [%d]=%d", segs[i].start, segs[i].val);
    printf("\n");
    assign(0, U, 3);
    check(nseg == 1 && segs[0].val == 3, "full assign collapses to one segment");
    printf("after full assign: segments=%d value=%d total=%ld\n", nseg, segs[0].val, weighted(0, U));
    free(segs);
    return 0;
}
