/*
 * title: Gift wrapping (Jarvis march) with step counts
 * topic: algorithms
 * covers: jarvis march, output-sensitive hull, orientation tests, tie-breaking, worst-case inputs
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    long long x, y;
} Pt;

static unsigned long long st = 0x9E3779B97F4A7C15ULL;

static unsigned rnd(void) {
    st += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = st;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (unsigned)((z ^ (z >> 31)) >> 8);
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

/* Wraps counter-clockwise from the leftmost-lowest point. Returns hull size and
 * counts the number of orientation tests in *tests. */
static int jarvis(const Pt *p, int n, int *out, long *tests) {
    int start = 0;
    for (int i = 1; i < n; i++)
        if (p[i].x < p[start].x || (p[i].x == p[start].x && p[i].y < p[start].y))
            start = i;
    int h = 0, cur = start;
    do {
        out[h++] = cur;
        int nxt = (cur + 1) % n;
        for (int i = 0; i < n; i++) {
            if (i == cur)
                continue;
            (*tests)++;
            long long c = cross(p[cur], p[nxt], p[i]);
            if (nxt == cur || c < 0 || (c == 0 && dist2(p[cur], p[i]) > dist2(p[cur], p[nxt])))
                nxt = i;
        }
        cur = nxt;
        check(h <= n, "wrapping terminates");
    } while (cur != start);
    return h;
}

static void gen_circle(Pt *p, int n) {
    /* points on the parabola y = x*x: every point is a hull vertex */
    for (int i = 0; i < n; i++) {
        p[i].x = i - n / 2;
        p[i].y = (long long)(i - n / 2) * (i - n / 2);
    }
}

static void gen_blob(Pt *p, int n) {
    for (int i = 0; i < n; i++) {
        p[i].x = (long long)(rnd() % 61) - 30;
        p[i].y = (long long)(rnd() % 61) - 30;
    }
}

static void gen_square_plus_interior(Pt *p, int n) {
    p[0] = (Pt){0, 0};
    p[1] = (Pt){100, 0};
    p[2] = (Pt){100, 100};
    p[3] = (Pt){0, 100};
    for (int i = 4; i < n; i++) {
        p[i].x = 1 + (long long)(rnd() % 98);
        p[i].y = 1 + (long long)(rnd() % 98);
    }
}

/* strict vertices by definition: a point is a vertex iff it is not in the closed
 * convex hull of the others, tested via the "exists a supporting line" O(n^3) rule */
static int is_extreme(const Pt *p, int n, int i) {
    for (int j = 0; j < n; j++) {
        if (j == i || (p[j].x == p[i].x && p[j].y == p[i].y))
            continue;
        int ok = 1, left = 0;
        for (int k = 0; k < n && ok; k++) {
            long long c = cross(p[i], p[j], p[k]);
            if (c > 0)
                left = 1;
            else if (c < 0)
                ok = 0;
            else if ((p[k].x - p[i].x) * (p[j].x - p[i].x) + (p[k].y - p[i].y) * (p[j].y - p[i].y) < 0)
                ok = 0;
        }
        if (ok && left)
            return 1;
    }
    return 0;
}

int main(void) {
    const char *names[] = {"parabola", "blob", "square+interior", "blob-large"};
    int ns[] = {40, 50, 80, 300};
    for (int t = 0; t < 4; t++) {
        int n = ns[t];
        Pt *p = malloc(sizeof(Pt) * (size_t)n);
        int *out = malloc(sizeof(int) * (size_t)(n + 1));
        if (t == 0)
            gen_circle(p, n);
        else if (t == 2)
            gen_square_plus_interior(p, n);
        else
            gen_blob(p, n);
        long tests = 0;
        int h = jarvis(p, n, out, &tests);
        int extreme = 0;
        for (int i = 0; i < n; i++)
            extreme += is_extreme(p, n, i);
        /* duplicates of a corner can make the count differ, so compare distinct locations */
        int distinct_hull = 0;
        for (int i = 0; i < h; i++) {
            int dup = 0;
            for (int j = 0; j < i; j++)
                if (p[out[i]].x == p[out[j]].x && p[out[i]].y == p[out[j]].y)
                    dup = 1;
            distinct_hull += !dup;
        }
        int distinct_extreme = 0;
        for (int i = 0; i < n; i++) {
            int dup = 0;
            for (int j = 0; j < i; j++)
                if (p[i].x == p[j].x && p[i].y == p[j].y)
                    dup = 1;
            if (!dup)
                distinct_extreme += is_extreme(p, n, i);
        }
        check(distinct_hull == distinct_extreme, "hull vertices equal extreme points");
        long long a2 = 0;
        for (int i = 0; i < h; i++) {
            Pt a = p[out[i]], b = p[out[(i + 1) % h]];
            a2 += a.x * b.y - a.y * b.x;
        }
        check(a2 > 0, "counter-clockwise");
        printf("%-16s n=%3d h=%3d tests=%6ld (h*n=%6d) extreme_raw=%d area2=%lld\n", names[t], n, h,
               tests, h * n, extreme, a2);
        check(tests == (long)h * (n - 1), "exactly h*(n-1) tests");
        free(p);
        free(out);
    }
    return 0;
}
