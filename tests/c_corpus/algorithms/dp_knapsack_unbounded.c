/*
 * title: Unbounded knapsack and rod cutting
 * topic: algorithms
 * covers: dynamic programming, unbounded knapsack, rod cutting, item multiplicity recovery, exact-fill variant
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 0x9E3779B97F4A7C15ULL;
static unsigned long long rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static int rr(int n) { return (int)(rnd() % (unsigned long long)n); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); exit(1); } } while (0)

#define NEG (-1000000)

static int rec_best(const int *w, const int *v, int n, int cap) {
    int best = 0;
    for (int i = 0; i < n; i++)
        if (w[i] <= cap) {
            int x = v[i] + rec_best(w, v, n, cap - w[i]);
            if (x > best) best = x;
        }
    return best;
}

int main(void) {
    for (int t = 0; t < 5; t++) {
        int n = 3 + rr(3), cap = 12 + rr(14);
        int w[6], v[6];
        for (int i = 0; i < n; i++) { w[i] = 2 + rr(7); v[i] = 3 + rr(20); }
        int d[64], ex[64], pick[64];
        d[0] = 0; ex[0] = 0; pick[0] = -1;
        for (int c = 1; c <= cap; c++) {
            d[c] = d[c - 1]; /* "at most" variant: allow wasted capacity */
            ex[c] = NEG;
            pick[c] = -1;
            for (int i = 0; i < n; i++)
                if (w[i] <= c) {
                    if (d[c - w[i]] + v[i] > d[c]) { d[c] = d[c - w[i]] + v[i]; pick[c] = i; }
                    if (ex[c - w[i]] > NEG && ex[c - w[i]] + v[i] > ex[c]) ex[c] = ex[c - w[i]] + v[i];
                }
        }
        CHECK(d[cap] == rec_best(w, v, n, cap));
        int cnt[6] = {0}, c = cap, tw = 0;
        while (c > 0) {
            if (pick[c] < 0) { c--; continue; }
            cnt[pick[c]]++;
            tw += w[pick[c]];
            c -= w[pick[c]];
        }
        int tv = 0;
        for (int i = 0; i < n; i++) tv += cnt[i] * v[i];
        CHECK(tv == d[cap]);
        printf("case %d: cap=%d best=%d exact=", t, cap, d[cap]);
        if (ex[cap] > NEG) printf("%d", ex[cap]); else printf("none");
        printf(" counts=");
        for (int i = 0; i < n; i++) printf("%d(w%d,v%d) ", cnt[i], w[i], v[i]);
        printf("used=%d\n", tw);
    }
    /* rod cutting, CLRS price table */
    int price[11] = {0, 1, 5, 8, 9, 10, 17, 17, 20, 24, 30};
    int r[11], cut[11];
    r[0] = 0;
    for (int len = 1; len <= 10; len++) {
        r[len] = -1;
        for (int k = 1; k <= len; k++)
            if (price[k] + r[len - k] > r[len]) { r[len] = price[k] + r[len - k]; cut[len] = k; }
    }
    int expect[11] = {0, 1, 5, 8, 10, 13, 17, 18, 22, 25, 30};
    for (int len = 1; len <= 10; len++) {
        CHECK(r[len] == expect[len]);
        printf("rod %2d: %2d cuts:", len, r[len]);
        for (int x = len; x > 0; x -= cut[x]) printf(" %d", cut[x]);
        printf("\n");
    }
    return 0;
}
