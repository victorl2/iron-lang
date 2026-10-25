/*
 * title: Divide and conquer DP optimization
 * topic: algorithms
 * covers: dynamic programming, divide and conquer optimization, monotone opt split, k-partition, prefix cost function, cubic DP cross-check, evaluation counts
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

typedef long long ll;
#define N 120
#define K 8

static ll pre[N + 1], pre2[N + 1];
static ll evals;

/* cost of group (l, r]: sum of squares of deviation from the mean, scaled by len: len*sum(x^2) - sum(x)^2 */
static ll cost(int l, int r) {
    evals++;
    ll len = r - l, s = pre[r] - pre[l], s2 = pre2[r] - pre2[l];
    return len * s2 - s * s;
}

static ll prev_row[N + 1], cur_row[N + 1];

static void solve(int lo, int hi, int optlo, int opthi) {
    if (lo > hi) return;
    int mid = (lo + hi) / 2, best = optlo;
    ll bv = -1;
    for (int j = optlo; j <= opthi && j < mid; j++) {
        if (prev_row[j] < 0) continue;
        ll v = prev_row[j] + cost(j, mid);
        if (bv < 0 || v < bv) { bv = v; best = j; }
    }
    cur_row[mid] = bv;
    solve(lo, mid - 1, optlo, best);
    solve(mid + 1, hi, best, opthi);
}

int main(void) {
    for (int t = 0; t < 5; t++) {
        int n = 30 + rr(N - 30), k = 2 + rr(K - 1);
        pre[0] = pre2[0] = 0;
        int x[N];
        for (int i = 0; i < n; i++) { x[i] = rr(50); }
        /* sorted values make the cost function satisfy the quadrangle inequality nicely; keep as-is either way */
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) if (x[j] < x[i]) { int tmp = x[i]; x[i] = x[j]; x[j] = tmp; }
        for (int i = 0; i < n; i++) { pre[i + 1] = pre[i] + x[i]; pre2[i + 1] = pre2[i] + (ll)x[i] * x[i]; }
        /* cubic reference */
        ll ref[K + 1][N + 1];
        for (int g = 0; g <= k; g++) for (int i = 0; i <= n; i++) ref[g][i] = -1;
        ref[0][0] = 0;
        ll cubic_evals = 0;
        for (int g = 1; g <= k; g++)
            for (int i = g; i <= n; i++)
                for (int j = g - 1; j < i; j++) {
                    if (ref[g - 1][j] < 0) continue;
                    cubic_evals++;
                    ll v = ref[g - 1][j] + (ll)(i - j) * (pre2[i] - pre2[j]) - (pre[i] - pre[j]) * (pre[i] - pre[j]);
                    if (ref[g][i] < 0 || v < ref[g][i]) ref[g][i] = v;
                }
        /* D&C */
        evals = 0;
        for (int i = 0; i <= n; i++) prev_row[i] = -1;
        prev_row[0] = 0;
        for (int g = 1; g <= k; g++) {
            for (int i = 0; i <= n; i++) cur_row[i] = -1;
            solve(g, n, g - 1, n - 1);
            for (int i = 0; i <= n; i++) prev_row[i] = cur_row[i];
        }
        CHECK(prev_row[n] == ref[k][n]);
        printf("case %d: n=%d k=%d cost=%lld dc_evals=%lld cubic_evals=%lld\n", t, n, k, prev_row[n], evals,
               cubic_evals);
    }
    return 0;
}
