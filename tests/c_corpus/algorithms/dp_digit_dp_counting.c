/*
 * title: Digit DP: counting numbers with digit constraints
 * topic: algorithms
 * covers: dynamic programming, digit DP, tight flag, leading zeros, memoization by position and state, brute force cross-check, ranges via prefix difference
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

/* count x in [0, n] with digit sum divisible by k and no two adjacent equal digits */
static int dig[20], nd;
static ll memo[20][200][11][2]; /* pos, sum mod, last digit (10 = none), started */

static ll go(int pos, int rem, int last, int tight, int started, int k) {
    if (pos == nd) return (rem == 0 && started) ? 1 : (started ? 0 : (rem == 0 ? 1 : 0)); /* 0 counts as digit sum 0 */
    if (!tight && memo[pos][rem][last][started] >= 0) return memo[pos][rem][last][started];
    int hi = tight ? dig[pos] : 9;
    ll r = 0;
    for (int d = 0; d <= hi; d++) {
        int ns = started || d != 0;
        if (ns && started && d == last) continue;
        int nl = ns ? d : 10;
        r += go(pos + 1, (rem + d) % k, nl, tight && d == hi, ns, k);
    }
    if (!tight) memo[pos][rem][last][started] = r;
    return r;
}

static ll count_le(ll n, int k) {
    if (n < 0) return 0;
    nd = 0;
    char buf[24];
    snprintf(buf, 24, "%lld", n);
    for (char *p = buf; *p; p++) dig[nd++] = *p - '0';
    memset(memo, -1, sizeof memo);
    return go(0, 0, 10, 1, 0, k);
}

static int ok(ll x, int k) {
    int s = 0, last = -1;
    if (x == 0) return 1;
    char buf[24];
    snprintf(buf, 24, "%lld", x);
    for (char *p = buf; *p; p++) {
        int d = *p - '0';
        if (d == last) return 0;
        last = d;
        s += d;
    }
    return s % k == 0;
}

/* count numbers in [1,n] containing digit 7 (complement of no-7 numbers) */
static ll no7(ll n) {
    char buf[24];
    snprintf(buf, 24, "%lld", n);
    int len = (int)strlen(buf);
    ll total = 0;
    ll pw[20]; pw[0] = 1;
    for (int i = 1; i < 20; i++) pw[i] = pw[i - 1] * 9;
    for (int i = 0; i < len; i++) {
        int d = buf[i] - '0';
        int less = d - (d > 7 ? 1 : 0); /* digits below d that are not 7 */
        total += less * pw[len - 1 - i];
        if (d == 7) return total - 1; /* tight prefix hits 7; drop zero */
    }
    return total; /* [0,n] without 7, minus zero */
}

int main(void) {
    (void)rr;
    ll ns[] = {9, 100, 999, 12345, 99999, 250000};
    int ks[] = {3, 5, 7, 9};
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 4; j++) {
            ll fast = count_le(ns[i], ks[j]), slow = 0;
            for (ll x = 0; x <= ns[i]; x++) slow += ok(x, ks[j]);
            CHECK(fast == slow);
            printf("n=%lld k=%d count=%lld\n", ns[i], ks[j], fast);
        }
    /* range query through prefix difference on a huge bound, then no-7 closed form */
    ll big = count_le(123456789012LL, 11) - count_le(9876543210LL - 1, 11);
    printf("range k=11 [9876543210,123456789012]: %lld\n", big);
    for (ll n = 1; n <= 2000; n++) {
        ll c = 0;
        for (ll x = 1; x <= n; x++) {
            ll y = x; int has = 0;
            while (y) { if (y % 10 == 7) has = 1; y /= 10; }
            c += !has;
        }
        CHECK(c == no7(n));
        if (n % 500 == 0) printf("numbers up to %lld without digit 7: %lld\n", n, c);
    }
    return 0;
}
