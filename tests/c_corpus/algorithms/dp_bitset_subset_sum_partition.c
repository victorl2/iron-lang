/*
 * title: Bitset subset sum, ways count and equal partition
 * topic: algorithms
 * covers: dynamic programming, subset sum, bit-parallel shifts on uint64 words, counting subsets, partition problem
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned long long rs = 7777777ULL;
static unsigned long long rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static int rr(int n) { return (int)(rnd() % (unsigned long long)n); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); exit(1); } } while (0)

#define WORDS 16
#define BITS (WORDS * 64)

static void shl_or(uint64_t *b, int s) {
    int ws = s / 64, bs = s % 64;
    for (int i = WORDS - 1; i >= ws; i--) {
        uint64_t x = b[i - ws] << bs;
        if (bs && i - ws - 1 >= 0) x |= b[i - ws - 1] >> (64 - bs);
        b[i] |= x;
    }
}
static int getb(const uint64_t *b, int i) { return (int)(b[i / 64] >> (i % 64) & 1); }

int main(void) {
    for (int t = 0; t < 6; t++) {
        int n = 10 + rr(8), a[32], total = 0;
        for (int i = 0; i < n; i++) { a[i] = 1 + rr(60); total += a[i]; }
        CHECK(total < BITS);
        uint64_t bs[WORDS];
        memset(bs, 0, sizeof bs);
        bs[0] = 1;
        for (int i = 0; i < n; i++) shl_or(bs, a[i]);
        /* counting DP, same reachability */
        unsigned long long ways[BITS] = {0};
        ways[0] = 1;
        for (int i = 0; i < n; i++)
            for (int s = total; s >= a[i]; s--) ways[s] += ways[s - a[i]];
        int reachable = 0;
        for (int s = 0; s <= total; s++) {
            CHECK(getb(bs, s) == (ways[s] > 0));
            reachable += getb(bs, s);
        }
        unsigned long long sum_ways = 0;
        for (int s = 0; s <= total; s++) sum_ways += ways[s];
        CHECK(sum_ways == (1ULL << n));
        int best_diff = total;
        for (int s = 0; s <= total / 2; s++)
            if (getb(bs, s) && total - 2 * s < best_diff) best_diff = total - 2 * s;
        printf("case %d: n=%d total=%d reachable=%d ways(total/2)=%llu min_diff=%d\n", t, n, total, reachable,
               ways[total / 2], best_diff);
        /* brute-force check of min difference */
        int bd = total;
        for (unsigned m = 0; m < (1u << n); m++) {
            int s = 0;
            for (int i = 0; i < n; i++) if (m >> i & 1) s += a[i];
            int d = total - 2 * s;
            if (d < 0) d = -d;
            if (d < bd) bd = d;
        }
        CHECK(bd == best_diff);
    }
    return 0;
}
