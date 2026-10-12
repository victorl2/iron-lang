/*
 * title: 2D range tree with fractional cascading
 * topic: data_structures
 * covers: range tree, merge-sort tree, fractional cascading bridges, orthogonal range counting, comparison savings
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NP 1024

typedef struct { int x, y, id; } Pt;
typedef struct RT {
    int lo, hi;         /* range in x-sorted array [lo,hi) */
    int n;
    Pt *ys;             /* points sorted by (y,id) */
    int *lb, *rb;       /* bridges: number of entries among ys[0..i) that go to the left / right child */
    struct RT *l, *r;
} RT;

static Pt byx[NP];
static long bin_steps; /* comparisons spent in binary searches */

static unsigned long long rs = 0x2D2D2DULL * 104729;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int lessy(const Pt *a, const Pt *b) { return a->y != b->y ? a->y < b->y : a->id < b->id; }
static int cmp_x(const void *a, const void *b) {
    const Pt *p = a, *q = b;
    if (p->x != q->x) return p->x < q->x ? -1 : 1;
    return p->id - q->id;
}
static RT *build(int lo, int hi) {
    RT *t = calloc(1, sizeof *t);
    t->lo = lo; t->hi = hi; t->n = hi - lo;
    t->ys = malloc(sizeof(Pt) * (size_t)t->n);
    if (t->n == 1) { t->ys[0] = byx[lo]; return t; }
    int mid = (lo + hi) / 2;
    t->l = build(lo, mid); t->r = build(mid, hi);
    t->lb = malloc(sizeof(int) * (size_t)(t->n + 1)); t->rb = malloc(sizeof(int) * (size_t)(t->n + 1));
    /* merge children by y; record how many came from each side so far */
    int i = 0, j = 0, k = 0;
    t->lb[0] = t->rb[0] = 0;
    while (i < t->l->n || j < t->r->n) {
        if (j >= t->r->n || (i < t->l->n && lessy(&t->l->ys[i], &t->r->ys[j]))) t->ys[k++] = t->l->ys[i++];
        else t->ys[k++] = t->r->ys[j++];
        t->lb[k] = i; t->rb[k] = j;
    }
    return t;
}
static void destroy(RT *t) { if (!t) return; destroy(t->l); destroy(t->r); free(t->ys); free(t->lb); free(t->rb); free(t); }

/* first index in ys with y >= v */
static int lower_y(const RT *t, int v) {
    int lo = 0, hi = t->n;
    while (lo < hi) { int m = (lo + hi) / 2; bin_steps++; if (t->ys[m].y < v) lo = m + 1; else hi = m; }
    return lo;
}
/* count points with x in [x1,x2] and y in [y1,y2]; a = #entries with y < y1, b = #entries with y <= y2 (positions in this node's array) */
static long count_cascade(const RT *t, int x1, int x2, int a, int b) {
    if (a >= b) return 0;
    int xlo = byx[t->lo].x, xhi = byx[t->hi - 1].x;
    if (xhi < x1 || xlo > x2) return 0;
    if (x1 <= xlo && xhi <= x2) return b - a;
    if (t->n == 1) return 0;
    return count_cascade(t->l, x1, x2, t->lb[a], t->lb[b]) + count_cascade(t->r, x1, x2, t->rb[a], t->rb[b]);
}
static long count_plain(const RT *t, int x1, int x2, int y1, int y2) {
    int xlo = byx[t->lo].x, xhi = byx[t->hi - 1].x;
    if (xhi < x1 || xlo > x2) return 0;
    if (x1 <= xlo && xhi <= x2) return lower_y(t, y2 + 1) - lower_y(t, y1);
    if (t->n == 1) return 0;
    return count_plain(t->l, x1, x2, y1, y2) + count_plain(t->r, x1, x2, y1, y2);
}
static int report(const RT *t, int x1, int x2, int y1, int y2, int a, int b, int *ids, int cnt) {
    if (a >= b) return cnt;
    int xlo = byx[t->lo].x, xhi = byx[t->hi - 1].x;
    if (xhi < x1 || xlo > x2) return cnt;
    if (x1 <= xlo && xhi <= x2) { for (int i = a; i < b; i++) ids[cnt++] = t->ys[i].id; return cnt; }
    if (t->n == 1) return cnt;
    cnt = report(t->l, x1, x2, y1, y2, t->lb[a], t->lb[b], ids, cnt);
    return report(t->r, x1, x2, y1, y2, t->rb[a], t->rb[b], ids, cnt);
}
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int height(const RT *t) { if (!t->l) return 1; int a = height(t->l), b = height(t->r); return 1 + (a > b ? a : b); }
static long total_storage(const RT *t) { return t->n + (t->l ? total_storage(t->l) + total_storage(t->r) : 0); }

int main(void) {
    static Pt orig[NP];
    for (int i = 0; i < NP; i++) { orig[i].x = (int)(rnd() % 400); orig[i].y = (int)(rnd() % 400); orig[i].id = i; }
    memcpy(byx, orig, sizeof orig);
    qsort(byx, NP, sizeof(Pt), cmp_x);
    RT *root = build(0, NP);
    printf("points %d, height %d, stored entries %ld (n log n = %d)\n", NP, height(root), total_storage(root), NP * 10);
    long sum = 0, plain_steps = 0, casc_steps = 0, rep_total = 0;
    for (int q = 0; q < 500; q++) {
        int x1 = (int)(rnd() % 400), x2 = x1 + (int)(rnd() % 200), y1 = (int)(rnd() % 400), y2 = y1 + (int)(rnd() % 200);
        if (q == 0) { x1 = 0; x2 = 399; y1 = 0; y2 = 399; }
        long brute = 0;
        for (int i = 0; i < NP; i++) if (orig[i].x >= x1 && orig[i].x <= x2 && orig[i].y >= y1 && orig[i].y <= y2) brute++;
        bin_steps = 0;
        long cp = count_plain(root, x1, x2, y1, y2);
        plain_steps += bin_steps;
        bin_steps = 0;
        int a = lower_y(root, y1), b = lower_y(root, y2 + 1);
        long cc = count_cascade(root, x1, x2, a, b);
        casc_steps += bin_steps;
        check(cp == brute && cc == brute, "range count vs brute force");
        if (q % 5 == 0) {
            int ids[NP]; int n = report(root, x1, x2, y1, y2, a, b, ids, 0);
            check(n == brute, "report count");
            qsort(ids, (size_t)n, sizeof(int), cmp_int);
            int k = 0;
            for (int i = 0; i < NP; i++) if (orig[i].x >= x1 && orig[i].x <= x2 && orig[i].y >= y1 && orig[i].y <= y2) check(ids[k++] == i, "reported ids");
            rep_total += n;
        }
        sum += brute;
    }
    printf("500 count queries, total %ld; reported ids over 100 queries %ld\n", sum, rep_total);
    printf("binary-search comparisons: plain range tree %ld, with cascading %ld\n", plain_steps, casc_steps);
    check(casc_steps * 2 < plain_steps, "cascading needs fewer comparisons");
    destroy(root);
    return 0;
}
