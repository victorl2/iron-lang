/*
 * title: Suffix array by prefix doubling with Kasai LCP
 * topic: algorithms
 * covers: suffix array, prefix doubling, Kasai LCP, qsort with total order, brute-force cross-check
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

static const char *g_s;
static int g_n;
static int g_k;
static int *g_rank;

static int cmp_sa(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    if (g_rank[x] != g_rank[y])
        return g_rank[x] < g_rank[y] ? -1 : 1;
    int rx = x + g_k < g_n ? g_rank[x + g_k] : -1;
    int ry = y + g_k < g_n ? g_rank[y + g_k] : -1;
    if (rx != ry)
        return rx < ry ? -1 : 1;
    return 0;
}

/* Prefix doubling, O(n log^2 n). Returns number of doubling rounds used. */
static int build_sa(const char *s, int n, int *sa) {
    int *rank = malloc(sizeof(int) * (size_t)n);
    int *tmp = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) {
        sa[i] = i;
        rank[i] = (unsigned char)s[i];
    }
    g_s = s;
    g_n = n;
    g_rank = rank;
    int rounds = 0;
    for (g_k = 1;; g_k <<= 1) {
        qsort(sa, (size_t)n, sizeof(int), cmp_sa);
        tmp[sa[0]] = 0;
        for (int i = 1; i < n; i++)
            tmp[sa[i]] = tmp[sa[i - 1]] + (cmp_sa(&sa[i - 1], &sa[i]) < 0 ? 1 : 0);
        memcpy(rank, tmp, sizeof(int) * (size_t)n);
        rounds++;
        if (rank[sa[n - 1]] == n - 1)
            break;
    }
    free(rank);
    free(tmp);
    return rounds;
}

static void kasai(const char *s, int n, const int *sa, int *lcp) {
    int *rank = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++)
        rank[sa[i]] = i;
    int h = 0;
    lcp[0] = 0;
    for (int i = 0; i < n; i++) {
        if (rank[i] > 0) {
            int j = sa[rank[i] - 1];
            while (i + h < n && j + h < n && s[i + h] == s[j + h])
                h++;
            lcp[rank[i]] = h;
            if (h > 0)
                h--;
        } else {
            h = 0;
        }
    }
    free(rank);
}

static int cmp_suffix_brute(const void *a, const void *b) {
    return strcmp(g_s + *(const int *)a, g_s + *(const int *)b);
}

int main(void) {
    const char *banana = "banana";
    int sa[64], lcp[64];
    int rounds = build_sa(banana, 6, sa);
    kasai(banana, 6, sa, lcp);
    printf("banana: rounds=%d\n", rounds);
    for (int i = 0; i < 6; i++)
        printf("  sa[%d]=%d lcp=%d %s\n", i, sa[i], lcp[i], banana + sa[i]);

    static char s[2001];
    static int sb[2001], sc[2001], lc[2001];
    int alphas[] = {2, 3, 26, 1};
    int lens[] = {1500, 1200, 2000, 300};
    for (int c = 0; c < 4; c++) {
        int n = lens[c];
        rand_str(s, n, alphas[c]);
        int r = build_sa(s, n, sb);
        for (int i = 0; i < n; i++)
            sc[i] = i;
        g_s = s;
        qsort(sc, (size_t)n, sizeof(int), cmp_suffix_brute);
        for (int i = 0; i < n; i++)
            check(sb[i] == sc[i], "sa equals sorted suffixes");
        kasai(s, n, sb, lc);
        long sum = 0;
        int mx = 0;
        for (int i = 1; i < n; i++) {
            int k = 0;
            while (s[sb[i - 1] + k] && s[sb[i - 1] + k] == s[sb[i] + k])
                k++;
            check(k == lc[i], "kasai lcp");
            sum += lc[i];
            if (lc[i] > mx)
                mx = lc[i];
        }
        printf("n=%d alphabet=%d rounds=%d max_lcp=%d sum_lcp=%ld sa[0]=%d\n", n, alphas[c], r, mx,
               sum, sb[0]);
    }
    return 0;
}
