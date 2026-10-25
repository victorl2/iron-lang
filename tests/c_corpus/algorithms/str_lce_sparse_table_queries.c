/*
 * title: O(1) longest common extension with sparse table
 * topic: algorithms
 * covers: suffix array, Kasai LCP, sparse table range minimum, LCE queries, squares counting, naive cross-check
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

/* LCE queries in O(1): suffix array + Kasai LCP + sparse table for range minimum. */
enum { MAXN = 2048, LOG = 12 };

static const char *g_s;
static int g_n;
static int sa[MAXN], rk[MAXN], lcp[MAXN];
static int sparse[LOG][MAXN];
static int lg[MAXN + 1];

static int cmp_suf(const void *a, const void *b) {
    return strcmp(g_s + *(const int *)a, g_s + *(const int *)b);
}

static void build_all(const char *s, int n) {
    g_s = s;
    g_n = n;
    for (int i = 0; i < n; i++)
        sa[i] = i;
    qsort(sa, (size_t)n, sizeof(int), cmp_suf);
    for (int i = 0; i < n; i++)
        rk[sa[i]] = i;
    int h = 0;
    lcp[0] = 0;
    for (int i = 0; i < n; i++) {
        if (rk[i] == 0) {
            h = 0;
            continue;
        }
        int j = sa[rk[i] - 1];
        while (i + h < n && j + h < n && s[i + h] == s[j + h])
            h++;
        lcp[rk[i]] = h;
        if (h)
            h--;
    }
    lg[1] = 0;
    for (int i = 2; i <= n; i++)
        lg[i] = lg[i / 2] + 1;
    for (int i = 0; i < n; i++)
        sparse[0][i] = lcp[i];
    for (int k = 1; (1 << k) <= n; k++)
        for (int i = 0; i + (1 << k) <= n; i++) {
            int a = sparse[k - 1][i], b = sparse[k - 1][i + (1 << (k - 1))];
            sparse[k][i] = a < b ? a : b;
        }
}

static int lce(int i, int j) {
    if (i == j)
        return g_n - i;
    int a = rk[i], b = rk[j];
    if (a > b) {
        int t = a;
        a = b;
        b = t;
    }
    /* min of lcp[a+1..b] */
    int l = a + 1, len = b - a, k = lg[len];
    int x = sparse[k][l], y = sparse[k][b - (1 << k) + 1];
    return x < y ? x : y;
}

static int lce_naive(const char *s, int n, int i, int j) {
    int k = 0;
    while (i + k < n && j + k < n && s[i + k] == s[j + k])
        k++;
    return k;
}

/* Use LCE to compute the number of positions where a text of length n has a square
 * (s[i..i+p) == s[i+p..i+2p)) for each half-length p. */
static long squares_at(int n, int p) {
    long c = 0;
    for (int i = 0; i + 2 * p <= n; i++)
        if (lce(i, i + p) >= p)
            c++;
    return c;
}

int main(void) {
    static char s[MAXN];
    int alph[] = {2, 3, 1, 26};
    int lens[] = {1500, 1200, 300, 2000};
    for (int c = 0; c < 4; c++) {
        int n = lens[c];
        rand_str(s, n, alph[c]);
        build_all(s, n);
        long sum = 0;
        int mx = 0;
        for (int q = 0; q < 6000; q++) {
            int i = (int)(rnd() % (unsigned)n), j = (int)(rnd() % (unsigned)n);
            int a = lce(i, j), b = lce_naive(s, n, i, j);
            check(a == b, "LCE vs naive");
            sum += a;
            if (a > mx)
                mx = a;
        }
        printf("n=%d alpha=%d: sum of 6000 LCEs=%ld max=%d\n", n, alph[c], sum, mx);
        if (c == 0) {
            for (int p = 1; p <= 4; p++) {
                long sq = squares_at(n, p), bq = 0;
                for (int i = 0; i + 2 * p <= n; i++)
                    bq += memcmp(s + i, s + i + p, (size_t)p) == 0;
                check(sq == bq, "square count");
                printf("  squares with half-length %d: %ld\n", p, sq);
            }
        }
    }
    return 0;
}
