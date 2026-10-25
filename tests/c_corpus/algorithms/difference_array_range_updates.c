/*
 * title: Difference array for batched range updates
 * topic: algorithms
 * covers: difference arrays, lazy range add, prefix reconstruction, 2D difference grid, flight-booking style counting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 271828u;
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

enum { N = 500, R = 40, C = 50 };

int main(void) {
    static long diff[N + 1], naive[N];
    int ops = 2000;
    for (int i = 0; i < ops; i++) {
        int l = (int)(rnd() % N), r = (int)(rnd() % N);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        long v = (long)(rnd() % 21) - 10;
        diff[l] += v;
        diff[r + 1] -= v;
        for (int j = l; j <= r; j++)
            naive[j] += v;
    }
    long run = 0, maxv = -1000000, minv = 1000000;
    int argmax = 0;
    for (int i = 0; i < N; i++) {
        run += diff[i];
        if (run != naive[i])
            fail("1D reconstruct");
        if (run > maxv) {
            maxv = run;
            argmax = i;
        }
        if (run < minv)
            minv = run;
    }
    printf("1D: max %ld at %d, min %ld\n", maxv, argmax, minv);

    /* bookings: how many reservations cover each day, days with no coverage */
    static int cover[365 + 2];
    for (int i = 0; i < 300; i++) {
        int s = 1 + (int)(rnd() % 365), len = 1 + (int)(rnd() % 20);
        int e = s + len - 1 > 365 ? 365 : s + len - 1;
        cover[s]++;
        cover[e + 1]--;
    }
    int cur = 0, empty = 0, peak = 0, peak_day = 0;
    for (int d = 1; d <= 365; d++) {
        cur += cover[d];
        if (cur == 0)
            empty++;
        if (cur > peak) {
            peak = cur;
            peak_day = d;
        }
    }
    printf("bookings: peak %d on day %d, empty days %d\n", peak, peak_day, empty);

    /* 2D: add v to rectangle */
    static long d2[R + 1][C + 1], n2[R][C];
    for (int i = 0; i < 300; i++) {
        int r0 = (int)(rnd() % R), r1 = (int)(rnd() % R), c0 = (int)(rnd() % C), c1 = (int)(rnd() % C);
        if (r0 > r1) {
            int t = r0;
            r0 = r1;
            r1 = t;
        }
        if (c0 > c1) {
            int t = c0;
            c0 = c1;
            c1 = t;
        }
        long v = (long)(rnd() % 9) - 4;
        d2[r0][c0] += v;
        d2[r0][c1 + 1] -= v;
        d2[r1 + 1][c0] -= v;
        d2[r1 + 1][c1 + 1] += v;
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++)
                n2[r][c] += v;
    }
    static long acc[R + 1][C + 1];
    long total = 0;
    for (int r = 0; r < R; r++)
        for (int c = 0; c < C; c++) {
            acc[r][c] = d2[r][c] + (r ? acc[r - 1][c] : 0) + (c ? acc[r][c - 1] : 0) -
                        (r && c ? acc[r - 1][c - 1] : 0);
            if (acc[r][c] != n2[r][c])
                fail("2D reconstruct");
            total += acc[r][c];
        }
    printf("2D: grid total %ld, corner values %ld %ld %ld %ld\n", total, acc[0][0], acc[0][C - 1],
           acc[R - 1][0], acc[R - 1][C - 1]);
    return 0;
}
