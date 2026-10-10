/*
 * title: Summed-area table rectangle queries
 * topic: algorithms
 * covers: 2D prefix sums, inclusion-exclusion, integral image, best fixed-size window, largest-sum square scan
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 65537u;
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

enum { H = 48, W = 64 };
static int img[H][W];
static long sat[H + 1][W + 1];

/* sum of rows [r0,r1) and columns [c0,c1) */
static long rect(int r0, int c0, int r1, int c1) {
    return sat[r1][c1] - sat[r0][c1] - sat[r1][c0] + sat[r0][c0];
}

int main(void) {
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++)
            img[r][c] = (int)(rnd() % 21) - 8;
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++)
            sat[r + 1][c + 1] = img[r][c] + sat[r][c + 1] + sat[r + 1][c] - sat[r][c];

    long chk = 0;
    for (int q = 0; q < 3000; q++) {
        int r0 = (int)(rnd() % H), r1 = (int)(rnd() % H) + 1;
        int c0 = (int)(rnd() % W), c1 = (int)(rnd() % W) + 1;
        if (r0 >= r1 || c0 >= c1) {
            int t;
            if (r0 >= r1) {
                t = r0;
                r0 = r1 - 1;
                r1 = t + 1;
            }
            if (c0 >= c1) {
                t = c0;
                c0 = c1 - 1;
                c1 = t + 1;
            }
        }
        long brute = 0;
        for (int r = r0; r < r1; r++)
            for (int c = c0; c < c1; c++)
                brute += img[r][c];
        if (rect(r0, c0, r1, c1) != brute)
            fail("rectangle sum");
        chk += brute;
    }
    printf("checksum of 3000 rectangle sums: %ld\n", chk);

    /* best k x k window for several k */
    int ks[] = {1, 2, 3, 5, 8, 16};
    for (int t = 0; t < 6; t++) {
        int k = ks[t];
        long best = -1000000, worst = 1000000;
        int br = 0, bc = 0;
        for (int r = 0; r + k <= H; r++)
            for (int c = 0; c + k <= W; c++) {
                long s = rect(r, c, r + k, c + k);
                if (s > best) {
                    best = s;
                    br = r;
                    bc = c;
                }
                if (s < worst)
                    worst = s;
            }
        long chk2 = 0;
        for (int r = br; r < br + k; r++)
            for (int c = bc; c < bc + k; c++)
                chk2 += img[r][c];
        if (chk2 != best)
            fail("best window");
        printf("k=%-2d best=%-5ld at (%d,%d) worst=%ld\n", k, best, br, bc, worst);
    }

    /* count of rectangles anchored at the origin with positive sum */
    int pos = 0;
    for (int r = 1; r <= H; r++)
        for (int c = 1; c <= W; c++)
            if (sat[r][c] > 0)
                pos++;
    printf("origin-anchored positive rectangles: %d of %d\n", pos, H * W);
    return 0;
}
