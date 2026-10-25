/*
 * title: Closest pair of points by divide and conquer
 * topic: algorithms
 * covers: divide and conquer, strip merge, sort by y merge, squared distances, duplicates, brute force check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    long long x, y;
} Pt;

static unsigned long long rs = 0x123456789ABCDEFULL;

static unsigned rnd(void) {
    rs ^= rs >> 12;
    rs ^= rs << 25;
    rs ^= rs >> 27;
    return (unsigned)((rs * 0x2545F4914F6CDD1DULL) >> 33);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static long long d2(Pt a, Pt b) {
    return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y);
}

static int cmp_x(const void *pa, const void *pb) {
    const Pt *a = pa, *b = pb;
    if (a->x != b->x)
        return a->x < b->x ? -1 : 1;
    if (a->y != b->y)
        return a->y < b->y ? -1 : 1;
    return 0;
}

static long strip_checks;

/* p sorted by x on entry; on exit p[lo..hi) is sorted by y. tmp is scratch space. */
static long long rec(Pt *p, Pt *tmp, int lo, int hi, Pt *ba, Pt *bb) {
    if (hi - lo <= 3) {
        long long best = -1;
        for (int i = lo; i < hi; i++)
            for (int j = i + 1; j < hi; j++) {
                long long d = d2(p[i], p[j]);
                if (best < 0 || d < best) {
                    best = d;
                    *ba = p[i];
                    *bb = p[j];
                }
            }
        for (int i = lo + 1; i < hi; i++) { /* insertion sort by y */
            Pt t = p[i];
            int j = i - 1;
            while (j >= lo && p[j].y > t.y) {
                p[j + 1] = p[j];
                j--;
            }
            p[j + 1] = t;
        }
        return best;
    }
    int mid = lo + (hi - lo) / 2;
    long long midx = p[mid].x;
    Pt la, lb, ra, rb;
    long long dl = rec(p, tmp, lo, mid, &la, &lb);
    long long dr = rec(p, tmp, mid, hi, &ra, &rb);
    long long best;
    if (dl <= dr) {
        best = dl;
        *ba = la;
        *bb = lb;
    } else {
        best = dr;
        *ba = ra;
        *bb = rb;
    }
    /* merge by y */
    int i = lo, j = mid, k = lo;
    while (i < mid && j < hi)
        tmp[k++] = (p[i].y <= p[j].y) ? p[i++] : p[j++];
    while (i < mid)
        tmp[k++] = p[i++];
    while (j < hi)
        tmp[k++] = p[j++];
    memcpy(p + lo, tmp + lo, sizeof(Pt) * (size_t)(hi - lo));
    /* strip: points with (x - midx)^2 < best, compare with next few in y order */
    int m = 0;
    for (int t = lo; t < hi; t++) {
        long long dx = p[t].x - midx;
        if (dx * dx < best)
            tmp[m++] = p[t];
    }
    for (int a = 0; a < m; a++)
        for (int b = a + 1; b < m; b++) {
            long long dy = tmp[b].y - tmp[a].y;
            if (dy * dy >= best)
                break;
            strip_checks++;
            long long d = d2(tmp[a], tmp[b]);
            if (d < best) {
                best = d;
                *ba = tmp[a];
                *bb = tmp[b];
            }
        }
    return best;
}

int main(void) {
    int sizes[] = {2, 5, 17, 100, 1000, 4000};
    int spans[] = {10, 30, 1000, 500, 100000, 2000};
    for (int t = 0; t < 6; t++) {
        int n = sizes[t];
        Pt *p = malloc(sizeof(Pt) * (size_t)n), *tmp = malloc(sizeof(Pt) * (size_t)n);
        Pt *orig = malloc(sizeof(Pt) * (size_t)n);
        for (int i = 0; i < n; i++) {
            p[i].x = (long long)(rnd() % (unsigned)spans[t]);
            p[i].y = (long long)(rnd() % (unsigned)spans[t]);
            orig[i] = p[i];
        }
        qsort(p, (size_t)n, sizeof(Pt), cmp_x);
        Pt a, b;
        strip_checks = 0;
        long long best = rec(p, tmp, 0, n, &a, &b);
        long long brute = -1;
        if (n <= 1000) {
            for (int i = 0; i < n; i++)
                for (int j = i + 1; j < n; j++) {
                    long long d = d2(orig[i], orig[j]);
                    if (brute < 0 || d < brute)
                        brute = d;
                }
            check(brute == best, "matches brute force");
        }
        check(d2(a, b) == best, "reported pair realises the distance");
        printf("n=%4d span=%6d min_dist_sq=%lld strip_checks=%ld%s\n", n, spans[t], best, strip_checks,
               best == 0 ? " (duplicate points)" : "");
        free(p);
        free(tmp);
        free(orig);
    }
    /* grid with an obvious answer: points 10 apart plus one pair at distance sqrt(2) */
    Pt g[26];
    int n = 0;
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++)
            g[n++] = (Pt){i * 10, j * 10};
    g[n++] = (Pt){21, 21};
    Pt tmp[26], a, b;
    qsort(g, (size_t)n, sizeof(Pt), cmp_x);
    long long best = rec(g, tmp, 0, n, &a, &b);
    printf("grid: %lld between (%lld,%lld) and (%lld,%lld)\n", best, a.x < b.x ? a.x : b.x,
           a.x < b.x ? a.y : b.y, a.x < b.x ? b.x : a.x, a.x < b.x ? b.y : a.y);
    check(best == 2, "grid answer");
    return 0;
}
