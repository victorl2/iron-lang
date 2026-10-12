/*
 * title: Quickhull by recursive farthest-point splitting
 * topic: algorithms
 * covers: quickhull, divide and conquer, dynamic arrays, farthest point, degenerate inputs
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    long long x, y;
} Pt;

typedef struct {
    Pt *v;
    int n, cap;
} Hull;

static unsigned s = 0xC0FFEEu;

static unsigned rnd(void) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
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

static void push(Hull *h, Pt p) {
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 8;
        h->v = realloc(h->v, sizeof(Pt) * (size_t)h->cap);
        check(h->v != NULL, "alloc");
    }
    h->v[h->n++] = p;
}

static int depth_max, calls;

/* Emits hull vertices strictly to the right of a->b, walking from a to b (exclusive). */
static void side(Pt a, Pt b, const Pt *pts, int n, Hull *h, int depth) {
    calls++;
    if (depth > depth_max)
        depth_max = depth;
    long long best = 0;
    int bi = -1;
    for (int i = 0; i < n; i++) {
        long long c = -cross(a, b, pts[i]); /* positive on the right side */
        if (c > best) {
            best = c;
            bi = i;
        }
    }
    if (bi < 0)
        return;
    Pt far = pts[bi];
    Pt *left = malloc(sizeof(Pt) * (size_t)n), *right = malloc(sizeof(Pt) * (size_t)n);
    int nl = 0, nr = 0;
    for (int i = 0; i < n; i++) {
        if (-cross(a, far, pts[i]) > 0)
            left[nl++] = pts[i];
        else if (-cross(far, b, pts[i]) > 0)
            right[nr++] = pts[i];
    }
    side(a, far, left, nl, h, depth + 1);
    push(h, far);
    side(far, b, right, nr, h, depth + 1);
    free(left);
    free(right);
}

/* Counter-clockwise hull without collinear points. */
static void quickhull(const Pt *pts, int n, Hull *h) {
    h->n = 0;
    int lo = 0, hi = 0;
    for (int i = 1; i < n; i++) {
        if (pts[i].x < pts[lo].x || (pts[i].x == pts[lo].x && pts[i].y < pts[lo].y))
            lo = i;
        if (pts[i].x > pts[hi].x || (pts[i].x == pts[hi].x && pts[i].y > pts[hi].y))
            hi = i;
    }
    if (pts[lo].x == pts[hi].x && pts[lo].y == pts[hi].y) {
        push(h, pts[lo]);
        return;
    }
    push(h, pts[lo]);
    /* lower chain: points below lo->hi are on its right, walking lo to hi */
    side(pts[lo], pts[hi], pts, n, h, 0);
    push(h, pts[hi]);
    side(pts[hi], pts[lo], pts, n, h, 0);
}

static int in_hull(const Hull *h, Pt p) {
    if (h->n < 3)
        return 1;
    for (int i = 0; i < h->n; i++)
        if (cross(h->v[i], h->v[(i + 1) % h->n], p) < 0)
            return 0;
    return 1;
}

static long long area2(const Hull *h) {
    long long a = 0;
    for (int i = 0; i < h->n; i++) {
        Pt p = h->v[i], q = h->v[(i + 1) % h->n];
        a += p.x * q.y - p.y * q.x;
    }
    return a;
}

int main(void) {
    struct {
        const char *name;
        int n, range, shape;
    } cases[] = {
        {"uniform", 200, 1000, 0}, {"tiny-range", 100, 6, 0}, {"disc", 500, 400, 1},
        {"line", 20, 100, 2},      {"single", 5, 100, 3},     {"two-clusters", 300, 50, 4},
    };
    for (int c = 0; c < 6; c++) {
        int n = cases[c].n, r = cases[c].range;
        Pt *p = malloc(sizeof(Pt) * (size_t)n);
        for (int i = 0; i < n; i++) {
            long long x = (long long)(rnd() % (unsigned)r), y = (long long)(rnd() % (unsigned)r);
            switch (cases[c].shape) {
            case 1: /* keep points inside a disc of radius r/2 */
                x -= r / 2;
                y -= r / 2;
                while (x * x + y * y > (long long)(r / 2) * (r / 2)) {
                    x = (long long)(rnd() % (unsigned)r) - r / 2;
                    y = (long long)(rnd() % (unsigned)r) - r / 2;
                }
                break;
            case 2:
                y = 2 * x + 1;
                break;
            case 3:
                x = 7;
                y = 7;
                break;
            case 4:
                if (i & 1) {
                    x += 1000;
                    y += 1000;
                }
                break;
            default:
                break;
            }
            p[i] = (Pt){x, y};
        }
        Hull h = {0, 0, 0};
        depth_max = 0;
        calls = 0;
        quickhull(p, n, &h);
        for (int i = 0; i < n; i++)
            check(in_hull(&h, p[i]), "hull contains every point");
        for (int i = 0; i < h.n && h.n >= 3; i++)
            check(cross(h.v[i], h.v[(i + 1) % h.n], h.v[(i + 2) % h.n]) > 0, "strictly convex");
        long long a2 = h.n >= 3 ? area2(&h) : 0;
        printf("%-13s n=%3d hull=%2d area2=%7lld recursion_calls=%3d max_depth=%d\n", cases[c].name, n,
               h.n, a2, calls, depth_max);
        free(h.v);
        free(p);
    }
    return 0;
}
