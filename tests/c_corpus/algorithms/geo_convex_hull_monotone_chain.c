/*
 * title: Convex hull by Andrew's monotone chain
 * topic: algorithms
 * covers: convex hull, cross product, integer geometry, collinear handling, containment check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    long long x, y;
} Pt;

static unsigned long long rs = 88172645463325252ULL;

static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 16);
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

static int cmp_pt(const void *pa, const void *pb) {
    const Pt *a = pa, *b = pb;
    if (a->x != b->x)
        return a->x < b->x ? -1 : 1;
    if (a->y != b->y)
        return a->y < b->y ? -1 : 1;
    return 0;
}

/* Returns hull size in CCW order. keep_collinear selects the variant that
 * retains points lying on hull edges. Input is sorted and deduplicated. */
static int hull(const Pt *p, int n, Pt *out, int keep_collinear) {
    if (n < 3) {
        for (int i = 0; i < n; i++)
            out[i] = p[i];
        return n;
    }
    int all_line = 1;
    for (int i = 1; i < n - 1; i++)
        if (cross(p[0], p[n - 1], p[i]) != 0)
            all_line = 0;
    if (all_line && keep_collinear) {
        for (int i = 0; i < n; i++)
            out[i] = p[i];
        return n;
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        while (k >= 2) {
            long long c = cross(out[k - 2], out[k - 1], p[i]);
            if (c < 0 || (c == 0 && !keep_collinear))
                k--;
            else
                break;
        }
        out[k++] = p[i];
    }
    int lower = k + 1;
    for (int i = n - 2; i >= 0; i--) {
        while (k >= lower) {
            long long c = cross(out[k - 2], out[k - 1], p[i]);
            if (c < 0 || (c == 0 && !keep_collinear))
                k--;
            else
                break;
        }
        out[k++] = p[i];
    }
    return k - 1;
}

static long long area2(const Pt *h, int n) {
    long long s = 0;
    for (int i = 0; i < n; i++) {
        const Pt *a = &h[i], *b = &h[(i + 1) % n];
        s += a->x * b->y - a->y * b->x;
    }
    return s;
}

static int run_case(int n, int range, int seed_tag) {
    Pt *pts = malloc(sizeof(Pt) * (size_t)n);
    Pt *h1 = malloc(sizeof(Pt) * (size_t)(2 * n + 2));
    Pt *h2 = malloc(sizeof(Pt) * (size_t)(2 * n + 2));
    for (int i = 0; i < n; i++) {
        pts[i].x = (long long)(rnd() % (unsigned)range);
        pts[i].y = (long long)(rnd() % (unsigned)range);
    }
    qsort(pts, (size_t)n, sizeof(Pt), cmp_pt);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (m == 0 || cmp_pt(&pts[m - 1], &pts[i]) != 0)
            pts[m++] = pts[i];
    int a = hull(pts, m, h1, 0);
    int b = hull(pts, m, h2, 1);
    check(b >= a, "collinear variant is a superset");
    for (int i = 0; i < a && a >= 3; i++)
        check(cross(h1[i], h1[(i + 1) % a], h1[(i + 2) % a]) > 0, "strictly convex");
    for (int i = 0; i < m && a >= 3; i++)
        for (int j = 0; j < a; j++)
            check(cross(h1[j], h1[(j + 1) % a], pts[i]) >= 0, "all points inside");
    for (int i = 0; i < a; i++) {
        int found = 0;
        for (int j = 0; j < m; j++)
            if (cmp_pt(&h1[i], &pts[j]) == 0)
                found = 1;
        check(found, "hull vertex is an input point");
    }
    long long ar1 = area2(h1, a), ar2 = area2(h2, b);
    check(ar1 == ar2, "same area for both variants");
    printf("case %d: unique=%d strict=%d with_collinear=%d area2=%lld\n", seed_tag, m, a, b, ar1);
    free(pts);
    free(h1);
    free(h2);
    return a;
}

int main(void) {
    int total = 0;
    total += run_case(30, 100, 1);
    total += run_case(200, 1000, 2);
    total += run_case(100, 8, 3);  /* dense lattice: many collinear points */
    total += run_case(500, 20, 4);
    total += run_case(3, 5, 5);
    /* degenerate: all on one line */
    Pt line[6] = {{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}};
    Pt out[16];
    int s = hull(line, 6, out, 0);
    int c = hull(line, 6, out, 1);
    printf("line: strict=%d collinear=%d\n", s, c);
    check(s == 2, "line strict hull is a segment");
    printf("total hull vertices %d\n", total);
    return 0;
}
