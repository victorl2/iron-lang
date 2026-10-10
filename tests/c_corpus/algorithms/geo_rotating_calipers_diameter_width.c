/*
 * title: Rotating calipers for diameter and minimum width
 * topic: algorithms
 * covers: rotating calipers, antipodal pairs, convex hull, two pointers on polygon, exact fraction comparison
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    long long x, y;
} Pt;

static unsigned s = 424242u;

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

static long long d2(Pt a, Pt b) {
    return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y);
}

static int cmp_pt(const void *pa, const void *pb) {
    const Pt *a = pa, *b = pb;
    if (a->x != b->x)
        return a->x < b->x ? -1 : 1;
    return (a->y > b->y) - (a->y < b->y);
}

static int hull(Pt *p, int n, Pt *h) {
    qsort(p, (size_t)n, sizeof(Pt), cmp_pt);
    int k = 0;
    for (int i = 0; i < n; i++) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0)
            k--;
        h[k++] = p[i];
    }
    for (int i = n - 2, lo = k + 1; i >= 0; i--) {
        while (k >= lo && cross(h[k - 2], h[k - 1], p[i]) <= 0)
            k--;
        h[k++] = p[i];
    }
    return k - 1;
}

/* Diameter (squared) by advancing the antipodal pointer while the triangle area grows. */
static long long diameter2(const Pt *h, int m, long *steps) {
    if (m == 2)
        return d2(h[0], h[1]);
    long long best = 0;
    int j = 1;
    for (int i = 0; i < m; i++) {
        int ni = (i + 1) % m;
        while (cross(h[i], h[ni], h[(j + 1) % m]) > cross(h[i], h[ni], h[j])) {
            j = (j + 1) % m;
            (*steps)++;
        }
        long long a = d2(h[i], h[j]), b = d2(h[ni], h[j]);
        if (a > best)
            best = a;
        if (b > best)
            best = b;
    }
    return best;
}

/* Minimum width squared as a fraction num/den: min over edges of (max cross)^2 / |edge|^2. */
static void width2(const Pt *h, int m, long long *num, long long *den) {
    long long bn = -1, bd = 1;
    int j = 1;
    for (int i = 0; i < m; i++) {
        int ni = (i + 1) % m;
        while (cross(h[i], h[ni], h[(j + 1) % m]) > cross(h[i], h[ni], h[j]))
            j = (j + 1) % m;
        long long c = cross(h[i], h[ni], h[j]);
        long long n2 = c * c, d = d2(h[i], h[ni]);
        if (bn < 0 || n2 * bd < bn * d) {
            bn = n2;
            bd = d;
        }
    }
    *num = bn;
    *den = bd;
}

static long long gcdll(long long a, long long b) {
    while (b) {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

int main(void) {
    int cfgs[][2] = {{8, 20}, {25, 100}, {60, 600}, {200, 50}, {300, 400}};
    for (int t = 0; t < 5; t++) {
        int n = cfgs[t][0], r = cfgs[t][1];
        Pt *p = malloc(sizeof(Pt) * (size_t)n), *all = malloc(sizeof(Pt) * (size_t)n);
        Pt *h = malloc(sizeof(Pt) * (size_t)(2 * n + 2));
        for (int i = 0; i < n; i++) {
            p[i] = (Pt){(long long)(rnd() % (unsigned)r), (long long)(rnd() % (unsigned)r)};
            all[i] = p[i];
        }
        int m = hull(p, n, h);
        check(m >= 3, "non-degenerate hull");
        long steps = 0;
        long long dia = diameter2(h, m, &steps);
        long long brute = 0;
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                long long d = d2(all[i], all[j]);
                if (d > brute)
                    brute = d;
            }
        check(dia == brute, "calipers diameter equals brute force");
        long long wn, wd;
        width2(h, m, &wn, &wd);
        /* brute width: for every edge direction take the farthest point of the full set */
        long long bn = -1, bd = 1;
        for (int i = 0; i < m; i++) {
            Pt a = h[i], b = h[(i + 1) % m];
            long long far = 0;
            for (int k = 0; k < n; k++) {
                long long c = cross(a, b, all[k]);
                if (c < 0)
                    c = -c;
                if (c > far)
                    far = c;
            }
            long long n2 = far * far, d = d2(a, b);
            if (bn < 0 || n2 * bd < bn * d) {
                bn = n2;
                bd = d;
            }
        }
        check(wn * bd == bn * wd, "calipers width equals brute force");
        long long g = gcdll(wn, wd);
        printf("n=%3d hull=%3d diameter^2=%9lld pointer_steps=%3ld width^2=%lld/%lld\n", n, m, dia, steps,
               wn / g, wd / g);
        check(steps <= 2L * m + 2, "pointer moves at most about two laps");
        free(p);
        free(all);
        free(h);
    }
    /* rectangle: diameter is the diagonal, width the short side */
    Pt rect[] = {{0, 0}, {8, 0}, {8, 3}, {0, 3}, {4, 1}};
    Pt h[12];
    int m = hull(rect, 5, h);
    long steps = 0;
    long long wn, wd;
    width2(h, m, &wn, &wd);
    printf("rectangle: diameter^2=%lld width^2=%lld/%lld\n", diameter2(h, m, &steps), wn / gcdll(wn, wd),
           wd / gcdll(wn, wd));
    check(diameter2(h, m, &steps) == 73, "rectangle diagonal");
    return 0;
}
