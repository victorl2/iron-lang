/*
 * title: Ear clipping triangulation of simple polygons
 * topic: algorithms
 * covers: ear clipping, triangulation, reflex vertices, point in triangle, angular sort, linked vertex ring
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    long long x, y;
} Pt;

typedef struct {
    int a, b, c;
} Tri;

static unsigned s = 5150u;

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

static long long cross3(Pt o, Pt a, Pt b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

static int in_tri_closed(Pt a, Pt b, Pt c, Pt p) {
    return cross3(a, b, p) >= 0 && cross3(b, c, p) >= 0 && cross3(c, a, p) >= 0;
}

/* Triangulates a counter-clockwise simple polygon. Returns the number of triangles. */
static int ear_clip(const Pt *p, int n, Tri *out, int *removed_collinear) {
    int *next = malloc(sizeof(int) * (size_t)n), *prev = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) {
        next[i] = (i + 1) % n;
        prev[i] = (i + n - 1) % n;
    }
    int left = n, cur = 0, nt = 0, stall = 0;
    *removed_collinear = 0;
    while (left > 3) {
        int a = prev[cur], b = cur, c = next[cur];
        long long turn = cross3(p[a], p[b], p[c]);
        int ear = 0;
        if (turn > 0) {
            ear = 1;
            for (int v = next[c]; v != a; v = next[v])
                if (in_tri_closed(p[a], p[b], p[c], p[v]) && !(p[v].x == p[a].x && p[v].y == p[a].y) &&
                    !(p[v].x == p[c].x && p[v].y == p[c].y)) {
                    ear = 0;
                    break;
                }
        } else if (turn == 0) {
            /* a collinear vertex adds nothing: drop it without emitting a triangle */
            next[a] = c;
            prev[c] = a;
            left--;
            (*removed_collinear)++;
            cur = c;
            stall = 0;
            continue;
        }
        if (ear) {
            out[nt++] = (Tri){a, b, c};
            next[a] = c;
            prev[c] = a;
            left--;
            cur = c;
            stall = 0;
        } else {
            cur = next[cur];
            check(++stall <= 2 * left + 2, "an ear always exists in a simple polygon");
        }
    }
    int a = cur, b = next[cur], c = next[b];
    if (cross3(p[a], p[b], p[c]) > 0)
        out[nt++] = (Tri){a, b, c};
    free(next);
    free(prev);
    return nt;
}

static int half(Pt v) {
    return (v.y > 0 || (v.y == 0 && v.x > 0)) ? 0 : 1;
}

static int cmp_angle(const void *pa, const void *pb) {
    const Pt *a = pa, *b = pb;
    int ha = half(*a), hb = half(*b);
    if (ha != hb)
        return ha - hb;
    long long c = a->x * b->y - a->y * b->x;
    if (c != 0)
        return c > 0 ? -1 : 1;
    return 0;
}

int main(void) {
    struct {
        const char *name;
        Pt v[16];
        int n;
    } fixed[] = {
        {"square", {{0, 0}, {4, 0}, {4, 4}, {0, 4}}, 4},
        {"arrow", {{0, 0}, {6, 3}, {0, 6}, {2, 3}}, 4},
        {"L-shape", {{0, 0}, {6, 0}, {6, 2}, {2, 2}, {2, 6}, {0, 6}}, 6},
        {"square+midpoints", {{0, 0}, {2, 0}, {4, 0}, {4, 2}, {4, 4}, {2, 4}, {0, 4}, {0, 2}}, 8},
        {"comb", {{0, 0}, {9, 0}, {9, 5}, {7, 5}, {7, 2}, {5, 2}, {5, 5}, {3, 5}, {3, 2}, {1, 2}, {1, 5}, {0, 5}}, 12},
    };
    for (int i = 0; i < 5; i++) {
        Tri tr[16];
        int rc;
        int nt = ear_clip(fixed[i].v, fixed[i].n, tr, &rc);
        long long a2 = 0, t2 = 0;
        for (int k = 0; k < fixed[i].n; k++)
            a2 += fixed[i].v[k].x * fixed[i].v[(k + 1) % fixed[i].n].y -
                  fixed[i].v[k].y * fixed[i].v[(k + 1) % fixed[i].n].x;
        for (int k = 0; k < nt; k++)
            t2 += cross3(fixed[i].v[tr[k].a], fixed[i].v[tr[k].b], fixed[i].v[tr[k].c]);
        check(t2 == a2, "triangle areas add up to polygon area");
        check(nt == fixed[i].n - 2 - rc, "triangle count is n-2 minus dropped collinear vertices");
        printf("%-17s n=%2d triangles=%2d collinear_dropped=%d area2=%lld\n", fixed[i].name, fixed[i].n, nt, rc, a2);
    }
    /* random star-shaped polygons: sort random points by angle around the origin */
    int total_tris = 0, total_reflex = 0;
    for (int t = 0; t < 40; t++) {
        int want = 5 + (int)(rnd() % 25);
        Pt raw[64];
        int m = 0;
        for (int tries = 0; tries < 400 && m < want; tries++) {
            Pt c = {(long long)(rnd() % 81) - 40, (long long)(rnd() % 81) - 40};
            if (c.x == 0 && c.y == 0)
                continue;
            int dup = 0;
            for (int i = 0; i < m; i++)
                if (raw[i].x * c.y - raw[i].y * c.x == 0 && raw[i].x * c.x + raw[i].y * c.y > 0)
                    dup = 1; /* same ray: would make the polygon non-simple */
            if (!dup)
                raw[m++] = c;
        }
        qsort(raw, (size_t)m, sizeof(Pt), cmp_angle);
        Tri tr[64];
        int rc;
        int nt = ear_clip(raw, m, tr, &rc);
        long long a2 = 0, t2 = 0;
        int reflex = 0;
        for (int k = 0; k < m; k++) {
            a2 += raw[k].x * raw[(k + 1) % m].y - raw[k].y * raw[(k + 1) % m].x;
            reflex += cross3(raw[k], raw[(k + 1) % m], raw[(k + 2) % m]) < 0;
        }
        for (int k = 0; k < nt; k++)
            t2 += cross3(raw[tr[k].a], raw[tr[k].b], raw[tr[k].c]);
        check(a2 > 0, "star polygon is counter-clockwise");
        check(t2 == a2, "random polygon area preserved");
        check(nt + rc == m - 2, "triangle count");
        total_tris += nt;
        total_reflex += reflex;
    }
    printf("random star polygons: triangles=%d reflex_vertices=%d\n", total_tris, total_reflex);
    return 0;
}
