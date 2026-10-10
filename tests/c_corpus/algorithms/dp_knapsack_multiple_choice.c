/*
 * title: Group (multiple-choice) knapsack
 * topic: algorithms
 * covers: dynamic programming, group knapsack, at most one item per group, layer ordering, brute force product enumeration
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 424242424242ULL;
static unsigned long long rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static int rr(int n) { return (int)(rnd() % (unsigned long long)n); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); exit(1); } } while (0)

#define G 6
#define K 4
static int gw[G][K], gv[G][K], gn[G];

static int brute(int g, int cap) {
    if (g == G) return 0;
    int best = brute(g + 1, cap); /* skip group */
    for (int j = 0; j < gn[g]; j++)
        if (gw[g][j] <= cap) {
            int x = gv[g][j] + brute(g + 1, cap - gw[g][j]);
            if (x > best) best = x;
        }
    return best;
}

int main(void) {
    for (int t = 0; t < 8; t++) {
        int cap = 20 + rr(60);
        for (int g = 0; g < G; g++) {
            gn[g] = 1 + rr(K);
            for (int j = 0; j < gn[g]; j++) { gw[g][j] = 2 + rr(25); gv[g][j] = 1 + rr(50); }
        }
        int d[100];
        memset(d, 0, sizeof d);
        for (int g = 0; g < G; g++) {
            for (int c = cap; c >= 0; c--) {
                int best = d[c];
                for (int j = 0; j < gn[g]; j++)
                    if (gw[g][j] <= c && d[c - gw[g][j]] + gv[g][j] > best) {
                        best = d[c - gw[g][j]] + gv[g][j];
                    }
                d[c] = best;
            }
        }
        /* d is updated in place downward, so a group never sees its own updates. */
        int ans = d[cap];
        CHECK(ans == brute(0, cap));
        printf("case %d: cap=%d best=%d sizes=", t, cap, ans);
        for (int g = 0; g < G; g++) printf("%d", gn[g]);
        printf("\n");
    }
    return 0;
}
