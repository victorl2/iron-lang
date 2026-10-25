/*
 * title: McCreight priority search tree
 * topic: data_structures
 * covers: priority search tree, three-sided range queries, heap on y plus search tree on x, output-sensitive query cost
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NP 2000

typedef struct { int x, y, id; } Pt;
typedef struct PST {
    Pt p;              /* point with maximum y in this subtree */
    int split;         /* x boundary: left subtree has x <= split, right has x > split */
    struct PST *l, *r;
} PST;

static long visited;

static unsigned long long rs = 0x7E57ED7EULL * 1013;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int cmp_x(const void *a, const void *b) {
    const Pt *p = a, *q = b;
    if (p->x != q->x) return p->x < q->x ? -1 : 1;
    return p->id < q->id ? -1 : p->id > q->id;
}
/* build from points sorted by x: the max-y point is pulled to the node, the rest split at the median */
static PST *build(Pt *a, int n) {
    if (n == 0) return NULL;
    PST *t = calloc(1, sizeof *t);
    int best = 0;
    for (int i = 1; i < n; i++) if (a[i].y > a[best].y || (a[i].y == a[best].y && a[i].id < a[best].id)) best = i;
    t->p = a[best];
    memmove(a + best, a + best + 1, sizeof(Pt) * (size_t)(n - best - 1)); /* keeps x order */
    n--;
    if (n == 0) { t->split = t->p.x; return t; }
    int mid = n / 2; /* left gets mid+1 points */
    t->split = a[mid].x;
    /* equal x values may straddle the split; queries descend into both sides when x equals the split */
    t->l = build(a, mid + 1);
    t->r = build(a + mid + 1, n - mid - 1);
    return t;
}
static void destroy(PST *t) { if (t) { destroy(t->l); destroy(t->r); free(t); } }

/* report points with x1 <= x <= x2 and y >= y0 */
static void query(const PST *t, int x1, int x2, int y0, Pt *out, int *cnt) {
    if (!t) return;
    visited++;
    if (t->p.y < y0) return; /* heap property: nothing below can qualify */
    if (t->p.x >= x1 && t->p.x <= x2) out[(*cnt)++] = t->p;
    if (x1 <= t->split) query(t->l, x1, x2, y0, out, cnt);
    if (x2 >= t->split) query(t->r, x1, x2, y0, out, cnt);
}
static int cmp_id(const void *a, const void *b) { return ((const Pt *)a)->id - ((const Pt *)b)->id; }
static int height(const PST *t) { if (!t) return 0; int a = height(t->l), b = height(t->r); return 1 + (a > b ? a : b); }
static int size(const PST *t) { return t ? 1 + size(t->l) + size(t->r) : 0; }
/* heap order and search order invariants */
static void verify(const PST *t, int lo, int hi, int ymax) {
    if (!t) return;
    check(t->p.y <= ymax, "heap order on y");
    check(t->p.x >= lo && t->p.x <= hi, "x inside the node's range");
    verify(t->l, lo, t->split, t->p.y);
    verify(t->r, t->split, hi, t->p.y);
}
static int lg2(int n) { int l = 0; while ((1 << l) < n) l++; return l; }

int main(void) {
    static Pt pts[NP], work[NP];
    for (int i = 0; i < NP; i++) { pts[i].x = (int)(rnd() % 5000); pts[i].y = (int)(rnd() % 100000); pts[i].id = i; if (i % 10 == 0) pts[i].x = 2500 + (int)(rnd() % 3); }
    memcpy(work, pts, sizeof pts);
    qsort(work, NP, sizeof(Pt), cmp_x);
    PST *t = build(work, NP);
    check(size(t) == NP, "all points stored");
    verify(t, -1, 1 << 30, 1 << 30);
    printf("points %d, tree height %d (log2 n = %d)\n", NP, height(t), lg2(NP));
    long total_out = 0, total_vis = 0; int worst_ratio = 0;
    for (int q = 0; q < 400; q++) {
        int x1 = (int)(rnd() % 5000), w = (int)(rnd() % 2500);
        int x2 = x1 + w, y0 = (q % 5 == 0) ? 0 : (int)(rnd() % 100000);
        if (q % 7 == 0) y0 = 99000;
        static Pt out[NP]; int cnt = 0; visited = 0;
        query(t, x1, x2, y0, out, &cnt);
        static Pt want[NP]; int nw = 0;
        for (int i = 0; i < NP; i++) if (pts[i].x >= x1 && pts[i].x <= x2 && pts[i].y >= y0) want[nw++] = pts[i];
        check(cnt == nw, "three-sided count");
        qsort(out, (size_t)cnt, sizeof(Pt), cmp_id);
        qsort(want, (size_t)nw, sizeof(Pt), cmp_id);
        for (int i = 0; i < cnt; i++) check(out[i].id == want[i].id, "three-sided members");
        /* output sensitive: visited nodes bounded by k + O(height) paths */
        check(visited <= 4L * cnt + 4L * height(t) + 8, "visited nodes are O(k + log n)");
        int ratio = (int)(visited * 10 / (cnt + 1));
        if (ratio > worst_ratio) worst_ratio = ratio;
        total_out += cnt; total_vis += visited;
    }
    printf("400 queries: %ld reported, %ld nodes visited\n", total_out, total_vis);
    /* highest-y point in an x-range: query with threshold walking down */
    long sum = 0;
    for (int q = 0; q < 100; q++) {
        int x1 = (int)(rnd() % 4000), x2 = x1 + 200 + (int)(rnd() % 800);
        int lo = 0, hi = 100000, ans = -1;
        while (lo <= hi) { /* binary search on threshold: largest y0 with a hit */
            int mid = (lo + hi) / 2; Pt out[NP]; int cnt = 0;
            query(t, x1, x2, mid, out, &cnt);
            if (cnt) { ans = mid; lo = mid + 1; } else hi = mid - 1;
        }
        int want = -1;
        for (int i = 0; i < NP; i++) if (pts[i].x >= x1 && pts[i].x <= x2 && pts[i].y > want) want = pts[i].y;
        check(ans == want, "max y in x-range via threshold search");
        sum += ans;
    }
    printf("max-y-in-range checksum %ld\n", sum);
    destroy(t);
    return 0;
}
