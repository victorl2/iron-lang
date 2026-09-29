/*
 * title: Counting topological orders with subset DP
 * topic: algorithms
 * covers: topological orders, bitmask dynamic programming, predecessor masks, backtracking cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { N = 15 };

static unsigned st = 4242u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void check(int c, const char *m) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", m);
        exit(1);
    }
}

static unsigned pred[N];
static unsigned long long bt_count;

static void backtrack(unsigned placed, int depth) {
    if (depth == N) {
        bt_count++;
        return;
    }
    for (int v = 0; v < N; v++)
        if (!(placed >> v & 1u) && (pred[v] & ~placed) == 0) backtrack(placed | 1u << v, depth + 1);
}

static unsigned long long count_orders(int n, const unsigned *pr) {
    unsigned long long *dp = calloc((size_t)1 << n, sizeof *dp);
    check(dp != NULL, "alloc");
    dp[0] = 1;
    for (unsigned mask = 0; mask < (1u << n); mask++) {
        if (!dp[mask]) continue;
        for (int v = 0; v < n; v++)
            if (!(mask >> v & 1u) && (pr[v] & ~mask) == 0) dp[mask | 1u << v] += dp[mask];
    }
    unsigned long long r = dp[(1u << n) - 1];
    free(dp);
    return r;
}

static unsigned long long fact(int n) {
    unsigned long long f = 1;
    for (int i = 2; i <= n; i++) f *= (unsigned long long)i;
    return f;
}

int main(void) {
    /* no constraints: N! */
    unsigned none[N] = {0};
    unsigned long long free_count = count_orders(N, none);
    check(free_count == fact(N), "unconstrained equals factorial");
    printf("no constraints, n=%d: %llu\n", N, free_count);

    /* chain: exactly one order */
    unsigned chain[N] = {0};
    for (int v = 1; v < N; v++) chain[v] = 1u << (v - 1);
    printf("chain: %llu\n", count_orders(N, chain));

    /* two chains interleaved: binomial coefficient */
    unsigned two[N] = {0};
    for (int v = 2; v < N; v++) two[v] = 1u << (v - 2);
    unsigned long long binom = 1;
    for (int i = 1; i <= 7; i++) binom = binom * (unsigned long long)(N - 7 + i) / (unsigned long long)i;
    unsigned long long two_count = count_orders(N, two);
    /* chains of lengths 8 and 7 */
    check(two_count == binom, "two chains give C(15,7)");
    printf("two chains (8+7): %llu\n", two_count);

    /* star: root first, then (N-1)! */
    unsigned star[N] = {0};
    for (int v = 1; v < N; v++) star[v] = 1u;
    printf("star: %llu (expected %llu)\n", count_orders(N, star), fact(N - 1));

    /* random DAGs verified by backtracking */
    for (int trial = 0; trial < 5; trial++) {
        for (int v = 0; v < N; v++) {
            pred[v] = 0;
            for (int u = 0; u < v; u++)
                if (rnd() % 100u < (unsigned)(22 + 6 * trial)) pred[v] |= 1u << u;
        }
        unsigned long long dp = count_orders(N, pred);
        bt_count = 0;
        backtrack(0, 0);
        check(dp == bt_count, "dp equals backtracking");
        int edges = 0;
        for (int v = 0; v < N; v++) for (int u = 0; u < v; u++) edges += (int)(pred[v] >> u & 1u);
        printf("random dag %d: edges %d, orders %llu\n", trial, edges, dp);
    }
    return 0;
}
