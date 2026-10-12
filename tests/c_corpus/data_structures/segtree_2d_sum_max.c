/*
 * title: Two-dimensional iterative segment tree
 * topic: data_structures
 * covers: segment tree of segment trees, non-square grid, point update, rectangle sum and max, brute force
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#define R 13
#define C 21

static long sm[2 * R][2 * C];
static long mxv[2 * R][2 * C];

static long max2(long a, long b) { return a > b ? a : b; }

static void set(int i, int j, long v) {
    int x = i + R;
    sm[x][j + C] = mxv[x][j + C] = v;
    for (int y = (j + C) >> 1; y >= 1; y >>= 1) {
        sm[x][y] = sm[x][2 * y] + sm[x][2 * y + 1];
        mxv[x][y] = max2(mxv[x][2 * y], mxv[x][2 * y + 1]);
    }
    for (x >>= 1; x >= 1; x >>= 1)
        for (int y = j + C; y >= 1; y >>= 1) {
            if (y >= C) {
                sm[x][y] = sm[2 * x][y] + sm[2 * x + 1][y];
                mxv[x][y] = max2(mxv[2 * x][y], mxv[2 * x + 1][y]);
            } else {
                sm[x][y] = sm[x][2 * y] + sm[x][2 * y + 1];
                mxv[x][y] = max2(mxv[x][2 * y], mxv[x][2 * y + 1]);
            }
        }
}

/* half-open row range [x1,x2), column range [y1,y2) */
static void query(int x1, int x2, int y1, int y2, long *s, long *m) {
    *s = 0;
    *m = -1000000000L;
    for (int x = x1 + R, xe = x2 + R; x < xe; x >>= 1, xe >>= 1) {
        int rows[2], nr = 0;
        if (x & 1)
            rows[nr++] = x++;
        if (xe & 1)
            rows[nr++] = --xe;
        /* the two guards above may consume the last node, the loop test catches it */
        for (int k = 0; k < nr; k++)
            for (int y = y1 + C, ye = y2 + C; y < ye; y >>= 1, ye >>= 1) {
                if (y & 1) {
                    *s += sm[rows[k]][y];
                    *m = max2(*m, mxv[rows[k]][y]);
                    y++;
                }
                if (ye & 1) {
                    --ye;
                    *s += sm[rows[k]][ye];
                    *m = max2(*m, mxv[rows[k]][ye]);
                }
            }
    }
}

int main(void) {
    long g[R][C];
    for (int i = 0; i < R; i++)
        for (int j = 0; j < C; j++)
            g[i][j] = (long)rnd(2000) - 1000;
    for (int i = 0; i < R; i++)
        for (int j = 0; j < C; j++)
            sm[R + i][C + j] = mxv[R + i][C + j] = g[i][j];
    for (int i = 0; i < R; i++)
        for (int j = C - 1; j >= 1; j--) {
            sm[R + i][j] = sm[R + i][2 * j] + sm[R + i][2 * j + 1];
            mxv[R + i][j] = max2(mxv[R + i][2 * j], mxv[R + i][2 * j + 1]);
        }
    for (int i = R - 1; i >= 1; i--)
        for (int j = 1; j < 2 * C; j++) {
            sm[i][j] = sm[2 * i][j] + sm[2 * i + 1][j];
            mxv[i][j] = max2(mxv[2 * i][j], mxv[2 * i + 1][j]);
        }
    long ssum = 0, msum = 0;
    int nq = 0, nu = 0;
    for (int step = 0; step < 6000; step++) {
        if (rnd(3) == 0) {
            int i = (int)rnd(R), j = (int)rnd(C);
            long v = (long)rnd(2000) - 1000;
            g[i][j] = v;
            set(i, j, v);
            nu++;
            continue;
        }
        int x1 = (int)rnd(R), x2 = (int)rnd(R + 1), y1 = (int)rnd(C), y2 = (int)rnd(C + 1);
        if (x1 >= x2 || y1 >= y2)
            continue;
        long ws = 0, wm = -1000000000L;
        for (int i = x1; i < x2; i++)
            for (int j = y1; j < y2; j++) {
                ws += g[i][j];
                wm = max2(wm, g[i][j]);
            }
        long gs, gm;
        query(x1, x2, y1, y2, &gs, &gm);
        check(gs == ws && gm == wm, "rectangle query");
        ssum += gs;
        msum += gm;
        nq++;
    }
    printf("updates=%d queries=%d\n", nu, nq);
    printf("sum digest=%ld max digest=%ld\n", ssum, msum);
    long s, m;
    query(0, R, 0, C, &s, &m);
    printf("whole grid sum=%ld max=%ld\n", s, m);
    return 0;
}
