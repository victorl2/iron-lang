/*
 * title: Hamming-distance search with kangaroo jumps
 * topic: algorithms
 * covers: k mismatches, longest common extension oracle, rolling hash, binary search, planted noisy occurrences
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 0x9e3779b97f4a7c15ULL;

static inline unsigned rnd(void) {
    unsigned long long z = (rng_s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return (unsigned)((z ^ (z >> 31)) >> 16);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline void rand_str(char *s, int n, int alpha) {
    for (int i = 0; i < n; i++)
        s[i] = (char)('a' + rnd() % (unsigned)alpha);
    s[n] = 0;
}

typedef unsigned long long u64;

/* Longest common extension oracle: rolling hash + binary search. */
static u64 P[4001];
static const u64 B = 1000003ULL;

static void powers(void) {
    P[0] = 1;
    for (int i = 0; i < 4000; i++)
        P[i + 1] = P[i] * B;
}

static u64 *prefix_hashes(const char *s, int n) {
    u64 *h = malloc(sizeof(u64) * (size_t)(n + 1));
    h[0] = 0;
    for (int i = 0; i < n; i++)
        h[i + 1] = h[i] * B + (unsigned char)s[i] + 1;
    return h;
}

static u64 sub(const u64 *h, int l, int r) {
    return h[r] - h[l] * P[r - l];
}

/* text and pattern are hashed in separate arrays but share the power table */
static int lce(const u64 *ht, int i, const u64 *hp, int j, int limit) {
    int lo = 0, hi = limit;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (sub(ht, i, i + mid) == sub(hp, j, j + mid))
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

/* Kangaroo jumps: occurrences with at most k mismatches in O(k) LCE queries each. */
static int k_mismatch(const u64 *ht, int n, const u64 *hp, int m, int k, int *out, long *lce_calls) {
    int c = 0;
    for (int i = 0; i + m <= n; i++) {
        int pos = 0, errs = 0;
        while (pos < m && errs <= k) {
            (*lce_calls)++;
            int l = lce(ht, i + pos, hp, pos, m - pos);
            pos += l;
            if (pos < m) {
                errs++;
                pos++;
            }
        }
        if (errs <= k)
            out[c++] = i;
    }
    return c;
}

static int hamming_brute(const char *t, const char *p, int m, int limit) {
    int e = 0;
    for (int i = 0; i < m && e <= limit; i++)
        e += t[i] != p[i];
    return e;
}

int main(void) {
    enum { N = 4000 };
    static char t[N + 1];
    powers();
    static int a[N], b[N];
    const char *pats[] = {"abcabcabc", "aaaaaaaaaa", "abbabbabbabb", "cabbacab"};
    int alph[] = {3, 2, 2, 3};
    for (int c = 0; c < 4; c++) {
        rand_str(t, N, alph[c]);
        int m = (int)strlen(pats[c]);
        /* plant a few noisy copies so that k>0 finds some */
        for (int q = 0; q < 5; q++) {
            int at = 100 + q * 700;
            memcpy(t + at, pats[c], (size_t)m);
            char ch = 'a' + (char)(rnd() % 3); /* value first, then position: the order the expected output uses */
            t[at + (int)(rnd() % (unsigned)m)] = ch;
        }
        u64 *ht = prefix_hashes(t, N);
        u64 *hp = prefix_hashes(pats[c], m);

        printf("pattern %-13s", pats[c]);
        for (int k = 0; k <= 3; k++) {
            long calls = 0;
            int na = k_mismatch(ht, N, hp, m, k, a, &calls);
            int nb = 0;
            for (int i = 0; i + m <= N; i++)
                if (hamming_brute(t + i, pats[c], m, k) <= k)
                    b[nb++] = i;
            check(na == nb, "k-mismatch count");
            for (int i = 0; i < na; i++)
                check(a[i] == b[i], "k-mismatch pos");
            printf(" k=%d:%d(%ld)", k, na, calls);
        }
        printf("\n");
        free(ht);
        free(hp);
    }
    return 0;
}
