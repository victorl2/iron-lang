/*
 * title: Graham scan with exact polar ordering
 * topic: algorithms
 * covers: graham scan, angular sort, cross-product comparator, stack, tie-breaking by distance
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    long long x, y;
} Pt;

static unsigned s = 2463534242u;

static unsigned rnd(void) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static long long cross(Pt o, Pt a, Pt b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

static long long dist2(Pt a, Pt b) {
    return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y);
}

static Pt pivot;

/* Order by polar angle around pivot (all points are above or right of it),
 * nearer points first when collinear. This is a total order on distinct points. */
static int cmp_polar(const void *pa, const void *pb) {
    const Pt *a = pa, *b = pb;
    long long c = cross(pivot, *a, *b);
    if (c != 0)
        return c > 0 ? -1 : 1;
    long long da = dist2(pivot, *a), db = dist2(pivot, *b);
    if (da != db)
        return da < db ? -1 : 1;
    return 0;
}

static int graham(Pt *p, int n, Pt *st) {
    int lo = 0;
    for (int i = 1; i < n; i++)
        if (p[i].y < p[lo].y || (p[i].y == p[lo].y && p[i].x < p[lo].x))
            lo = i;
    Pt t = p[0];
    p[0] = p[lo];
    p[lo] = t;
    pivot = p[0];
    qsort(p + 1, (size_t)(n - 1), sizeof(Pt), cmp_polar);
    /* popping on <= 0 discards collinear points, so ties need no special casing */
    int k = 0;
    for (int i = 0; i < n; i++) {
        while (k >= 2 && cross(st[k - 2], st[k - 1], p[i]) <= 0)
            k--;
        st[k++] = p[i];
    }
    return k;
}

/* O(n^3) reference: count strict hull edges directly. An ordered pair (i,j) is a
 * hull edge when every point is left of it, or on it strictly inside the span. */
static int brute_vertex_count(const Pt *p, int n) {
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        int is_vertex = 0;
        for (int j = 0; j < n && !is_vertex; j++) {
            if (i == j)
                continue;
            int ok = 1, has_left = 0;
            for (int k = 0; k < n && ok; k++) {
                long long c = cross(p[i], p[j], p[k]);
                if (c > 0)
                    has_left = 1;
                else if (c < 0)
                    ok = 0;
                else {
                    /* on the line: must be within [i, j] or, for endpoint status of i,
                     * must not lie behind i */
                    long long dot = (p[k].x - p[i].x) * (p[j].x - p[i].x) +
                                    (p[k].y - p[i].y) * (p[j].y - p[i].y);
                    if (dot < 0)
                        ok = 0;
                }
            }
            if (ok && has_left)
                is_vertex = 1;
        }
        cnt += is_vertex;
    }
    return cnt;
}

int main(void) {
    int sizes[] = {12, 25, 60, 90};
    int ranges[] = {10, 15, 50, 9};
    for (int t = 0; t < 4; t++) {
        int n = sizes[t];
        Pt *p = malloc(sizeof(Pt) * (size_t)n);
        Pt *keep = malloc(sizeof(Pt) * (size_t)n);
        Pt *st = malloc(sizeof(Pt) * (size_t)(n + 1));
        int m = 0;
        while (m < n) {
            Pt c = {(long long)(rnd() % (unsigned)ranges[t]), (long long)(rnd() % (unsigned)ranges[t])};
            int dup = 0;
            for (int i = 0; i < m; i++)
                if (p[i].x == c.x && p[i].y == c.y)
                    dup = 1;
            if (!dup && m < ranges[t] * ranges[t])
                p[m++] = c;
            if (m >= ranges[t] * ranges[t])
                break;
        }
        n = m;
        for (int i = 0; i < n; i++)
            keep[i] = p[i];
        int h = graham(p, n, st);
        check(h == brute_vertex_count(keep, n), "vertex count matches brute force");
        long long a2 = 0, per2 = 0;
        for (int i = 0; i < h; i++) {
            Pt a = st[i], b = st[(i + 1) % h];
            a2 += a.x * b.y - a.y * b.x;
            per2 += dist2(a, b);
            check(cross(a, b, st[(i + 2) % h]) > 0, "left turns only");
        }
        for (int i = 0; i < n; i++)
            for (int j = 0; j < h; j++)
                check(cross(st[j], st[(j + 1) % h], keep[i]) >= 0, "contains all points");
        printf("n=%d hull=%d area2=%lld sum_sq_edges=%lld first=(%lld,%lld)\n", n, h, a2, per2,
               st[0].x, st[0].y);
        free(p);
        free(keep);
        free(st);
    }
    /* fixed example with points on the final ray back to the pivot */
    Pt fx[] = {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 2}, {2, 0}, {2, 2}, {0, 1}, {3, 3}};
    Pt st[16];
    int h = graham(fx, 9, st);
    printf("fixed hull:");
    for (int i = 0; i < h; i++)
        printf(" (%lld,%lld)", st[i].x, st[i].y);
    printf("\n");
    check(h == 4, "square hull");
    return 0;
}
