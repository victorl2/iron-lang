/*
 * title: Offline rectangle counting by sweep and Fenwick tree
 * topic: data_structures
 * covers: offline queries, sweep line, event sorting with total order, dominance counting, inclusion-exclusion, Fenwick tree
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

#define GRID 200

typedef struct {
    int x, y;
    int kind; /* 0 = point, 1 = query corner */
    int qid;  /* query corner: which dominance query */
} Event;

static int cmp_event(const void *a, const void *b) {
    const Event *p = a, *q = b;
    if (p->x != q->x)
        return p->x < q->x ? -1 : 1;
    if (p->kind != q->kind)
        return p->kind - q->kind; /* points first so x <= X is inclusive */
    if (p->y != q->y)
        return p->y < q->y ? -1 : 1;
    return p->qid - q->qid;
}

static int bit[GRID + 2];

static void add(int y) {
    for (int i = y + 1; i <= GRID + 1; i += i & -i)
        bit[i]++;
}

static int upto(int y) { /* points with y' <= y; y may be -1 */
    int s = 0;
    for (int i = y + 1; i > 0; i -= i & -i)
        s += bit[i];
    return s;
}

int main(void) {
    enum { NP = 1500, NQ = 800 };
    static int px[NP], py[NP];
    for (int i = 0; i < NP; i++) {
        px[i] = (int)rnd(GRID);
        py[i] = (int)rnd(GRID);
        if (i % 7 == 0) { /* clusters and duplicates */
            px[i] = 50 + (int)rnd(5);
            py[i] = 120 + (int)rnd(5);
        }
    }
    static int qx1[NQ], qx2[NQ], qy1[NQ], qy2[NQ];
    for (int i = 0; i < NQ; i++) {
        int a = (int)rnd(GRID), b = (int)rnd(GRID), c = (int)rnd(GRID), d = (int)rnd(GRID);
        qx1[i] = a < b ? a : b;
        qx2[i] = a < b ? b : a;
        qy1[i] = c < d ? c : d;
        qy2[i] = c < d ? d : c;
    }
    /* four dominance corners per query */
    static Event ev[NP + 4 * NQ];
    int ne = 0;
    for (int i = 0; i < NP; i++) {
        Event e = {px[i], py[i], 0, 0};
        ev[ne++] = e;
    }
    for (int i = 0; i < NQ; i++) {
        int xs[4] = {qx2[i], qx1[i] - 1, qx2[i], qx1[i] - 1};
        int ys[4] = {qy2[i], qy2[i], qy1[i] - 1, qy1[i] - 1};
        for (int k = 0; k < 4; k++) {
            Event e = {xs[k], ys[k], 1, i * 4 + k};
            ev[ne++] = e;
        }
    }
    qsort(ev, (size_t)ne, sizeof(Event), cmp_event);
    static int dom[4 * NQ];
    for (int i = 0; i < ne; i++) {
        if (ev[i].kind == 0)
            add(ev[i].y);
        else
            dom[ev[i].qid] = upto(ev[i].y);
    }
    long total = 0;
    int empty = 0, maxc = 0, maxq = 0;
    for (int i = 0; i < NQ; i++) {
        int got = dom[4 * i] - dom[4 * i + 1] - dom[4 * i + 2] + dom[4 * i + 3];
        int want = 0;
        for (int j = 0; j < NP; j++)
            want += px[j] >= qx1[i] && px[j] <= qx2[i] && py[j] >= qy1[i] && py[j] <= qy2[i];
        check(got == want, "rectangle count");
        total += got;
        empty += got == 0;
        if (got > maxc) {
            maxc = got;
            maxq = i;
        }
    }
    printf("points=%d queries=%d events=%d\n", NP, NQ, ne);
    printf("total counted=%ld empty rectangles=%d\n", total, empty);
    printf("busiest rectangle #%d [%d..%d]x[%d..%d] holds %d\n", maxq, qx1[maxq], qx2[maxq], qy1[maxq],
           qy2[maxq], maxc);
    return 0;
}
