/*
 * title: Distinct substrings and k-th substring from SA and LCP
 * topic: algorithms
 * covers: suffix array, LCP, distinct substring count, k-th substring, brute-force cross-check
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

static int cmp_suf(const void *a, const void *b) {
    return strcmp(g_s + *(const int *)a, g_s + *(const int *)b);
}

static void build_sa(const char *s, int n, int *sa) {
    for (int i = 0; i < n; i++)
        sa[i] = i;
    g_s = s;
    qsort(sa, (size_t)n, sizeof(int), cmp_suf);
}

static void build_lcp(const char *s, int n, const int *sa, int *lcp) {
    int *rank = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++)
        rank[sa[i]] = i;
    int h = 0;
    lcp[0] = 0;
    for (int i = 0; i < n; i++) {
        if (rank[i] == 0) {
            h = 0;
            continue;
        }
        int j = sa[rank[i] - 1];
        while (i + h < n && j + h < n && s[i + h] == s[j + h])
            h++;
        lcp[rank[i]] = h;
        if (h > 0)
            h--;
    }
    free(rank);
}

static long distinct_from_sa(int n, const int *sa, const int *lcp) {
    long total = 0;
    for (int i = 0; i < n; i++)
        total += (n - sa[i]) - lcp[i];
    return total;
}

static int cmp_str_ptr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* brute: collect every substring, sort, count unique */
static long distinct_brute(const char *s, int n) {
    int cnt = n * (n + 1) / 2;
    char **all = malloc(sizeof(char *) * (size_t)cnt);
    int k = 0;
    for (int i = 0; i < n; i++)
        for (int l = 1; i + l <= n; l++) {
            char *p = malloc((size_t)l + 1);
            memcpy(p, s + i, (size_t)l);
            p[l] = 0;
            all[k++] = p;
        }
    qsort(all, (size_t)cnt, sizeof(char *), cmp_str_ptr);
    long u = 1;
    for (int i = 1; i < cnt; i++)
        if (strcmp(all[i], all[i - 1]) != 0)
            u++;
    for (int i = 0; i < cnt; i++)
        free(all[i]);
    free(all);
    return u;
}

/* k-th (1-based) distinct substring in lexicographic order, written to out. */
static int kth_substring(const char *s, int n, const int *sa, const int *lcp, long k, char *out) {
    for (int i = 0; i < n; i++) {
        long fresh = (n - sa[i]) - lcp[i];
        if (k <= fresh) {
            int len = lcp[i] + (int)k;
            memcpy(out, s + sa[i], (size_t)len);
            out[len] = 0;
            return len;
        }
        k -= fresh;
    }
    return -1;
}

int main(void) {
    static char s[64], buf[64];
    static int sa[64], lcp[64];
    const char *fixed[] = {"banana", "aaaa", "abcabc", "mississippi", "a"};
    long expected[] = {15, 4, 15, 53, 1};
    for (int c = 0; c < 5; c++) {
        int n = (int)strlen(fixed[c]);
        build_sa(fixed[c], n, sa);
        build_lcp(fixed[c], n, sa, lcp);
        long d = distinct_from_sa(n, sa, lcp);
        check(d == expected[c], "known distinct count");
        printf("%-12s distinct=%ld\n", fixed[c], d);
    }
    for (int c = 0; c < 6; c++) {
        int n = 20 + (int)(rnd() % 40);
        rand_str(s, n, 2 + c % 3);
        build_sa(s, n, sa);
        build_lcp(s, n, sa, lcp);
        long d = distinct_from_sa(n, sa, lcp);
        check(d == distinct_brute(s, n), "distinct vs brute");
        printf("random n=%d alphabet=%d distinct=%ld of %d\n", n, 2 + c % 3, d, n * (n + 1) / 2);
    }
    /* enumerate mississippi's substrings in order via k-th and verify sortedness */
    const char *m = "mississippi";
    build_sa(m, 11, sa);
    build_lcp(m, 11, sa, lcp);
    char prev[64] = "";
    for (long k = 1; k <= 53; k++) {
        int len = kth_substring(m, 11, sa, lcp, k, buf);
        check(len > 0, "kth exists");
        check(strcmp(prev, buf) < 0, "kth strictly increasing");
        strcpy(prev, buf);
        if (k == 1 || k == 10 || k == 25 || k == 40 || k == 53)
            printf("k=%ld -> %s\n", k, buf);
    }
    check(kth_substring(m, 11, sa, lcp, 54, buf) < 0, "k beyond range");
    return 0;
}
