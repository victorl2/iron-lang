/*
 * title: Polygon area, orientation, centroid and simplicity
 * topic: algorithms
 * covers: shoelace formula, signed area, winding orientation, convexity test, simple polygon check, centroid
 * deps: libc, libm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    long long x, y;
} Pt;

typedef struct {
    const char *name;
    Pt v[12];
    int n;
} Poly;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static long long cross3(Pt o, Pt a, Pt b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

static long long area2_shoelace(const Poly *p) {
    long long s = 0;
    for (int i = 0; i < p->n; i++) {
        Pt a = p->v[i], b = p->v[(i + 1) % p->n];
        s += a.x * b.y - a.y * b.x;
    }
    return s;
}

/* Same quantity through a triangle fan anchored at vertex 0. */
static long long area2_fan(const Poly *p) {
    long long s = 0;
    for (int i = 1; i + 1 < p->n; i++)
        s += cross3(p->v[0], p->v[i], p->v[i + 1]);
    return s;
}

/* Trapezoid rule: sum of (x_{i+1}-x_i)*(y_i+y_{i+1}), negated for CCW. */
static long long area2_trapezoid(const Poly *p) {
    long long s = 0;
    for (int i = 0; i < p->n; i++) {
        Pt a = p->v[i], b = p->v[(i + 1) % p->n];
        s += (b.x - a.x) * (a.y + b.y);
    }
    return -s;
}

/* 1 = convex (all turns same sign, ignoring collinear), 0 = concave. */
static int is_convex(const Poly *p) {
    int pos = 0, neg = 0;
    for (int i = 0; i < p->n; i++) {
        long long c = cross3(p->v[i], p->v[(i + 1) % p->n], p->v[(i + 2) % p->n]);
        if (c > 0)
            pos = 1;
        if (c < 0)
            neg = 1;
    }
    return !(pos && neg);
}

static int sgn(long long v) {
    return (v > 0) - (v < 0);
}

static int seg_touch(Pt a, Pt b, Pt c, Pt d) {
    int o1 = sgn(cross3(a, b, c)), o2 = sgn(cross3(a, b, d));
    int o3 = sgn(cross3(c, d, a)), o4 = sgn(cross3(c, d, b));
    if (o1 * o2 < 0 && o3 * o4 < 0)
        return 1;
    /* touching or collinear overlap: bounding boxes overlap and no strict separation */
    if (o1 * o2 > 0 || o3 * o4 > 0)
        return 0;
    long long lx1 = a.x < b.x ? a.x : b.x, hx1 = a.x < b.x ? b.x : a.x;
    long long lx2 = c.x < d.x ? c.x : d.x, hx2 = c.x < d.x ? d.x : c.x;
    long long ly1 = a.y < b.y ? a.y : b.y, hy1 = a.y < b.y ? b.y : a.y;
    long long ly2 = c.y < d.y ? c.y : d.y, hy2 = c.y < d.y ? d.y : c.y;
    return !(hx1 < lx2 || hx2 < lx1 || hy1 < ly2 || hy2 < ly1);
}

/* Simple: non-adjacent edges never meet, adjacent edges only share their common vertex. */
static int is_simple(const Poly *p) {
    int n = p->n;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            Pt a = p->v[i], b = p->v[(i + 1) % n], c = p->v[j], d = p->v[(j + 1) % n];
            if (j == i + 1 || (i == 0 && j == n - 1)) {
                /* adjacent edges: reject only if they fold back on each other */
                Pt shared = (j == i + 1) ? b : a;
                Pt other1 = (j == i + 1) ? a : b, other2 = (j == i + 1) ? d : c;
                if (cross3(shared, other1, other2) == 0 &&
                    (other1.x - shared.x) * (other2.x - shared.x) + (other1.y - shared.y) * (other2.y - shared.y) > 0)
                    return 0;
                continue;
            }
            if (seg_touch(a, b, c, d))
                return 0;
        }
    }
    return 1;
}

int main(void) {
    Poly polys[] = {
        {"square-ccw", {{0, 0}, {4, 0}, {4, 4}, {0, 4}}, 4},
        {"square-cw", {{0, 0}, {0, 4}, {4, 4}, {4, 0}}, 4},
        {"L-shape", {{0, 0}, {6, 0}, {6, 2}, {2, 2}, {2, 6}, {0, 6}}, 6},
        {"star", {{0, 3}, {1, 1}, {3, 0}, {1, -1}, {0, -3}, {-1, -1}, {-3, 0}, {-1, 1}}, 8},
        {"bowtie", {{0, 0}, {4, 4}, {4, 0}, {0, 4}}, 4},
        {"triangle", {{1, 1}, {8, 2}, {3, 9}}, 3},
        {"comb", {{0, 0}, {9, 0}, {9, 5}, {7, 5}, {7, 2}, {5, 2}, {5, 5}, {3, 5}, {3, 2}, {1, 2}, {1, 5}, {0, 5}}, 12},
        {"collinear-mid", {{0, 0}, {2, 0}, {4, 0}, {4, 3}, {0, 3}}, 5},
    };
    int np = (int)(sizeof polys / sizeof polys[0]);
    for (int i = 0; i < np; i++) {
        Poly *p = &polys[i];
        long long a = area2_shoelace(p);
        check(a == area2_fan(p), "fan matches shoelace");
        check(a == area2_trapezoid(p), "trapezoid matches shoelace");
        int simple = is_simple(p);
        const char *orient = a > 0 ? "ccw" : (a < 0 ? "cw" : "degenerate");
        printf("%-13s n=%2d area2=%4lld orient=%-3s convex=%d simple=%d", p->name, p->n, a, orient,
               is_convex(p), simple);
        if (simple && a != 0) {
            /* centroid: sum (p_i + p_{i+1}) * cross / (3 * area2) */
            long long cx = 0, cy = 0;
            for (int k = 0; k < p->n; k++) {
                Pt u = p->v[k], v = p->v[(k + 1) % p->n];
                long long cr = u.x * v.y - u.y * v.x;
                cx += (u.x + v.x) * cr;
                cy += (u.y + v.y) * cr;
            }
            double gx = (double)cx / (3.0 * (double)a), gy = (double)cy / (3.0 * (double)a);
            printf(" centroid=(%.4f, %.4f)", fabs(gx) < 5e-5 ? 0.0 : gx, fabs(gy) < 5e-5 ? 0.0 : gy);
            /* translating and reversing must preserve |area| */
            Poly q = *p;
            for (int k = 0; k < q.n; k++) {
                q.v[k].x = p->v[q.n - 1 - k].x + 100;
                q.v[k].y = p->v[q.n - 1 - k].y - 37;
            }
            check(area2_shoelace(&q) == -a, "reverse + translate flips sign only");
        }
        printf("\n");
    }
    check(area2_shoelace(&polys[0]) == 32, "square area");
    check(is_simple(&polys[2]) && !is_convex(&polys[2]), "L-shape is simple and concave");
    check(!is_simple(&polys[4]), "bowtie is not simple");
    return 0;
}
