/*
 * title: Weighted median by partition-based selection
 * topic: algorithms
 * covers: weighted selection, three-way partition around a pivot, cumulative weight test, L1 minimiser property, struct records
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 5555u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

typedef struct {
    long x, w;
} Pt;

/* lower weighted median: smallest x with cumulative weight >= total/2 (in half-units) */
static long wmedian(Pt *p, int n, long total) {
    int lo = 0, hi = n;
    long need = (total + 1) / 2; /* weight that must lie at or left of the answer */
    for (;;) {
        Pt pivot = p[lo + (int)(rnd() % (unsigned)(hi - lo))];
        /* three-way partition of [lo,hi): < pivot, == pivot, > pivot */
        int lt = lo, i = lo, gt = hi;
        while (i < gt) {
            if (p[i].x < pivot.x) {
                Pt t = p[lt];
                p[lt++] = p[i];
                p[i++] = t;
            } else if (p[i].x > pivot.x) {
                Pt t = p[--gt];
                p[gt] = p[i];
                p[i] = t;
            } else
                i++;
        }
        long wl = 0, we = 0;
        for (int j = lo; j < lt; j++)
            wl += p[j].w;
        for (int j = lt; j < gt; j++)
            we += p[j].w;
        if (need <= wl)
            hi = lt;
        else if (need <= wl + we)
            return pivot.x;
        else {
            need -= wl + we;
            lo = gt;
        }
    }
}

static long cost(const Pt *p, int n, long c) {
    long s = 0;
    for (int i = 0; i < n; i++)
        s += p[i].w * labs(p[i].x - c);
    return s;
}

int main(void) {
    Pt p[200], q[200];
    long sum = 0;
    for (int trial = 0; trial < 300; trial++) {
        int n = 1 + (int)(rnd() % 100);
        long total = 0;
        for (int i = 0; i < n; i++) {
            p[i].x = (long)(rnd() % 60) - 20;
            p[i].w = 1 + (long)(rnd() % 9);
            total += p[i].w;
            q[i] = p[i];
        }
        long m = wmedian(q, n, total);
        /* brute force: the x minimising cost, smallest on ties, must be a lower weighted median */
        long bx = p[0].x, bc = cost(p, n, p[0].x);
        for (int i = 1; i < n; i++) {
            long c = cost(p, n, p[i].x);
            if (c < bc || (c == bc && p[i].x < bx)) {
                bc = c;
                bx = p[i].x;
            }
        }
        if (cost(p, n, m) != bc)
            fail("cost not minimal");
        long left = 0, right = 0;
        for (int i = 0; i < n; i++) {
            if (p[i].x < m)
                left += p[i].w;
            if (p[i].x > m)
                right += p[i].w;
        }
        if (left * 2 > total || right * 2 > total)
            fail("balance");
        sum += m;
    }
    printf("300 random weighted sets verified, median checksum %ld\n", sum);

    Pt d[] = {{1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 100}};
    printf("heavy right: %ld\n", wmedian(d, 5, 104));
    Pt e[] = {{10, 5}, {20, 5}};
    printf("tie lower: %ld\n", wmedian(e, 2, 10));
    Pt f[] = {{7, 3}};
    printf("single: %ld\n", wmedian(f, 1, 3));
    /* facility location: warehouse on a road, customers with demand */
    Pt shops[] = {{0, 4}, {3, 2}, {8, 7}, {12, 1}, {20, 6}};
    long best = wmedian(shops, 5, 20);
    printf("warehouse at km %ld, total demand-distance %ld\n", best, cost(shops, 5, best));
    return 0;
}
