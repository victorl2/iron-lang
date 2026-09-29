/*
 * title: When does greedy coin change work
 * topic: algorithms
 * covers: greedy vs dynamic programming, canonical coin systems, smallest counterexample, counterexample bound
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define MAXAMT 2000
#define MAXC 8

static unsigned st = 97531u;

static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* coins are ascending, coins[0] == 1 so every amount is reachable */
static int greedy(const int *coins, int n, int amount) {
    int cnt = 0;
    for (int i = n - 1; i >= 0; i--) {
        cnt += amount / coins[i];
        amount %= coins[i];
    }
    return cnt;
}

static int dp_table[MAXAMT + 1];

static void build_dp(const int *coins, int n, int limit) {
    dp_table[0] = 0;
    for (int a = 1; a <= limit; a++) {
        int best = 1 << 30;
        for (int i = 0; i < n; i++)
            if (coins[i] <= a && dp_table[a - coins[i]] + 1 < best)
                best = dp_table[a - coins[i]] + 1;
        dp_table[a] = best;
    }
}

/* Returns the smallest amount where greedy is worse than optimal, or 0. */
static int first_counterexample(const int *coins, int n, int limit) {
    build_dp(coins, n, limit);
    for (int a = 1; a <= limit; a++)
        if (greedy(coins, n, a) != dp_table[a])
            return a;
    return 0;
}

/* Kannan-Pearson style test: a counterexample, if any, is below c[n-2] + c[n-1];
 * check that candidate window only. */
static int pearson_bound_test(const int *coins, int n) {
    if (n < 3)
        return 0;
    return first_counterexample(coins, n, coins[n - 1] + coins[n - 2]);
}

static void show(const int *coins, int n) {
    printf("{");
    for (int i = 0; i < n; i++)
        printf("%s%d", i ? "," : "", coins[i]);
    printf("}");
}

static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

int main(void) {
    int systems[][MAXC + 1] = {
        {4, 1, 5, 10, 25},          /* US coins: canonical */
        {3, 1, 3, 4},               /* classic failure at 6 */
        {5, 1, 5, 10, 20, 25},      /* fails at 40 */
        {6, 1, 2, 5, 10, 20, 50},   /* euro-like: canonical */
        {4, 1, 5, 12, 25},          /* fails */
        {5, 1, 2, 4, 8, 16},        /* powers of two */
        {4, 1, 10, 21, 25},
        {6, 1, 3, 6, 12, 24, 30},   /* fails */
    };
    for (int s = 0; s < 8; s++) {
        int n = systems[s][0];
        int *c = &systems[s][1];
        int ce = first_counterexample(c, n, MAXAMT);
        int bound_ce = pearson_bound_test(c, n);
        printf("system ");
        show(c, n);
        if (ce) {
            printf(" not canonical: smallest counterexample %d, greedy %d coins vs optimal %d\n", ce,
                   greedy(c, n, ce), dp_table[ce]);
        } else {
            printf(" canonical up to %d\n", MAXAMT);
        }
        check((ce == 0) == (bound_ce == 0), "bound window decides canonicity");
        if (ce)
            check(ce == bound_ce, "smallest counterexample lies inside the window");
    }
    /* random systems: count canonical ones and verify the window property */
    int canonical = 0, total = 0, worst_ce = 0;
    for (int t = 0; t < 300; t++) {
        int n = 3 + (int)(rnd() % 5);
        int c[MAXC];
        c[0] = 1;
        int cur = 1;
        for (int i = 1; i < n; i++) {
            cur += 1 + (int)(rnd() % 9);
            c[i] = cur;
        }
        qsort(c, (size_t)n, sizeof(int), cmp_int);
        int ce = first_counterexample(c, n, 600);
        int wb = pearson_bound_test(c, n);
        check((ce == 0) == (wb == 0), "window test agrees");
        total++;
        if (!ce)
            canonical++;
        else if (ce > worst_ce)
            worst_ce = ce;
    }
    printf("random systems: %d of %d canonical, largest smallest-counterexample %d\n", canonical, total, worst_ce);
    /* the greedy answer is never better than optimal */
    int c[] = {1, 7, 10, 23};
    build_dp(c, 4, 500);
    int bad = 0, excess = 0;
    for (int a = 1; a <= 500; a++) {
        int g = greedy(c, 4, a);
        check(g >= dp_table[a], "greedy never beats optimal");
        if (g > dp_table[a]) {
            bad++;
            excess += g - dp_table[a];
        }
    }
    printf("{1,7,10,23}: greedy wrong for %d of 500 amounts, total excess coins %d\n", bad, excess);
    return 0;
}
