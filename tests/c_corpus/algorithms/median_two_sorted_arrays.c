/*
 * title: Median of two sorted arrays
 * topic: algorithms
 * covers: binary search on partition, O(log min(m,n)) median, sentinel infinities, kth element of union, even/odd totals
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 303030u;
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
static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

#define NEG_INF (-2000000000)
#define POS_INF 2000000000

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* returns twice the median (an integer) so that even totals stay exact */
static long median2(const int *a, int na, const int *b, int nb) {
    if (na > nb)
        return median2(b, nb, a, na);
    int lo = 0, hi = na, half = (na + nb + 1) / 2;
    while (lo <= hi) {
        int i = lo + (hi - lo) / 2, j = half - i;
        int aleft = i ? a[i - 1] : NEG_INF, aright = i < na ? a[i] : POS_INF;
        int bleft = j ? b[j - 1] : NEG_INF, bright = j < nb ? b[j] : POS_INF;
        if (aleft <= bright && bleft <= aright) {
            long lmax = imax(aleft, bleft), rmin = imin(aright, bright);
            return (na + nb) % 2 ? 2 * lmax : lmax + rmin;
        }
        if (aleft > bright)
            hi = i - 1;
        else
            lo = i + 1;
    }
    fail("no partition");
    return 0;
}

/* k-th (1-based) smallest of the union by discarding k/2 candidates each round */
static int kth(const int *a, int na, const int *b, int nb, int k) {
    if (na == 0)
        return b[k - 1];
    if (nb == 0)
        return a[k - 1];
    if (k == 1)
        return imin(a[0], b[0]);
    int h = k / 2;
    int ia = imin(h, na), ib = imin(h, nb);
    if (a[ia - 1] <= b[ib - 1])
        return kth(a + ia, na - ia, b, nb, k - ia);
    return kth(a, na, b + ib, nb - ib, k - ib);
}

int main(void) {
    static int a[80], b[80], m[160];
    for (int trial = 0; trial < 1000; trial++) {
        int na = (int)(rnd() % 60), nb = (int)(rnd() % 60);
        if (na + nb == 0)
            na = 1;
        int range = 1 + (int)(rnd() % 100);
        for (int i = 0; i < na; i++)
            a[i] = (int)(rnd() % (unsigned)range) - 30;
        for (int i = 0; i < nb; i++)
            b[i] = (int)(rnd() % (unsigned)range) - 30;
        qsort(a, (size_t)na, sizeof(int), cmp_int);
        qsort(b, (size_t)nb, sizeof(int), cmp_int);
        for (int i = 0; i < na; i++)
            m[i] = a[i];
        for (int i = 0; i < nb; i++)
            m[na + i] = b[i];
        int n = na + nb;
        qsort(m, (size_t)n, sizeof(int), cmp_int);
        long want = n % 2 ? 2L * m[n / 2] : (long)m[n / 2 - 1] + m[n / 2];
        if (median2(a, na, b, nb) != want)
            fail("median");
        int k = 1 + (int)(rnd() % (unsigned)n);
        if (kth(a, na, b, nb, k) != m[k - 1])
            fail("kth");
    }
    printf("1000 random pairs verified\n");

    int x1[] = {1, 3}, y1[] = {2};
    int x2[] = {1, 2}, y2[] = {3, 4};
    int x3[] = {0, 0}, y3[] = {0, 0};
    int x4[] = {2}, y4[] = {0};
    int x5[] = {1, 2, 3, 4, 5}, y5[] = {100, 200, 300, 400, 500, 600};
    struct {
        const char *name;
        int *a, na, *b, nb;
    } demos[] = {{"[1,3]+[2]", x1, 2, y1, 1}, {"[1,2]+[3,4]", x2, 2, y2, 2},
                 {"zeros", x3, 2, y3, 2}, {"[2]+[0]", x4, 1, y4, 1},
                 {"disjoint", x5, 5, y5, 6}, {"empty+[1,2,3]", x5, 0, x5, 3}};
    for (int i = 0; i < 6; i++) {
        long t = median2(demos[i].a, demos[i].na, demos[i].b, demos[i].nb);
        printf("%-14s median = %ld%s\n", demos[i].name, t / 2, t % 2 ? ".5" : "");
    }
    return 0;
}
