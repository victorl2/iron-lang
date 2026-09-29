/*
 * title: Point quadtree with range and nearest queries
 * topic: data_structures
 * covers: point quadtree, quadrant children per point, rectangle range search with pruning, nearest neighbour, depth statistics, brute force
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

typedef struct QNode {
    int x, y, id;
    struct QNode *kid[4]; /* 0: x<px,y<py  1: x>=px,y<py  2: x<px,y>=py  3: x>=px,y>=py */
} QNode;

static int quadrant(const QNode *n, int x, int y) { return (x >= n->x ? 1 : 0) + (y >= n->y ? 2 : 0); }

static int nodes_;

static QNode *insert(QNode *root, int x, int y, int id) {
    QNode *fresh = calloc(1, sizeof *fresh);
    check(fresh != NULL, "alloc");
    fresh->x = x;
    fresh->y = y;
    fresh->id = id;
    nodes_++;
    if (!root)
        return fresh;
    QNode *cur = root;
    for (;;) {
        int q = quadrant(cur, x, y);
        if (!cur->kid[q]) {
            cur->kid[q] = fresh;
            return root;
        }
        cur = cur->kid[q];
    }
}

static long visited;

static void range(const QNode *n, int x1, int y1, int x2, int y2, int *out, int *cnt) {
    if (!n)
        return;
    visited++;
    if (n->x >= x1 && n->x <= x2 && n->y >= y1 && n->y <= y2)
        out[(*cnt)++] = n->id;
    /* quadrant q can hold hits only if the query rectangle reaches into it */
    int west = x1 < n->x, east = x2 >= n->x, south = y1 < n->y, north = y2 >= n->y;
    if (west && south)
        range(n->kid[0], x1, y1, x2, y2, out, cnt);
    if (east && south)
        range(n->kid[1], x1, y1, x2, y2, out, cnt);
    if (west && north)
        range(n->kid[2], x1, y1, x2, y2, out, cnt);
    if (east && north)
        range(n->kid[3], x1, y1, x2, y2, out, cnt);
}

typedef struct {
    long d;
    int id;
} Best;

static void nearest(const QNode *n, int qx, int qy, Best *b) {
    if (!n)
        return;
    long dx = n->x - qx, dy = n->y - qy;
    long d = dx * dx + dy * dy;
    if (d < b->d || (d == b->d && n->id < b->id)) {
        b->d = d;
        b->id = n->id;
    }
    int first = quadrant(n, qx, qy);
    nearest(n->kid[first], qx, qy, b);
    for (int q = 0; q < 4; q++) {
        if (q == first || !n->kid[q])
            continue;
        /* distance from the query to the (unbounded) quadrant q */
        long gx = 0, gy = 0;
        int qe = q & 1, qn = q >> 1;
        if (qe && qx < n->x)
            gx = n->x - qx;
        if (!qe && qx >= n->x)
            gx = qx - n->x + 1;
        if (qn && qy < n->y)
            gy = n->y - qy;
        if (!qn && qy >= n->y)
            gy = qy - n->y + 1;
        if (gx * gx + gy * gy <= b->d)
            nearest(n->kid[q], qx, qy, b);
    }
}

static int depth(const QNode *n) {
    if (!n)
        return 0;
    int m = 0;
    for (int q = 0; q < 4; q++) {
        int d = depth(n->kid[q]);
        if (d > m)
            m = d;
    }
    return m + 1;
}

static void destroy(QNode *n) {
    if (!n)
        return;
    for (int q = 0; q < 4; q++)
        destroy(n->kid[q]);
    free(n);
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void) {
    enum { NP = 1500 };
    static int px[NP], py[NP];
    QNode *root = NULL;
    for (int i = 0; i < NP; i++) {
        px[i] = (int)rnd(1000);
        py[i] = (int)rnd(1000);
        root = insert(root, px[i], py[i], i);
    }
    printf("points=%d depth=%d\n", nodes_, depth(root));
    long rsum = 0;
    for (int q = 0; q < 700; q++) {
        int x1 = (int)rnd(1000), y1 = (int)rnd(1000);
        int x2 = x1 + (int)rnd(250), y2 = y1 + (int)rnd(250);
        int got[NP], cnt = 0;
        range(root, x1, y1, x2, y2, got, &cnt);
        qsort(got, (size_t)cnt, sizeof(int), cmp_int);
        int want[NP], nw = 0;
        for (int i = 0; i < NP; i++)
            if (px[i] >= x1 && px[i] <= x2 && py[i] >= y1 && py[i] <= y2)
                want[nw++] = i;
        check(cnt == nw, "range count");
        for (int i = 0; i < nw; i++)
            check(got[i] == want[i], "range ids");
        rsum += cnt;
    }
    printf("range results=%ld visited=%ld\n", rsum, visited);
    long dsum = 0, isum = 0;
    for (int q = 0; q < 1000; q++) {
        int qx = (int)rnd(1200) - 100, qy = (int)rnd(1200) - 100;
        Best b = {1L << 60, NP + 5};
        nearest(root, qx, qy, &b);
        long wd = 1L << 60;
        int wid = -1;
        for (int i = 0; i < NP; i++) {
            long dx = px[i] - qx, dy = py[i] - qy;
            long d = dx * dx + dy * dy;
            if (d < wd) {
                wd = d;
                wid = i;
            }
        }
        check(b.d == wd && b.id == wid, "nearest");
        dsum += b.d;
        isum += b.id;
    }
    printf("nearest digests: %ld %ld\n", dsum, isum);
    destroy(root);
    return 0;
}
