/*
 * title: Aliens trick (Lagrangian relaxation) for exactly-k DP
 * topic: algorithms
 * covers: dynamic programming, lambda optimization, binary search on penalty, tie-breaking by count, convexity in k, exact-k DP cross-check
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
#define N 60

static int a[N];

/* choose exactly k disjoint non-adjacent-overlapping segments? Use: pick exactly k disjoint subarrays maximizing total sum. */
/* exact-k reference DP: f[i][j][s] with s = currently inside a segment */
static ll exact_k(int n, int k) {
    static ll f[N + 1][N + 1][2];
    const ll NEG = -(1LL << 50);
    for (int i = 0; i <= n; i++) for (int j = 0; j <= k; j++) f[i][j][0] = f[i][j][1] = NEG;
    f[0][0][0] = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j <= k; j++)
            for (int s = 0; s < 2; s++) {
                ll v = f[i][j][s];
                if (v == NEG) continue;
                /* leave / stay outside */
                if (f[i + 1][j][0] < v) f[i + 1][j][0] = v;
                /* element in segment: start new (j+1) or continue */
                if (s == 1 && f[i + 1][j][1] < v + a[i]) f[i + 1][j][1] = v + a[i];
                if (j < k && f[i + 1][j + 1][1] < v + a[i]) f[i + 1][j + 1][1] = v + a[i];
            }
    ll r = f[n][k][0] > f[n][k][1] ? f[n][k][0] : f[n][k][1];
    return r;
}

/* penalized: every segment costs lambda; returns best value and the min number of segments among ties */
static void penalized(int n, ll lambda, ll *val, int *cnt) {
    ll v0 = 0, v1 = -(1LL << 50);
    int c0 = 0, c1 = 0;
    for (int i = 0; i < n; i++) {
        /* new state 1: continue (v1) or start (v0 - lambda) */
        ll s1 = v1, s0 = v0;
        int k1 = c1, k0 = c0;
        ll nv1; int nc1;
        ll start = s0 - lambda;
        int startc = k0 + 1;
        if (start > s1 || (start == s1 && startc < k1)) { nv1 = start; nc1 = startc; } else { nv1 = s1; nc1 = k1; }
        nv1 += a[i];
        /* state 0: stay outside or close segment */
        ll nv0; int nc0;
        if (s1 > s0 || (s1 == s0 && k1 < k0)) { nv0 = s1; nc0 = k1; } else { nv0 = s0; nc0 = k0; }
        v0 = nv0; c0 = nc0; v1 = nv1; c1 = nc1;
    }
    if (v1 > v0 || (v1 == v0 && c1 < c0)) { *val = v1; *cnt = c1; } else { *val = v0; *cnt = c0; }
}

int main(void) {
    for (int t = 0; t < 6; t++) {
        int n = 20 + rr(N - 20);
        for (int i = 0; i < n; i++) a[i] = rr(41) - 20;
        int maxk = n / 2;
        /* how many positive-gain segments are worthwhile: ask lambda=0 result */
        ll v; int c;
        penalized(n, 0, &v, &c);
        int kmax = c; /* min count at zero penalty */
        int k = kmax > 1 ? 1 + rr(kmax) : 1;
        if (k > maxk) k = maxk;
        ll lo = 0, hi = 2000, ans = -1;
        while (lo <= hi) {
            ll mid = (lo + hi) / 2;
            penalized(n, mid, &v, &c);
            if (c <= k) { ans = mid; hi = mid - 1; } else lo = mid + 1;
        }
        CHECK(ans >= 0);
        penalized(n, ans, &v, &c);
        ll aliens = v + ans * k;
        ll ref = exact_k(n, k);
        CHECK(aliens == ref);
        printf("case %d: n=%d zero_penalty_segments=%d k=%d lambda=%lld best=%lld\n", t, n, kmax, k, ans, ref);
    }
    return 0;
}
