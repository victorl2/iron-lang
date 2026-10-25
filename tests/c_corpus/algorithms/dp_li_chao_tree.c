/*
 * title: Li Chao tree for line minimum queries
 * topic: algorithms
 * covers: dynamic programming, Li Chao segment tree, arbitrary slope insertion, dynamic node pool, brute force over lines, DP with non-monotone slopes
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
#define XR 2000 /* domain x in [0, XR) */
#define MAXL 400

typedef struct { ll m, b; } Line;
static Line tree[4 * XR];
static char has[4 * XR];

static ll ev(Line l, ll x) { return l.m * x + l.b; }

static void insert(int node, int lo, int hi, Line nw) {
    if (!has[node]) { tree[node] = nw; has[node] = 1; return; }
    int mid = (lo + hi) / 2;
    int lef = ev(nw, lo) < ev(tree[node], lo);
    int mi = ev(nw, mid) < ev(tree[node], mid);
    if (mi) { Line t = tree[node]; tree[node] = nw; nw = t; }
    if (lo == hi) return;
    if (lef != mi) insert(node * 2, lo, mid, nw);
    else insert(node * 2 + 1, mid + 1, hi, nw);
}

static ll query(int node, int lo, int hi, ll x) {
    ll best = has[node] ? ev(tree[node], x) : (ll)4e18;
    if (lo == hi) return best;
    int mid = (lo + hi) / 2;
    ll sub = x <= mid ? query(node * 2, lo, mid, x) : query(node * 2 + 1, mid + 1, hi, x);
    return sub < best ? sub : best;
}

int main(void) {
    for (int t = 0; t < 4; t++) {
        memset(has, 0, sizeof has);
        Line L[MAXL];
        int nl = 0, checks = 0;
        ll xor_sum = 0;
        for (int step = 0; step < 300; step++) {
            if (rr(3) != 0 && nl < MAXL) {
                Line l = {rr(2001) - 1000, rr(2000001) - 1000000};
                L[nl++] = l;
                insert(1, 0, XR - 1, l);
            } else if (nl) {
                ll x = rr(XR);
                ll best = ev(L[0], x);
                for (int i = 1; i < nl; i++) if (ev(L[i], x) < best) best = ev(L[i], x);
                CHECK(query(1, 0, XR - 1, x) == best);
                checks++;
                xor_sum ^= best & 0xFFFFF;
            }
        }
        printf("run %d: lines=%d queries_checked=%d fingerprint=%lld\n", t, nl, checks, xor_sum);
    }
    /* DP application: dp[i] = min_j dp[j] + (h[i]-h[j])^2, frog jumping with quadratic cost, heights arbitrary */
    ll h[200], dp[200], slow[200];
    int n = 150;
    for (int i = 0; i < n; i++) h[i] = rr(500);
    memset(has, 0, sizeof has);
    dp[0] = 0;
    insert(1, 0, XR - 1, (Line){-2 * h[0], h[0] * h[0]}); /* covers h in [0,500) but also indices */
    for (int i = 1; i < n; i++) {
        dp[i] = query(1, 0, XR - 1, h[i]) + h[i] * h[i] + 7;
        insert(1, 0, XR - 1, (Line){-2 * h[i], dp[i] + h[i] * h[i]});
    }
    slow[0] = 0;
    for (int i = 1; i < n; i++) {
        slow[i] = (ll)4e18;
        for (int j = 0; j < i; j++) {
            ll c = slow[j] + (h[i] - h[j]) * (h[i] - h[j]) + 7;
            if (c < slow[i]) slow[i] = c;
        }
        CHECK(slow[i] == dp[i]);
    }
    printf("frog dp final=%lld\n", dp[n - 1]);
    return 0;
}
