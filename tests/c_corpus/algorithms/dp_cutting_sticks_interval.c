/*
 * title: Interval DP: cutting a stick at marked points
 * topic: algorithms
 * covers: dynamic programming, interval DP, sentinel endpoints, last-cut decomposition, Knuth style split bounds, cut order reconstruction, permutation brute force
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

static int pos[N + 2];

static int permute(int *cuts, int n, int l, int r) {
    /* brute force over every order of cuts: recursive on which cut is first */
    (void)cuts;
    int best = 1 << 28;
    int any = 0;
    for (int i = 0; i < n; i++) {
        if (pos[i] <= l || pos[i] >= r) continue;
        any = 1;
        /* cut at pos[i] first: the two sides are independent */
        int v = (r - l) + permute(cuts, n, l, pos[i]) + permute(cuts, n, pos[i], r);
        if (v < best) best = v;
    }
    return any ? best : 0;
}

static void order(int l, int r, int (*sp)[N + 2], int *out, int *cnt) {
    if (r - l < 2) return;
    int k = sp[l][r];
    out[(*cnt)++] = pos[k];
    order(l, k, sp, out, cnt);
    order(k, r, sp, out, cnt);
}

int main(void) {
    for (int t = 0; t < 8; t++) {
        int n = 2 + rr(N - 2), len = 100 + rr(400);
        int marks[N + 2];
        /* n distinct marks in (0, len) */
        int used[600] = {0}, k = 0;
        while (k < n) { int p = 1 + rr(len - 1); if (!used[p]) { used[p] = 1; k++; } }
        int cnt = 0;
        pos[0] = 0;
        for (int p = 1; p < len; p++) if (used[p]) pos[++cnt] = p;
        pos[n + 1] = len;
        (void)marks;
        int d[N + 2][N + 2], sp[N + 2][N + 2];
        memset(d, 0, sizeof d);
        for (int span = 2; span <= n + 1; span++)
            for (int l = 0; l + span <= n + 1; l++) {
                int r = l + span;
                d[l][r] = 1 << 28;
                for (int m = l + 1; m < r; m++) {
                    int v = d[l][m] + d[m][r] + pos[r] - pos[l];
                    if (v < d[l][r]) { d[l][r] = v; sp[l][r] = m; }
                }
            }
        int cuts_arr[N];
        int best = d[0][n + 1];
        CHECK(best == permute(cuts_arr, n + 2, 0, len));
        int ord[N], oc = 0;
        order(0, n + 1, sp, ord, &oc);
        CHECK(oc == n);
        /* simulate that cut order to validate the cost */
        int segs_l[N + 2], segs_r[N + 2], ns = 1, total = 0;
        segs_l[0] = 0; segs_r[0] = len;
        for (int c = 0; c < oc; c++)
            for (int s = 0; s < ns; s++)
                if (ord[c] > segs_l[s] && ord[c] < segs_r[s]) {
                    total += segs_r[s] - segs_l[s];
                    segs_l[ns] = ord[c]; segs_r[ns] = segs_r[s]; segs_r[s] = ord[c]; ns++;
                    break;
                }
        CHECK(total == best);
        printf("case %d: length=%d cuts=%d cost=%d first_cut=%d\n", t, len, n, best, ord[0]);
    }
    return 0;
}
