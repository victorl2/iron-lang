/*
 * title: Longest increasing subsequence, n log n with reconstruction
 * topic: algorithms
 * covers: dynamic programming, patience sorting, binary search on tails, predecessor links, counting LIS, O(n^2) cross-check
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

static int lower_bound(const int *a, int n, int x) {
    int lo = 0, hi = n;
    while (lo < hi) { int m = (lo + hi) / 2; if (a[m] < x) lo = m + 1; else hi = m; }
    return lo;
}

int main(void) {
    for (int t = 0; t < 8; t++) {
        int n = 10 + rr(30), a[64];
        int range = t < 4 ? 100 : 6; /* small range forces duplicates */
        for (int i = 0; i < n; i++) a[i] = rr(range);
        int tails[64], tail_idx[64], prev[64], len = 0;
        for (int i = 0; i < n; i++) {
            int p = lower_bound(tails, len, a[i]);
            tails[p] = a[i];
            tail_idx[p] = i;
            prev[i] = p ? tail_idx[p - 1] : -1;
            if (p == len) len++;
        }
        int seq[64], k = len, at = tail_idx[len - 1];
        while (at >= 0) { seq[--k] = a[at]; at = prev[at]; }
        CHECK(k == 0);
        for (int i = 1; i < len; i++) CHECK(seq[i - 1] < seq[i]);
        /* quadratic DP with counts */
        int L[64], C[64], best = 0;
        for (int i = 0; i < n; i++) {
            L[i] = 1; C[i] = 1;
            for (int j = 0; j < i; j++)
                if (a[j] < a[i]) {
                    if (L[j] + 1 > L[i]) { L[i] = L[j] + 1; C[i] = C[j]; }
                    else if (L[j] + 1 == L[i]) C[i] += C[j];
                }
            if (L[i] > best) best = L[i];
        }
        long count = 0;
        for (int i = 0; i < n; i++) if (L[i] == best) count += C[i];
        CHECK(best == len);
        /* verify that seq is a subsequence of a */
        int pos = 0;
        for (int i = 0; i < n && pos < len; i++) if (a[i] == seq[pos]) pos++;
        CHECK(pos == len);
        printf("case %d: n=%d LIS=%d count=%ld seq=", t, n, len, count);
        for (int i = 0; i < len; i++) printf("%d ", seq[i]);
        printf("\n");
    }
    return 0;
}
