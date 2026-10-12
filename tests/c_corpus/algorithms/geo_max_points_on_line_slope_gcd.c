/*
 * title: Maximum collinear points via normalised slopes
 * topic: algorithms
 * covers: gcd normalisation, exact slopes, sorting direction pairs, duplicates, brute force cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    long long x, y;
} Pt;

typedef struct {
    long long dx, dy;
} Dir;

static unsigned s = 271828u;

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

static int cmp_dir(const void *pa, const void *pb) {
    const Dir *a = pa, *b = pb;
    if (a->dx != b->dx)
        return a->dx < b->dx ? -1 : 1;
    return (a->dy > b->dy) - (a->dy < b->dy);
}

/* For each anchor, direction vectors to all later distinct points are reduced by gcd
 * and sign-normalised (dx > 0, or dx == 0 and dy > 0). The largest run of equal
 * directions plus the anchor plus its duplicates is the best line through it. */
static int best_line(const Pt *p, int n, Dir *scratch, long long *bdx, long long *bdy) {
    int best = n > 0 ? 1 : 0;
    *bdx = *bdy = 0;
    for (int i = 0; i < n; i++) {
        int m = 0, dups = 1;
        for (int j = i + 1; j < n; j++) {
            long long dx = p[j].x - p[i].x, dy = p[j].y - p[i].y;
            if (dx == 0 && dy == 0) {
                dups++;
                continue;
            }
            long long g = gcdll(dx, dy);
            dx /= g;
            dy /= g;
            if (dx < 0 || (dx == 0 && dy < 0)) {
                dx = -dx;
                dy = -dy;
            }
            scratch[m++] = (Dir){dx, dy};
        }
        if (dups > best)
            best = dups; /* every point is a duplicate of the anchor */
        qsort(scratch, (size_t)m, sizeof(Dir), cmp_dir);
        for (int a = 0; a < m;) {
            int b = a;
            while (b < m && cmp_dir(&scratch[a], &scratch[b]) == 0)
                b++;
            if (dups + (b - a) > best) {
                best = dups + (b - a);
                *bdx = scratch[a].dx;
                *bdy = scratch[a].dy;
            }
            a = b;
        }
    }
    return best;
}

static int brute(const Pt *p, int n) {
    int best = n > 0 ? 1 : 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            if (p[i].x == p[j].x && p[i].y == p[j].y)
                continue;
            int c = 0;
            for (int k = 0; k < n; k++)
                if ((p[j].x - p[i].x) * (p[k].y - p[i].y) == (p[j].y - p[i].y) * (p[k].x - p[i].x))
                    c++;
            if (c > best)
                best = c;
        }
    /* all points identical also counts */
    for (int i = 0; i < n; i++) {
        int c = 0;
        for (int k = 0; k < n; k++)
            c += p[k].x == p[i].x && p[k].y == p[i].y;
        if (c > best)
            best = c;
    }
    return best;
}

int main(void) {
    Pt fixed[] = {{1, 1}, {3, 2}, {5, 3}, {4, 1}, {2, 3}, {1, 4}};
    Dir scratch[512];
    long long dx, dy;
    int r = best_line(fixed, 6, scratch, &dx, &dy);
    printf("classic: best=%d direction=(%lld,%lld)\n", r, dx, dy);
    check(r == 4 && r == brute(fixed, 6), "classic");
    Pt vert[] = {{2, 0}, {2, 5}, {2, -3}, {3, 3}, {2, 9}};
    r = best_line(vert, 5, scratch, &dx, &dy);
    printf("vertical: best=%d direction=(%lld,%lld)\n", r, dx, dy);
    check(r == 4, "vertical line");
    Pt same[] = {{4, 4}, {4, 4}, {4, 4}, {5, 6}};
    r = best_line(same, 4, scratch, &dx, &dy);
    printf("duplicates: best=%d\n", r);
    check(r == 4, "duplicates count on any line through them");
    int total = 0;
    int cfg[][2] = {{10, 4}, {30, 6}, {60, 10}, {100, 20}, {150, 40}, {200, 100}};
    for (int t = 0; t < 6; t++) {
        int n = cfg[t][0], span = cfg[t][1];
        Pt *p = malloc(sizeof(Pt) * (size_t)n);
        Dir *sc = malloc(sizeof(Dir) * (size_t)n);
        for (int i = 0; i < n; i++)
            p[i] = (Pt){(long long)(rnd() % (unsigned)span) - span / 2, (long long)(rnd() % (unsigned)span) - span / 2};
        r = best_line(p, n, sc, &dx, &dy);
        check(r == brute(p, n), "matches O(n^3) brute force");
        printf("n=%3d span=%3d best=%2d direction=(%lld,%lld)\n", n, span, r, dx, dy);
        total += r;
        free(p);
        free(sc);
    }
    printf("sum of best=%d\n", total);
    return 0;
}
