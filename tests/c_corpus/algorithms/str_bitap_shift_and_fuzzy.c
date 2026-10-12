/*
 * title: Shift-And and Wu-Manber fuzzy bitap
 * topic: algorithms
 * covers: bit-parallel matching, Shift-And, Wu-Manber approximate search, Sellers DP cross-check, 64-bit masks
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

/* Shift-And exact matching for patterns up to 64 characters. */
static int shift_and(const char *t, const char *p, int *out) {
    int m = (int)strlen(p), c = 0;
    u64 mask[256] = {0};
    for (int i = 0; i < m; i++)
        mask[(unsigned char)p[i]] |= 1ULL << i;
    u64 d = 0, accept = 1ULL << (m - 1);
    for (int i = 0; t[i]; i++) {
        d = ((d << 1) | 1ULL) & mask[(unsigned char)t[i]];
        if (d & accept)
            out[c++] = i - m + 1;
    }
    return c;
}

/* Wu-Manber bitap with up to k errors (Levenshtein). A clear bit i in R[e] means the
 * pattern prefix of length i matches a suffix of the text read so far with <= e errors.
 * best[i] receives the smallest e for text position i, or k+1 if none. */
static void bitap_fuzzy(const char *t, const char *p, int k, int *best) {
    int m = (int)strlen(p);
    u64 mask[256];
    for (int i = 0; i < 256; i++)
        mask[i] = ~0ULL;
    for (int i = 0; i < m; i++)
        mask[(unsigned char)p[i]] &= ~(1ULL << i);
    u64 R[8], old[8];
    for (int e = 0; e <= k; e++)
        R[e] = ~0ULL << (e + 1);
    for (int i = 0; t[i]; i++) {
        u64 mk = mask[(unsigned char)t[i]];
        memcpy(old, R, sizeof(u64) * (size_t)(k + 1));
        R[0] = (old[0] | mk) << 1;
        for (int e = 1; e <= k; e++)
            R[e] = ((old[e] | mk) << 1) & (old[e - 1] << 1) & old[e - 1] & (R[e - 1] << 1);
        best[i] = k + 1;
        for (int e = 0; e <= k; e++)
            if (!(R[e] & (1ULL << m))) {
                best[i] = e;
                break;
            }
    }
}

/* Sellers' dynamic programming: minimum edit distance of p to any substring ending at i. */
static void sellers(const char *t, const char *p, int *dist) {
    int m = (int)strlen(p);
    int col[80], nc[80];
    for (int i = 0; i <= m; i++)
        col[i] = i;
    for (int j = 0; t[j]; j++) {
        nc[0] = 0;
        for (int i = 1; i <= m; i++) {
            int v = col[i - 1] + (p[i - 1] != t[j]);
            if (col[i] + 1 < v)
                v = col[i] + 1;
            if (nc[i - 1] + 1 < v)
                v = nc[i - 1] + 1;
            nc[i] = v;
        }
        memcpy(col, nc, sizeof(int) * (size_t)(m + 1));
        dist[j] = col[m];
    }
}

int main(void) {
    static char t[3001];
    static int a[3001], b[3001], best[3001], dist[3001];
    const char *pats[] = {"abcab", "aabba", "cabcabcab", "abcdefg"};
    int alph[] = {3, 2, 3, 26};
    for (int c = 0; c < 4; c++) {
        rand_str(t, 3000, alph[c]);
        int na = shift_and(t, pats[c], a);
        int m = (int)strlen(pats[c]), nb = 0;
        for (int i = 0; i + m <= 3000; i++)
            if (memcmp(t + i, pats[c], (size_t)m) == 0)
                b[nb++] = i;
        check(na == nb, "shift-and count");
        for (int i = 0; i < na; i++)
            check(a[i] == b[i], "shift-and pos");
        printf("shift-and %-10s exact hits=%d", pats[c], na);
        for (int k = 1; k <= 3; k++) {
            bitap_fuzzy(t, pats[c], k, best);
            sellers(t, pats[c], dist);
            int hits = 0;
            for (int i = 0; i < 3000; i++) {
                int expect = dist[i] <= k ? dist[i] : k + 1;
                check(best[i] == expect, "bitap vs sellers");
                hits += best[i] <= k;
            }
            printf(" k=%d:%d", k, hits);
        }
        printf("\n");
    }
    return 0;
}
