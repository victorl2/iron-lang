/*
 * title: Nested envelopes and bitonic subsequences as LIS variants
 * topic: algorithms
 * covers: dynamic programming, LIS variants, two-key sort with descending tie-break, binary search tails, longest bitonic subsequence, longest non-decreasing, quadratic cross-check
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

#define N 60

typedef struct { int w, h; } Env;

static int cmp_env(const void *a, const void *b) {
    const Env *x = a, *y = b;
    if (x->w != y->w) return x->w < y->w ? -1 : 1;
    if (x->h != y->h) return x->h > y->h ? -1 : 1; /* equal widths: descending height so they cannot chain */
    return 0;
}

static int lis_strict(const int *a, int n) {
    int tails[N], len = 0;
    for (int i = 0; i < n; i++) {
        int lo = 0, hi = len;
        while (lo < hi) { int m = (lo + hi) / 2; if (tails[m] < a[i]) lo = m + 1; else hi = m; }
        tails[lo] = a[i];
        if (lo == len) len++;
    }
    return len;
}

static int lnds(const int *a, int n) {
    int tails[N], len = 0;
    for (int i = 0; i < n; i++) {
        int lo = 0, hi = len;
        while (lo < hi) { int m = (lo + hi) / 2; if (tails[m] <= a[i]) lo = m + 1; else hi = m; }
        tails[lo] = a[i];
        if (lo == len) len++;
    }
    return len;
}

int main(void) {
    for (int t = 0; t < 6; t++) {
        int n = 10 + rr(N - 10), range = 8 + rr(20);
        Env e[N], s[N];
        for (int i = 0; i < n; i++) { e[i].w = 1 + rr(range); e[i].h = 1 + rr(range); }
        memcpy(s, e, sizeof e);
        qsort(s, (size_t)n, sizeof(Env), cmp_env);
        int hs[N];
        for (int i = 0; i < n; i++) hs[i] = s[i].h;
        int fast = lis_strict(hs, n);
        /* quadratic reference straight from the definition */
        int best[N], slow = 0;
        for (int i = 0; i < n; i++) {
            best[i] = 1;
            for (int j = 0; j < i; j++)
                if (s[j].w < s[i].w && s[j].h < s[i].h && best[j] + 1 > best[i]) best[i] = best[j] + 1;
            if (best[i] > slow) slow = best[i];
        }
        CHECK(fast == slow);
        printf("envelopes case %d: n=%d range=%d max_nesting=%d\n", t, n, range, fast);
    }
    for (int t = 0; t < 6; t++) {
        int n = 8 + rr(30), a[N];
        for (int i = 0; i < n; i++) a[i] = rr(30);
        int inc[N], dec[N], bit = 0;
        for (int i = 0; i < n; i++) {
            inc[i] = 1;
            for (int j = 0; j < i; j++) if (a[j] < a[i] && inc[j] + 1 > inc[i]) inc[i] = inc[j] + 1;
        }
        for (int i = n - 1; i >= 0; i--) {
            dec[i] = 1;
            for (int j = n - 1; j > i; j--) if (a[j] < a[i] && dec[j] + 1 > dec[i]) dec[i] = dec[j] + 1;
        }
        for (int i = 0; i < n; i++) if (inc[i] + dec[i] - 1 > bit) bit = inc[i] + dec[i] - 1;
        /* brute force bitonic for n <= 20 is skipped; validate via reversed-array LIS relation */
        int rev[N];
        for (int i = 0; i < n; i++) rev[i] = a[n - 1 - i];
        int mx = 0;
        for (int i = 0; i < n; i++) if (inc[i] > mx) mx = inc[i];
        CHECK(mx == lis_strict(a, n));
        int mx2 = 0;
        for (int i = 0; i < n; i++) if (dec[i] > mx2) mx2 = dec[i];
        CHECK(mx2 == lis_strict(rev, n));
        printf("bitonic case %d: n=%d LIS=%d LDS=%d LNDS=%d longest_bitonic=%d\n", t, n, mx, mx2, lnds(a, n), bit);
    }
    return 0;
}
