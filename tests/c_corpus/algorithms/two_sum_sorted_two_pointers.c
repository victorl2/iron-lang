/*
 * title: Two-sum family with two pointers
 * topic: algorithms
 * covers: opposing two pointers, pair counting with duplicates, closest pair to target, sorted-array invariants
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>

static unsigned st = 5150u;
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

/* number of index pairs i<j with a[i]+a[j]==t */
static long count_pairs(const int *a, int n, int t) {
    long cnt = 0;
    int i = 0, j = n - 1;
    while (i < j) {
        int s = a[i] + a[j];
        if (s < t)
            i++;
        else if (s > t)
            j--;
        else if (a[i] == a[j]) {
            long m = j - i + 1;
            cnt += m * (m - 1) / 2;
            break;
        } else {
            int x = a[i], y = a[j];
            long ci = 0, cj = 0;
            while (a[i] == x) {
                ci++;
                i++;
            }
            while (a[j] == y) {
                cj++;
                j--;
            }
            cnt += ci * cj;
        }
    }
    return cnt;
}

/* pair with the sum closest to t; ties resolved toward the smaller sum */
static int closest_pair(const int *a, int n, int t, int *bi, int *bj) {
    int i = 0, j = n - 1, best = 0, have = 0;
    while (i < j) {
        int s = a[i] + a[j];
        int d = abs(s - t), bd = abs(best - t);
        if (!have || d < bd || (d == bd && s < best)) {
            best = s;
            *bi = i;
            *bj = j;
            have = 1;
        }
        if (s < t)
            i++;
        else if (s > t)
            j--;
        else
            break;
    }
    return best;
}

int main(void) {
    enum { N = 60 };
    int a[N];
    long total = 0;
    for (int trial = 0; trial < 200; trial++) {
        int n = 2 + (int)(rnd() % (N - 1));
        int range = 5 + (int)(rnd() % 40);
        for (int i = 0; i < n; i++)
            a[i] = (int)(rnd() % (unsigned)range) - range / 2;
        qsort(a, (size_t)n, sizeof a[0], cmp_int);
        for (int t = -range; t <= range; t++) {
            long want = 0;
            for (int i = 0; i < n; i++)
                for (int j = i + 1; j < n; j++)
                    if (a[i] + a[j] == t)
                        want++;
            long got = count_pairs(a, n, t);
            if (got != want)
                fail("count_pairs");
            total += got;
            int bi = -1, bj = -1;
            int cs = closest_pair(a, n, t, &bi, &bj);
            int bd = 1 << 30, bs = 0;
            for (int i = 0; i < n; i++)
                for (int j = i + 1; j < n; j++) {
                    int d = abs(a[i] + a[j] - t), s = a[i] + a[j];
                    if (d < bd || (d == bd && s < bs)) {
                        bd = d;
                        bs = s;
                    }
                }
            if (cs != bs || a[bi] + a[bj] != cs)
                fail("closest_pair");
        }
    }
    printf("total matching pairs over all trials: %ld\n", total);

    int demo[] = {-4, -1, -1, 0, 1, 2, 2, 2, 5, 9};
    for (int t = 0; t <= 4; t++)
        printf("pairs summing to %d: %ld\n", t, count_pairs(demo, 10, t));
    int bi, bj;
    int s = closest_pair(demo, 10, 7, &bi, &bj);
    printf("closest to 7: %d + %d = %d\n", demo[bi], demo[bj], s);
    return 0;
}
