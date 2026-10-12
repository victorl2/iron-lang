/*
 * title: Matrix chain multiplication order
 * topic: algorithms
 * covers: dynamic programming, interval DP, split table, parenthesization output, multiplication count validation, Catalan enumeration check
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
static long cost[N][N];
static int split[N][N];

static void paren(int i, int j, char *buf, int *pos) {
    if (i == j) { *pos += snprintf(buf + *pos, 16, "M%d", i + 1); return; }
    buf[(*pos)++] = '(';
    paren(i, split[i][j], buf, pos);
    buf[(*pos)++] = ' ';
    paren(split[i][j] + 1, j, buf, pos);
    buf[(*pos)++] = ')';
    buf[*pos] = 0;
}

static long brute(const int *p, int i, int j) {
    if (i == j) return 0;
    long best = -1;
    for (int k = i; k < j; k++) {
        long c = brute(p, i, k) + brute(p, k + 1, j) + (long)p[i] * p[k + 1] * p[j + 1];
        if (best < 0 || c < best) best = c;
    }
    return best;
}

static long count_trees(int n) { /* Catalan(n-1) via its own DP */
    long c[N + 1] = {1};
    for (int i = 1; i < n; i++) { c[i] = 0; for (int j = 0; j < i; j++) c[i] += c[j] * c[i - 1 - j]; }
    return c[n - 1];
}

int main(void) {
    int classic[] = {30, 35, 15, 5, 10, 20, 25}; /* CLRS: 15125 */
    int sets[5][N + 1];
    int lens[5] = {6, 4, 7, 8, 10};
    memcpy(sets[0], classic, sizeof classic);
    int nm[5] = {6, 0, 0, 0, 0};
    for (int s = 1; s < 5; s++) {
        nm[s] = lens[s];
        for (int i = 0; i <= nm[s]; i++) sets[s][i] = 5 + rr(45);
    }
    for (int s = 0; s < 5; s++) {
        int n = nm[s];
        const int *p = sets[s];
        for (int i = 0; i < n; i++) cost[i][i] = 0;
        for (int len = 2; len <= n; len++)
            for (int i = 0; i + len - 1 < n; i++) {
                int j = i + len - 1;
                cost[i][j] = -1;
                for (int k = i; k < j; k++) {
                    long c = cost[i][k] + cost[k + 1][j] + (long)p[i] * p[k + 1] * p[j + 1];
                    if (cost[i][j] < 0 || c < cost[i][j]) { cost[i][j] = c; split[i][j] = k; }
                }
            }
        CHECK(cost[0][n - 1] == brute(p, 0, n - 1));
        char buf[256];
        int pos = 0;
        paren(0, n - 1, buf, &pos);
        printf("n=%d cost=%ld trees=%ld order=%s\n", n, cost[0][n - 1], count_trees(n), buf);
    }
    CHECK(cost[0][0] == 0);
    return 0;
}
