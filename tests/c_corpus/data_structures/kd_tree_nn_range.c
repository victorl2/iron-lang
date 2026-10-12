/*
 * title: Static 2D k-d tree with nearest neighbour and box search
 * topic: data_structures
 * covers: k-d tree, median split by sorting, nearest neighbour with plane pruning, orthogonal range search, deterministic tie-break, visit counts
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
    int x, y, id;
} Pt;

typedef struct {
    Pt p;
    int left, right; /* indices into nodes, -1 if none */
} KNode;

#define NP 2000
static KNode nodes[NP];
static int nn_;
static Pt work[NP];
static int sort_axis;

static int cmp_axis(const void *a, const void *b) {
    const Pt *p = a, *q = b;
    int u = sort_axis == 0 ? p->x : p->y, v = sort_axis == 0 ? q->x : q->y;
    if (u != v)
        return u < v ? -1 : 1;
    return p->id - q->id;
}

static int build(int lo, int hi, int axis) { /* [lo, hi) of work[] */
    if (lo >= hi)
        return -1;
    sort_axis = axis;
    qsort(work + lo, (size_t)(hi - lo), sizeof(Pt), cmp_axis);
    int mid = (lo + hi) / 2;
    int me = nn_++;
    nodes[me].p = work[mid];
    nodes[me].left = build(lo, mid, 1 - axis);
    nodes[me].right = build(mid + 1, hi, 1 - axis);
    return me;
}

static long d2(Pt a, int x, int y) {
    long dx = a.x - x, dy = a.y - y;
    return dx * dx + dy * dy;
}

static long visited;

typedef struct {
    Pt best;
    long bd;
} Best;

static void nearest(int t, int axis, int qx, int qy, Best *b) {
    if (t < 0)
        return;
    visited++;
    long d = d2(nodes[t].p, qx, qy);
    if (d < b->bd || (d == b->bd && nodes[t].p.id < b->best.id)) {
        b->bd = d;
        b->best = nodes[t].p;
    }
    long diff = axis == 0 ? qx - nodes[t].p.x : qy - nodes[t].p.y;
    int near = diff <= 0 ? nodes[t].left : nodes[t].right;
    int far = diff <= 0 ? nodes[t].right : nodes[t].left;
    nearest(near, 1 - axis, qx, qy, b);
    /* ties can hide on the far side, so use <= */
    if (diff * diff <= b->bd)
        nearest(far, 1 - axis, qx, qy, b);
}

static void box(int t, int axis, int x1, int y1, int x2, int y2, int *out, int *cnt) {
    if (t < 0)
        return;
    visited++;
    Pt p = nodes[t].p;
    if (p.x >= x1 && p.x <= x2 && p.y >= y1 && p.y <= y2)
        out[(*cnt)++] = p.id;
    int c = axis == 0 ? p.x : p.y;
    int lo = axis == 0 ? x1 : y1, hi = axis == 0 ? x2 : y2;
    if (lo <= c)
        box(nodes[t].left, 1 - axis, x1, y1, x2, y2, out, cnt);
    if (hi >= c)
        box(nodes[t].right, 1 - axis, x1, y1, x2, y2, out, cnt);
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void) {
    static Pt pts[NP];
    for (int i = 0; i < NP; i++) {
        pts[i].x = (int)rnd(1000);
        pts[i].y = (int)rnd(1000);
        pts[i].id = i;
        if (i % 5 == 0) { /* duplicate coordinates to exercise ties */
            pts[i].x = pts[i / 2].x;
            pts[i].y = pts[i / 2].y;
        }
    }
    memcpy(work, pts, sizeof pts);
    int root = build(0, NP, 0);
    check(root == 0 && nn_ == NP, "all points stored");
    long dsum = 0, idsum = 0;
    for (int q = 0; q < 1500; q++) {
        int qx = (int)rnd(1100) - 50, qy = (int)rnd(1100) - 50;
        Best b = {pts[0], -1};
        b.bd = 1L << 60;
        b.best.id = NP + 1;
        nearest(root, 0, qx, qy, &b);
        long wd = 1L << 60;
        int wid = -1;
        for (int i = 0; i < NP; i++) {
            long d = d2(pts[i], qx, qy);
            if (d < wd) {
                wd = d;
                wid = i;
            }
        }
        check(b.bd == wd && b.best.id == wid, "nearest neighbour");
        dsum += b.bd;
        idsum += b.best.id;
    }
    printf("nearest: distance^2 digest=%ld id digest=%ld\n", dsum, idsum);
    long nn_visits = visited;
    visited = 0;
    long rsum = 0;
    for (int q = 0; q < 800; q++) {
        int x1 = (int)rnd(1000), y1 = (int)rnd(1000);
        int x2 = x1 + (int)rnd(200), y2 = y1 + (int)rnd(200);
        static int got[NP];
        int cnt = 0;
        box(root, 0, x1, y1, x2, y2, got, &cnt);
        int want[NP], nw = 0;
        for (int i = 0; i < NP; i++)
            if (pts[i].x >= x1 && pts[i].x <= x2 && pts[i].y >= y1 && pts[i].y <= y2)
                want[nw++] = i;
        qsort(got, (size_t)cnt, sizeof(int), cmp_int);
        check(cnt == nw, "box count");
        for (int i = 0; i < nw; i++)
            check(got[i] == want[i], "box ids");
        rsum += cnt;
    }
    printf("box results=%ld\n", rsum);
    printf("avg nodes visited: nearest %ld/query, box %ld/query (of %d points)\n", nn_visits / 1500,
           visited / 800, NP);
    return 0;
}
