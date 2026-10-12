/*
 * title: Interval DP: burst balloons and remove boxes style scoring
 * topic: algorithms
 * covers: dynamic programming, interval DP, last-action decomposition, sentinel padding, memoized recursion cross-check, exhaustive order brute force
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

#define N 12
static int a[N + 2];
static int memo[N + 2][N + 2];

static int rec(int l, int r) { /* open interval (l, r) */
    if (r - l < 2) return 0;
    if (memo[l][r] >= 0) return memo[l][r];
    int best = 0;
    for (int k = l + 1; k < r; k++) {
        int v = rec(l, k) + rec(k, r) + a[l] * a[k] * a[r];
        if (v > best) best = v;
    }
    return memo[l][r] = best;
}

static int brute(int *v, int n) {
    if (n == 0) return 0;
    int best = 0;
    for (int i = 0; i < n; i++) {
        int left = i > 0 ? v[i - 1] : 1, right = i + 1 < n ? v[i + 1] : 1;
        int gain = left * v[i] * right;
        int w[N];
        int m = 0;
        for (int j = 0; j < n; j++) if (j != i) w[m++] = v[j];
        int x = gain + brute(w, m);
        if (x > best) best = x;
    }
    return best;
}

int main(void) {
    int tests[3][6] = {{3, 1, 5, 8}, {1, 5}, {9, 76, 64, 21}};
    int tn[3] = {4, 2, 4};
    int expect[3] = {167, 10, 116718};
    for (int t = 0; t < 3; t++) {
        int n = tn[t];
        a[0] = a[n + 1] = 1;
        for (int i = 0; i < n; i++) a[i + 1] = tests[t][i];
        memset(memo, -1, sizeof memo);
        int r = rec(0, n + 1);
        CHECK(r == expect[t]);
        printf("known %d: %d\n", t, r);
    }
    for (int t = 0; t < 8; t++) {
        int n = 3 + rr(6), v[N];
        a[0] = a[n + 1] = 1;
        for (int i = 0; i < n; i++) { v[i] = 1 + rr(9); a[i + 1] = v[i]; }
        memset(memo, -1, sizeof memo);
        /* bottom-up */
        int d[N + 2][N + 2];
        memset(d, 0, sizeof d);
        for (int len = 2; len <= n + 1; len++)
            for (int l = 0; l + len <= n + 1; l++) {
                int r = l + len;
                for (int k = l + 1; k < r; k++) {
                    int x = d[l][k] + d[k][r] + a[l] * a[k] * a[r];
                    if (x > d[l][r]) d[l][r] = x;
                }
            }
        int top = d[0][n + 1];
        CHECK(top == rec(0, n + 1));
        CHECK(top == brute(v, n));
        printf("random %d: n=%d best=%d\n", t, n, top);
    }
    return 0;
}
