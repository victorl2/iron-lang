/*
 * title: Segment intersection classification with exact points
 * topic: algorithms
 * covers: orientation predicate, proper/touching/overlapping segments, rational intersection point, gcd reduction
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    long long x, y;
} Pt;

typedef struct {
    Pt a, b;
} Seg;

typedef enum { DISJOINT, PROPER, TOUCH, OVERLAP, COLLINEAR_DISJOINT } Kind;

static const char *kind_name[] = {"disjoint", "proper", "touch", "overlap", "collinear-disjoint"};

static unsigned s = 7u;

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

static int sgn(long long v) {
    return (v > 0) - (v < 0);
}

static int orient(Pt a, Pt b, Pt c) {
    return sgn((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
}

static long long llmin(long long a, long long b) {
    return a < b ? a : b;
}

static long long llmax(long long a, long long b) {
    return a > b ? a : b;
}

static int on_seg(Pt a, Pt b, Pt p) {
    return orient(a, b, p) == 0 && p.x >= llmin(a.x, b.x) && p.x <= llmax(a.x, b.x) &&
           p.y >= llmin(a.y, b.y) && p.y <= llmax(a.y, b.y);
}

static Kind classify(Seg s1, Seg s2) {
    int o1 = orient(s1.a, s1.b, s2.a), o2 = orient(s1.a, s1.b, s2.b);
    int o3 = orient(s2.a, s2.b, s1.a), o4 = orient(s2.a, s2.b, s1.b);
    if (o1 * o2 < 0 && o3 * o4 < 0)
        return PROPER;
    if (o1 == 0 && o2 == 0) {
        /* collinear: project onto the dominant axis */
        int any = on_seg(s1.a, s1.b, s2.a) || on_seg(s1.a, s1.b, s2.b) || on_seg(s2.a, s2.b, s1.a) ||
                  on_seg(s2.a, s2.b, s1.b);
        if (!any)
            return COLLINEAR_DISJOINT;
        /* a single shared endpoint counts as touch, a shared stretch as overlap */
        Pt lo1 = s1.a, hi1 = s1.b, lo2 = s2.a, hi2 = s2.b;
        int use_x = llmax(s1.a.x, s1.b.x) - llmin(s1.a.x, s1.b.x) >= llmax(s1.a.y, s1.b.y) - llmin(s1.a.y, s1.b.y);
        long long l1 = use_x ? llmin(lo1.x, hi1.x) : llmin(lo1.y, hi1.y);
        long long h1 = use_x ? llmax(lo1.x, hi1.x) : llmax(lo1.y, hi1.y);
        long long l2 = use_x ? llmin(lo2.x, hi2.x) : llmin(lo2.y, hi2.y);
        long long h2 = use_x ? llmax(lo2.x, hi2.x) : llmax(lo2.y, hi2.y);
        long long lo = llmax(l1, l2), hi = llmin(h1, h2);
        return lo == hi ? TOUCH : OVERLAP;
    }
    if (on_seg(s1.a, s1.b, s2.a) || on_seg(s1.a, s1.b, s2.b) || on_seg(s2.a, s2.b, s1.a) ||
        on_seg(s2.a, s2.b, s1.b))
        return TOUCH;
    return DISJOINT;
}

static long long gcdll(long long a, long long b) {
    if (a < 0)
        a = -a;
    if (b < 0)
        b = -b;
    while (b) {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Intersection point of two non-parallel lines as reduced fractions xn/d, yn/d (d > 0). */
static void meet(Seg s1, Seg s2, long long *xn, long long *yn, long long *d) {
    long long rx = s1.b.x - s1.a.x, ry = s1.b.y - s1.a.y;
    long long sx = s2.b.x - s2.a.x, sy = s2.b.y - s2.a.y;
    long long den = rx * sy - ry * sx;
    long long tn = (s2.a.x - s1.a.x) * sy - (s2.a.y - s1.a.y) * sx; /* t = tn/den */
    long long nx = s1.a.x * den + tn * rx, ny = s1.a.y * den + tn * ry;
    if (den < 0) {
        den = -den;
        nx = -nx;
        ny = -ny;
    }
    long long g = gcdll(gcdll(nx, ny), den);
    if (g == 0)
        g = 1;
    *xn = nx / g;
    *yn = ny / g;
    *d = den / g;
}

/* Independent reference: brute force over a fine lattice of half-integer parameters is
 * impractical, so instead compare the bounding-box + straddle formulation. */
static int reference_intersects(Seg s1, Seg s2) {
    if (llmax(s1.a.x, s1.b.x) < llmin(s2.a.x, s2.b.x) || llmax(s2.a.x, s2.b.x) < llmin(s1.a.x, s1.b.x))
        return 0;
    if (llmax(s1.a.y, s1.b.y) < llmin(s2.a.y, s2.b.y) || llmax(s2.a.y, s2.b.y) < llmin(s1.a.y, s1.b.y))
        return 0;
    return orient(s1.a, s1.b, s2.a) * orient(s1.a, s1.b, s2.b) <= 0 &&
           orient(s2.a, s2.b, s1.a) * orient(s2.a, s2.b, s1.b) <= 0;
}

int main(void) {
    Seg fixed[][2] = {
        {{{0, 0}, {4, 4}}, {{0, 4}, {4, 0}}},
        {{{0, 0}, {4, 0}}, {{4, 0}, {4, 4}}},
        {{{0, 0}, {4, 0}}, {{2, 0}, {6, 0}}},
        {{{0, 0}, {4, 0}}, {{5, 0}, {6, 0}}},
        {{{0, 0}, {4, 0}}, {{4, 0}, {8, 0}}},
        {{{0, 0}, {6, 3}}, {{2, 3}, {5, 0}}},
        {{{1, 1}, {3, 3}}, {{2, 0}, {6, 0}}},
        {{{0, 0}, {0, 5}}, {{0, 2}, {0, 9}}},
        {{{0, 0}, {5, 5}}, {{2, 2}, {3, 3}}},
        {{{0, 0}, {2, 0}}, {{1, -1}, {1, 0}}},
    };
    for (int i = 0; i < 10; i++) {
        Kind k = classify(fixed[i][0], fixed[i][1]);
        printf("fixed %d: %s", i, kind_name[k]);
        if (k == PROPER) {
            long long xn, yn, d;
            meet(fixed[i][0], fixed[i][1], &xn, &yn, &d);
            printf(" at (%lld/%lld, %lld/%lld)", xn, d, yn, d);
        }
        printf("\n");
    }
    int counts[5] = {0};
    long proper_checked = 0;
    for (int it = 0; it < 4000; it++) {
        Seg a, b;
        long long *f[8] = {&a.a.x, &a.a.y, &a.b.x, &a.b.y, &b.a.x, &b.a.y, &b.b.x, &b.b.y};
        for (int i = 0; i < 8; i++)
            *f[i] = (long long)(rnd() % 9);
        if ((a.a.x == a.b.x && a.a.y == a.b.y) || (b.a.x == b.b.x && b.a.y == b.b.y))
            continue;
        Kind k = classify(a, b);
        counts[k]++;
        check((k != DISJOINT && k != COLLINEAR_DISJOINT) == reference_intersects(a, b), "matches reference");
        Kind k2 = classify(b, a);
        check(k == k2, "symmetric");
        if (k == PROPER) {
            long long xn, yn, d;
            meet(a, b, &xn, &yn, &d);
            /* the point must lie on both supporting lines: cross product zero in rationals */
            Pt p1 = {xn, yn}, a1 = {a.a.x * d, a.a.y * d}, b1 = {a.b.x * d, a.b.y * d};
            Pt a2 = {b.a.x * d, b.a.y * d}, b2 = {b.b.x * d, b.b.y * d};
            check(orient(a1, b1, p1) == 0, "point on first line");
            check(orient(a2, b2, p1) == 0, "point on second line");
            proper_checked++;
        }
    }
    for (int k = 0; k < 5; k++)
        printf("%-19s %d\n", kind_name[k], counts[k]);
    printf("proper points verified: %ld\n", proper_checked);
    return 0;
}
