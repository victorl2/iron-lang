/*
 * title: Dice sum distributions and exact expectations by DP
 * topic: algorithms
 * covers: dynamic programming, convolution of distributions, exact integer counts, cumulative probabilities as fractions, expected value identities, brute-force enumeration
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

typedef unsigned long long u64;
#define MAXS 400

static u64 gcd(u64 a, u64 b) { while (b) { u64 t = a % b; a = b; b = t; } return a; }

/* ways[s]: number of outcomes of n dice with f faces summing to s */
static void distribution(int n, int f, u64 *ways) {
    u64 cur[MAXS] = {0}, nxt[MAXS];
    cur[0] = 1;
    int maxs = 0;
    for (int d = 0; d < n; d++) {
        memset(nxt, 0, sizeof nxt);
        memset(nxt, 0, sizeof nxt);
        for (int s = 0; s <= maxs; s++)
            if (cur[s]) for (int k = 1; k <= f; k++) nxt[s + k] += cur[s];
        maxs += f;
        memcpy(cur, nxt, sizeof cur);
    }
    memcpy(ways, cur, sizeof cur);
}

static u64 brute(int n, int f, int target) {
    if (n == 0) return target == 0;
    u64 t = 0;
    for (int k = 1; k <= f; k++) if (target >= k) t += brute(n - 1, f, target - k);
    return t;
}

int main(void) {
    (void)rr;
    int ns[] = {1, 2, 3, 4, 6, 10};
    int fs[] = {6, 6, 6, 6, 6, 6};
    for (int i = 0; i < 6; i++) {
        int n = ns[i], f = fs[i];
        u64 ways[MAXS];
        distribution(n, f, ways);
        u64 total = 1, sum = 0, weighted = 0, mode = 0;
        for (int k = 0; k < n; k++) total *= (u64)f;
        int mode_s = 0;
        for (int s = 0; s < MAXS; s++) {
            sum += ways[s];
            weighted += ways[s] * (u64)s;
            if (ways[s] > mode) { mode = ways[s]; mode_s = s; }
        }
        CHECK(sum == total);
        /* expectation is n(f+1)/2: weighted*2 == total*n*(f+1) */
        CHECK(weighted * 2 == total * (u64)n * (u64)(f + 1));
        if (n <= 4) for (int s = n; s <= n * f; s++) CHECK(ways[s] == brute(n, f, s));
        /* probability of the mode as a reduced fraction */
        u64 g = gcd(mode, total);
        printf("%dd%d: outcomes=%llu mode_sum=%d (%llu/%llu)", n, f, total, mode_s, mode / g, total / g);
        /* P(sum >= 3.5n) style tail: sum above the mean */
        u64 tail = 0;
        for (int s = 0; s < MAXS; s++) if (2 * s > n * (f + 1)) tail += ways[s];
        g = gcd(tail, total);
        if (tail) printf(" P(above mean)=%llu/%llu", tail / g, total / g); else printf(" P(above mean)=0");
        printf("\n");
    }
    /* famous: 3d6 distribution row and 2d6 */
    u64 w[MAXS];
    distribution(3, 6, w);
    printf("3d6 counts for 3..18:");
    for (int s = 3; s <= 18; s++) printf(" %llu", w[s]);
    printf("\n");
    /* variance via second moment for 10d6: n*(f^2-1)/12 exactly => 12*var_total = n(f^2-1)*total scaled */
    distribution(10, 6, w);
    u64 total = 60466176ULL, m1 = 0, m2 = 0;
    for (int s = 0; s < MAXS; s++) { m1 += w[s] * (u64)s; m2 += w[s] * (u64)s * (u64)s; }
    /* variance*total^2*12 = 12*(m2*total - m1^2) should equal n*(f^2-1)*total^2 */
    /* the exact values exceed 64 bits, so compare them modulo 2^64 (unsigned wraparound) */
    u64 lhs = 12u * (m2 * total - m1 * m1);
    u64 rhs = 10u * 35u * total * total;
    CHECK(lhs == rhs);
    printf("10d6 variance identity holds; mean*2=%llu\n", 2 * m1 / total);
    return 0;
}
