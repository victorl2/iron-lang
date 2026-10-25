/*
 * title: Stock trading DP: k transactions, cooldown, and fees
 * topic: algorithms
 * covers: dynamic programming, state machine DP, hold/sold/rest states, at most k transactions, cooldown, transaction fee, exhaustive recursion cross-check
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

#define N 16

static int price[N];

/* exhaustive: state = day, holding, transactions left, cooldown flag */
static int brute(int d, int n, int hold, int k, int cool, int fee, int cooldown) {
    if (d == n) return 0;
    int best = brute(d + 1, n, hold, k, cool ? 0 : 0, fee, cooldown); /* do nothing (cooldown ends) */
    if (cooldown) { /* cool is a countdown for the cooldown variant */
        best = brute(d + 1, n, hold, k, cool > 0 ? cool - 1 : 0, fee, cooldown);
    }
    if (hold) {
        int v = price[d] - fee + brute(d + 1, n, 0, k, cooldown ? 1 : 0, fee, cooldown);
        if (v > best) best = v;
    } else if (k > 0 && (!cooldown || cool == 0)) {
        int v = -price[d] + brute(d + 1, n, 1, k - 1, 0, fee, cooldown);
        if (v > best) best = v;
    }
    return best;
}

static int at_most_k(int n, int k) {
    int buy[N + 1], sell[N + 1];
    for (int j = 0; j <= k; j++) { buy[j] = -(1 << 28); sell[j] = 0; }
    for (int d = 0; d < n; d++)
        for (int j = 1; j <= k; j++) {
            if (sell[j - 1] - price[d] > buy[j]) buy[j] = sell[j - 1] - price[d];
            if (buy[j] + price[d] > sell[j]) sell[j] = buy[j] + price[d];
        }
    return sell[k];
}

static int with_cooldown(int n) {
    int hold = -(1 << 28), sold = 0, rest = 0;
    for (int d = 0; d < n; d++) {
        int nh = hold > rest - price[d] ? hold : rest - price[d];
        int ns = hold + price[d];
        int nr = rest > sold ? rest : sold;
        hold = nh; sold = ns; rest = nr;
    }
    return sold > rest ? sold : rest;
}

static int with_fee(int n, int fee) {
    int cash = 0, hold = -price[0];
    for (int d = 1; d < n; d++) {
        int nc = cash > hold + price[d] - fee ? cash : hold + price[d] - fee;
        int nh = hold > cash - price[d] ? hold : cash - price[d];
        cash = nc; hold = nh;
    }
    return cash;
}

static int unlimited(int n) {
    int s = 0;
    for (int d = 1; d < n; d++) if (price[d] > price[d - 1]) s += price[d] - price[d - 1];
    return s;
}

int main(void) {
    int demo[] = {3, 2, 6, 5, 0, 3};
    memcpy(price, demo, sizeof demo);
    CHECK(at_most_k(6, 2) == 7);
    printf("demo: k=1 %d, k=2 %d, unlimited %d, cooldown %d\n", at_most_k(6, 1), at_most_k(6, 2), unlimited(6),
           with_cooldown(6));
    for (int t = 0; t < 8; t++) {
        int n = 6 + rr(N - 6);
        price[0] = 20 + rr(30);
        for (int i = 1; i < n; i++) { price[i] = price[i - 1] + rr(15) - 7; if (price[i] < 1) price[i] = 1; }
        int fee = 1 + rr(4);
        int k1 = at_most_k(n, 1), k2 = at_most_k(n, 2), k3 = at_most_k(n, 3), kinf = unlimited(n);
        int cd = with_cooldown(n), wf = with_fee(n, fee);
        CHECK(k1 == brute(0, n, 0, 1, 0, 0, 0));
        CHECK(k2 == brute(0, n, 0, 2, 0, 0, 0));
        CHECK(k3 == brute(0, n, 0, 3, 0, 0, 0));
        CHECK(kinf == at_most_k(n, n));
        CHECK(cd == brute(0, n, 0, n, 0, 0, 1));
        CHECK(wf == brute(0, n, 0, n, 0, fee, 0));
        CHECK(k1 <= k2 && k2 <= k3 && k3 <= kinf && cd <= kinf && wf <= kinf);
        printf("case %d: n=%2d k1=%3d k2=%3d k3=%3d unlimited=%3d cooldown=%3d fee(%d)=%3d\n", t, n, k1, k2, k3, kinf, cd,
               fee, wf);
    }
    return 0;
}
