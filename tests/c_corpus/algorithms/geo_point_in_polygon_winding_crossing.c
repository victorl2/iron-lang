/*
 * title: Point in polygon by crossing and winding numbers
 * topic: algorithms
 * covers: ray casting, winding number, boundary detection, even-odd vs nonzero rule, integer exactness
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    long long x, y;
} Pt;

typedef enum { OUTSIDE, INSIDE, BOUNDARY } Loc;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static long long cross3(Pt o, Pt a, Pt b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

static int on_segment(Pt a, Pt b, Pt p) {
    if (cross3(a, b, p) != 0)
        return 0;
    return (p.x >= (a.x < b.x ? a.x : b.x)) && (p.x <= (a.x > b.x ? a.x : b.x)) &&
           (p.y >= (a.y < b.y ? a.y : b.y)) && (p.y <= (a.y > b.y ? a.y : b.y));
}

/* Even-odd crossing number using the half-open rule (a.y <= p.y) != (b.y <= p.y). */
static Loc crossing(const Pt *v, int n, Pt p) {
    int in = 0;
    for (int i = 0; i < n; i++) {
        Pt a = v[i], b = v[(i + 1) % n];
        if (on_segment(a, b, p))
            return BOUNDARY;
        if ((a.y <= p.y) != (b.y <= p.y)) {
            /* is p strictly left of the edge as seen going upward? */
            long long c = cross3(a, b, p);
            if (b.y > a.y ? c > 0 : c < 0)
                in = !in;
        }
    }
    return in ? INSIDE : OUTSIDE;
}

/* Nonzero winding number, returned directly; boundary reported through *on. */
static int winding(const Pt *v, int n, Pt p, int *on) {
    int w = 0;
    *on = 0;
    for (int i = 0; i < n; i++) {
        Pt a = v[i], b = v[(i + 1) % n];
        if (on_segment(a, b, p)) {
            *on = 1;
            return 0;
        }
        if (a.y <= p.y) {
            if (b.y > p.y && cross3(a, b, p) > 0)
                w++;
        } else {
            if (b.y <= p.y && cross3(a, b, p) < 0)
                w--;
        }
    }
    return w;
}

static const char *loc_name(Loc l) {
    return l == INSIDE ? "in" : (l == OUTSIDE ? "out" : "on");
}

int main(void) {
    /* concave "U" shape, counter-clockwise */
    Pt u[] = {{0, 0}, {9, 0}, {9, 8}, {6, 8}, {6, 3}, {3, 3}, {3, 8}, {0, 8}};
    Pt probes[] = {{1, 1}, {4, 5}, {4, 2}, {6, 3}, {3, 8}, {0, 4}, {10, 4}, {7, 5}, {-1, 0}, {9, 8}, {5, 3}, {4, 8}};
    printf("U-shape probes:\n");
    for (int i = 0; i < 12; i++) {
        Loc c = crossing(u, 8, probes[i]);
        int on;
        int w = winding(u, 8, probes[i], &on);
        check((c == BOUNDARY) == on, "boundary agreement");
        if (c != BOUNDARY)
            check((c == INSIDE) == (w != 0), "crossing agrees with winding on simple polygon");
        printf("  (%2lld,%2lld) %-3s w=%d\n", probes[i].x, probes[i].y, loc_name(c), w);
    }
    /* full grid census for the U polygon */
    int cnt[3] = {0, 0, 0};
    for (int y = -2; y <= 10; y++)
        for (int x = -2; x <= 11; x++) {
            Pt p = {x, y};
            cnt[crossing(u, 8, p)]++;
        }
    printf("U census: out=%d in=%d boundary=%d\n", cnt[OUTSIDE], cnt[INSIDE], cnt[BOUNDARY]);
    /* area 2A = 2*(9*8 - 3*5) = 114; Pick: A = I + B/2 - 1 -> 2A = 2I + B - 2 */
    check(114 == 2 * cnt[INSIDE] + cnt[BOUNDARY] - 2, "Pick's theorem agrees with grid census");

    /* self-overlapping pentagram: even-odd and nonzero disagree in the middle */
    Pt star[] = {{0, 10}, {6, -8}, {-9, 3}, {9, 3}, {-6, -8}};
    int diff = 0, in_eo = 0, in_nz = 0;
    for (int y = -9; y <= 11; y++)
        for (int x = -10; x <= 10; x++) {
            Pt p = {x, y};
            Loc c = crossing(star, 5, p);
            int on;
            int w = winding(star, 5, p, &on);
            check((c == BOUNDARY) == on, "star boundary agreement");
            if (c == BOUNDARY)
                continue;
            in_eo += c == INSIDE;
            in_nz += w != 0;
            if ((c == INSIDE) != (w != 0))
                diff++;
        }
    printf("pentagram: even-odd=%d nonzero=%d disagree=%d\n", in_eo, in_nz, diff);
    Pt centre = {0, 1};
    int on;
    printf("pentagram centre: crossing=%s winding=%d\n", loc_name(crossing(star, 5, centre)),
           winding(star, 5, centre, &on));
    check(winding(star, 5, centre, &on) == -2, "centre winds twice (clockwise)");
    check(crossing(star, 5, centre) == OUTSIDE, "centre is outside by even-odd");

    /* clockwise polygon has negative winding inside */
    Pt cw[] = {{0, 0}, {0, 5}, {5, 5}, {5, 0}};
    printf("cw square: winding at (2,2) = %d\n", winding(cw, 4, (Pt){2, 2}, &on));
    check(winding(cw, 4, (Pt){2, 2}, &on) == -1, "cw winding is -1");
    /* horizontal edges and ray passing through vertices must not double count */
    Pt hz[] = {{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}};
    int ins = 0;
    for (int x = -1; x <= 5; x++)
        for (int y = -1; y <= 5; y++)
            ins += crossing(hz, 6, (Pt){x, y}) == INSIDE;
    printf("staircase interior lattice points: %d\n", ins);
    return 0;
}
