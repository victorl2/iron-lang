/*
 * title: Area of a union of rectangles by sweep line
 * topic: algorithms
 * covers: sweep line, coordinate compression, segment tree with cover counts, events, grid painting check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int x1, y1, x2, y2;
} Rect;

typedef struct {
    int x, y1, y2, delta;
} Ev;

static unsigned s = 99991u;

static unsigned rnd(void) {
    s = s * 1103515245u + 12345u;
    return (s >> 8) & 0xFFFFFFu;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int ys[1024];
static int ny;
static int cover[4096];
static int len[4096];

static int cmp_int(const void *a, const void *b) {
    return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}

static int cmp_ev(const void *pa, const void *pb) {
    const Ev *a = pa, *b = pb;
    if (a->x != b->x)
        return a->x < b->x ? -1 : 1;
    return b->delta - a->delta; /* adds before removals: they only matter at equal x with zero width */
}

static int find_y(int v) {
    int lo = 0, hi = ny - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (ys[mid] < v)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* node covers elementary intervals [l, r) of the compressed y axis */
static void update(int node, int l, int r, int ql, int qr, int delta) {
    if (qr <= l || r <= ql)
        return;
    if (ql <= l && r <= qr) {
        cover[node] += delta;
    } else {
        int mid = (l + r) / 2;
        update(node * 2, l, mid, ql, qr, delta);
        update(node * 2 + 1, mid, r, ql, qr, delta);
    }
    if (cover[node] > 0)
        len[node] = ys[r] - ys[l];
    else if (r - l == 1)
        len[node] = 0;
    else
        len[node] = len[node * 2] + len[node * 2 + 1];
}

static long long union_area(const Rect *rc, int n) {
    Ev *ev = malloc(sizeof(Ev) * (size_t)(2 * n));
    int m = 0, k = 0;
    int *raw = malloc(sizeof(int) * (size_t)(2 * n));
    for (int i = 0; i < n; i++) {
        raw[k++] = rc[i].y1;
        raw[k++] = rc[i].y2;
        ev[m++] = (Ev){rc[i].x1, rc[i].y1, rc[i].y2, +1};
        ev[m++] = (Ev){rc[i].x2, rc[i].y1, rc[i].y2, -1};
    }
    qsort(raw, (size_t)k, sizeof(int), cmp_int);
    ny = 0;
    for (int i = 0; i < k; i++)
        if (ny == 0 || ys[ny - 1] != raw[i])
            ys[ny++] = raw[i];
    qsort(ev, (size_t)m, sizeof(Ev), cmp_ev);
    for (int i = 0; i < 4096; i++)
        cover[i] = len[i] = 0;
    long long area = 0;
    for (int i = 0; i < m; i++) {
        if (i > 0)
            area += (long long)len[1] * (ev[i].x - ev[i - 1].x);
        update(1, 0, ny - 1, find_y(ev[i].y1), find_y(ev[i].y2), ev[i].delta);
    }
    free(ev);
    free(raw);
    return area;
}

static long long paint_area(const Rect *rc, int n) {
    static unsigned char grid[128][128];
    for (int i = 0; i < 128; i++)
        for (int j = 0; j < 128; j++)
            grid[i][j] = 0;
    for (int r = 0; r < n; r++)
        for (int x = rc[r].x1; x < rc[r].x2; x++)
            for (int y = rc[r].y1; y < rc[r].y2; y++)
                grid[x][y] = 1;
    long long a = 0;
    for (int i = 0; i < 128; i++)
        for (int j = 0; j < 128; j++)
            a += grid[i][j];
    return a;
}

int main(void) {
    Rect fixed[] = {{0, 0, 4, 4}, {2, 2, 6, 6}, {1, 1, 3, 3}, {10, 10, 12, 14}};
    long long fa = union_area(fixed, 4);
    printf("fixed: union=%lld (sum of areas 16+16+4+8=44)\n", fa);
    check(fa == 16 + 16 - 4 + 8, "fixed union");
    long long total = 0;
    int counts[] = {1, 2, 5, 10, 25, 60, 120};
    for (int t = 0; t < 7; t++) {
        int n = counts[t];
        Rect rc[128];
        long long naive = 0;
        for (int i = 0; i < n; i++) {
            int x1 = (int)(rnd() % 100), y1 = (int)(rnd() % 100);
            int w = 1 + (int)(rnd() % (unsigned)(5 + 60 / (t + 1))), h = 1 + (int)(rnd() % (unsigned)(5 + 60 / (t + 1)));
            rc[i] = (Rect){x1, y1, x1 + w > 127 ? 127 : x1 + w, y1 + h > 127 ? 127 : y1 + h};
            naive += (long long)(rc[i].x2 - rc[i].x1) * (rc[i].y2 - rc[i].y1);
        }
        long long a = union_area(rc, n);
        long long b = paint_area(rc, n);
        check(a == b, "sweep equals grid painting");
        printf("rects=%3d sum_areas=%6lld union=%6lld overlap_saved=%6lld distinct_y=%d\n", n, naive, a,
               naive - a, ny);
        total += a;
    }
    printf("total=%lld\n", total);
    return 0;
}
