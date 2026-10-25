/*
 * title: Egg dropping puzzle: three DP formulations
 * topic: algorithms
 * covers: dynamic programming, egg drop, min trials with binary search on split, moves-based DP (floors covered), closed form binomial sum, worst-case game tree
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 88172645463325252ULL;
static unsigned long long rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static int rr(int n) { return (int)(rnd() % (unsigned long long)n); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); exit(1); } } while (0)

typedef unsigned long long u64;

/* O(k n^2): dp[e][f] = 1 + min over x of max(dp[e-1][x-1], dp[e][f-x]) */
static int classic(int eggs, int floors) {
    static int d[12][300];
    for (int f = 0; f <= floors; f++) d[1][f] = f;
    for (int e = 2; e <= eggs; e++) {
        d[e][0] = 0;
        for (int f = 1; f <= floors; f++) {
            int best = 1 << 28;
            for (int x = 1; x <= f; x++) {
                int a = d[e - 1][x - 1], b = d[e][f - x];
                int worst = 1 + (a > b ? a : b);
                if (worst < best) best = worst;
            }
            d[e][f] = best;
        }
    }
    return d[eggs][floors];
}

/* binary search on the split since dp[e-1][x-1] rises and dp[e][f-x] falls with x */
static int bsearch_version(int eggs, int floors) {
    static int d[12][300];
    for (int f = 0; f <= floors; f++) d[1][f] = f;
    for (int e = 2; e <= eggs; e++) {
        d[e][0] = 0;
        for (int f = 1; f <= floors; f++) {
            int lo = 1, hi = f;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                if (d[e - 1][mid - 1] < d[e][f - mid]) lo = mid + 1; else hi = mid;
            }
            int best = 1 << 28;
            for (int x = lo - 1; x <= lo; x++) {
                if (x < 1 || x > f) continue;
                int a = d[e - 1][x - 1], b = d[e][f - x];
                int worst = 1 + (a > b ? a : b);
                if (worst < best) best = worst;
            }
            d[e][f] = best;
        }
    }
    return d[eggs][floors];
}

/* moves-based: f(m, e) = f(m-1, e-1) + f(m-1, e) + 1 floors covered with m drops */
static int moves_version(int eggs, int floors) {
    u64 cover[12] = {0};
    int m = 0;
    while (cover[eggs] < (u64)floors) {
        m++;
        for (int e = eggs; e >= 1; e--) cover[e] = cover[e] + cover[e - 1] + 1;
    }
    return m;
}

/* closed form: covered(m, e) = sum_{i=1..e} C(m, i) */
static u64 covered(int m, int e) {
    u64 c = 1, s = 0;
    for (int i = 1; i <= e; i++) {
        c = c * (u64)(m - i + 1) / (u64)i;
        s += c;
        if (i > m) break;
    }
    return s;
}

int main(void) {
    (void)rr;
    printf("floors:      ");
    int fl[] = {1, 2, 6, 10, 36, 100, 250};
    for (int i = 0; i < 7; i++) printf("%4d", fl[i]);
    printf("\n");
    for (int e = 1; e <= 6; e++) {
        printf("eggs=%d      ", e);
        for (int i = 0; i < 7; i++) {
            int a = e <= 1 ? fl[i] : classic(e, fl[i]);
            int b = e <= 1 ? fl[i] : bsearch_version(e, fl[i]);
            int c = e == 1 ? fl[i] : moves_version(e, fl[i]);
            CHECK(a == b && b == c);
            if (e > 1) {
                CHECK(covered(c, e) >= (u64)fl[i]);
                CHECK(covered(c - 1, e) < (u64)fl[i]);
            }
            printf("%4d", a);
        }
        printf("\n");
    }
    printf("100 floors, 2 eggs: %d drops; 10^9 floors, 2 eggs: %d; 10^9 floors, 11 eggs: %d\n", classic(2, 100),
           moves_version(2, 1000000000), moves_version(11, 1000000000));
    return 0;
}
