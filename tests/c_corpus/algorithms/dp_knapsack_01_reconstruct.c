/*
 * title: 0/1 knapsack with item reconstruction
 * topic: algorithms
 * covers: dynamic programming, 0/1 knapsack, full table traceback, 1-D rolling array, brute force cross-check
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

#define MAXN 20
#define MAXW 200

static int wt[MAXN], val[MAXN];

static int solve_table(int n, int cap, int *chosen) {
    static int t[MAXN + 1][MAXW + 1];
    memset(t, 0, sizeof t);
    for (int i = 1; i <= n; i++)
        for (int c = 0; c <= cap; c++) {
            t[i][c] = t[i - 1][c];
            if (c >= wt[i - 1] && t[i - 1][c - wt[i - 1]] + val[i - 1] > t[i][c])
                t[i][c] = t[i - 1][c - wt[i - 1]] + val[i - 1];
        }
    int c = cap;
    for (int i = n; i >= 1; i--) {
        chosen[i - 1] = 0;
        if (t[i][c] != t[i - 1][c]) {
            chosen[i - 1] = 1;
            c -= wt[i - 1];
        }
    }
    return t[n][cap];
}

static int solve_rolling(int n, int cap) {
    int d[MAXW + 1];
    memset(d, 0, sizeof d);
    for (int i = 0; i < n; i++)
        for (int c = cap; c >= wt[i]; c--)
            if (d[c - wt[i]] + val[i] > d[c])
                d[c] = d[c - wt[i]] + val[i];
    return d[cap];
}

static int brute(int n, int cap) {
    int best = 0;
    for (unsigned m = 0; m < (1u << n); m++) {
        int w = 0, v = 0;
        for (int i = 0; i < n; i++)
            if (m >> i & 1) { w += wt[i]; v += val[i]; }
        if (w <= cap && v > best) best = v;
    }
    return best;
}

int main(void) {
    for (int trial = 0; trial < 6; trial++) {
        int n = 8 + trial * 2, cap = 30 + rr(150);
        for (int i = 0; i < n; i++) { wt[i] = 1 + rr(40); val[i] = 1 + rr(90); }
        int chosen[MAXN];
        int best = solve_table(n, cap, chosen);
        int w = 0, v = 0;
        for (int i = 0; i < n; i++)
            if (chosen[i]) { w += wt[i]; v += val[i]; }
        CHECK(w <= cap);
        CHECK(v == best);
        CHECK(solve_rolling(n, cap) == best);
        CHECK(brute(n, cap) == best);
        printf("trial %d: n=%d cap=%d best=%d weight=%d items=", trial, n, cap, best, w);
        for (int i = 0; i < n; i++)
            if (chosen[i]) printf("%d,", i);
        printf("\n");
    }
    /* classic textbook instance */
    wt[0] = 1; wt[1] = 3; wt[2] = 4; wt[3] = 5;
    val[0] = 1; val[1] = 4; val[2] = 5; val[3] = 7;
    int ch[MAXN];
    int b = solve_table(4, 7, ch);
    CHECK(b == 9);
    printf("textbook: %d\n", b);
    return 0;
}
