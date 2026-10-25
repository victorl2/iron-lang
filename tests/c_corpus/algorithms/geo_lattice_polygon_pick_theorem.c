/*
 * title: Lattice polygons and Pick's theorem
 * topic: algorithms
 * covers: Pick's theorem, gcd on edges, boundary lattice points, shoelace, lattice point census, integer geometry
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    long long x, y;
} Pt;

static unsigned s = 0xBADC0DEu;

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

static long long area2(const Pt *p, int n) {
    long long a = 0;
    for (int i = 0; i < n; i++)
        a += p[i].x * p[(i + 1) % n].y - p[i].y * p[(i + 1) % n].x;
    return a < 0 ? -a : a;
}

static long long boundary_pts(const Pt *p, int n) {
    long long b = 0;
    for (int i = 0; i < n; i++)
        b += gcdll(p[(i + 1) % n].x - p[i].x, p[(i + 1) % n].y - p[i].y);
    return b;
}

static long long cross3(Pt o, Pt a, Pt b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

static int on_seg(Pt a, Pt b, Pt p) {
    if (cross3(a, b, p) != 0)
        return 0;
    return p.x >= (a.x < b.x ? a.x : b.x) && p.x <= (a.x > b.x ? a.x : b.x) &&
           p.y >= (a.y < b.y ? a.y : b.y) && p.y <= (a.y > b.y ? a.y : b.y);
}

/* Census by scanning the bounding box: counts strict interior and boundary lattice points. */
static void census(const Pt *p, int n, long long *inside, long long *bound) {
    long long x0 = p[0].x, x1 = p[0].x, y0 = p[0].y, y1 = p[0].y;
    for (int i = 1; i < n; i++) {
        if (p[i].x < x0)
            x0 = p[i].x;
        if (p[i].x > x1)
            x1 = p[i].x;
        if (p[i].y < y0)
            y0 = p[i].y;
        if (p[i].y > y1)
            y1 = p[i].y;
    }
    *inside = *bound = 0;
    for (long long y = y0; y <= y1; y++)
        for (long long x = x0; x <= x1; x++) {
            Pt q = {x, y};
            int on = 0, in = 0;
            for (int i = 0; i < n; i++) {
                Pt a = p[i], b = p[(i + 1) % n];
                if (on_seg(a, b, q)) {
                    on = 1;
                    break;
                }
                if ((a.y <= y) != (b.y <= y)) {
                    long long c = cross3(a, b, q);
                    if (b.y > a.y ? c > 0 : c < 0)
                        in = !in;
                }
            }
            if (on)
                (*bound)++;
            else if (in)
                (*inside)++;
        }
}

int main(void) {
    struct {
        const char *name;
        Pt v[8];
        int n;
    } fixed[] = {
        {"unit-square", {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, 4},
        {"triangle-3-4", {{0, 0}, {3, 0}, {0, 4}}, 3},
        {"sheared", {{0, 0}, {7, 3}, {12, 8}, {5, 5}}, 4},
        {"empty-triangle", {{0, 0}, {1, 0}, {0, 1}}, 3},
        {"thin-sliver", {{0, 0}, {10, 3}, {7, 2}}, 3},
        {"hexagon", {{2, 0}, {5, 0}, {7, 3}, {5, 6}, {2, 6}, {0, 3}}, 6},
    };
    for (int i = 0; i < 6; i++) {
        long long a2 = area2(fixed[i].v, fixed[i].n);
        long long b = boundary_pts(fixed[i].v, fixed[i].n);
        long long in_pick = (a2 - b + 2) / 2;
        long long in_c, b_c;
        census(fixed[i].v, fixed[i].n, &in_c, &b_c);
        check((a2 - b + 2) % 2 == 0, "parity of Pick expression");
        check(in_c == in_pick && b_c == b, "census matches Pick");
        printf("%-15s 2A=%3lld B=%2lld I=%3lld\n", fixed[i].name, a2, b, in_pick);
    }
    /* random convex-ish lattice triangles and quadrilaterals */
    long long sum_i = 0, sum_b = 0;
    int tested = 0;
    for (int t = 0; t < 60; t++) {
        Pt tri[3];
        for (int i = 0; i < 3; i++)
            tri[i] = (Pt){(long long)(rnd() % 25), (long long)(rnd() % 25)};
        long long a2 = area2(tri, 3);
        if (a2 == 0)
            continue;
        long long b = boundary_pts(tri, 3);
        long long in_c, b_c;
        census(tri, 3, &in_c, &b_c);
        check(b_c == b, "triangle boundary count");
        check(2 * in_c + b - 2 == a2, "Pick on random triangle");
        sum_i += in_c;
        sum_b += b;
        tested++;
    }
    printf("random triangles: tested=%d sum_interior=%lld sum_boundary=%lld\n", tested, sum_i, sum_b);
    /* Ehrhart-style scaling: a lattice polygon scaled by k has area k^2 A and boundary k B */
    Pt base[] = {{0, 0}, {3, 1}, {2, 4}};
    long long a1 = area2(base, 3), b1 = boundary_pts(base, 3);
    for (int k = 1; k <= 5; k++) {
        Pt sc[3];
        for (int i = 0; i < 3; i++)
            sc[i] = (Pt){base[i].x * k, base[i].y * k};
        long long a = area2(sc, 3), b = boundary_pts(sc, 3);
        check(a == a1 * k * k && b == b1 * k, "scaling law");
        printf("scale %d: interior=%lld boundary=%lld\n", k, (a - b + 2) / 2, b);
    }
    return 0;
}
