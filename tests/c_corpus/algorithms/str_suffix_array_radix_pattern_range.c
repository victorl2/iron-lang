/*
 * title: Radix suffix array and pattern range queries
 * topic: algorithms
 * covers: counting-sort prefix doubling, suffix array, binary search on prefix range, occurrence counting
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

/* O(n log n) suffix array: prefix doubling with counting sort (radix on pairs). */
static void build_sa_radix(const unsigned char *s, int n, int *sa) {
    int m = n > 256 ? n : 256;
    int *rk = calloc((size_t)n * 2 + 2, sizeof(int));
    int *tmp = calloc((size_t)n * 2 + 2, sizeof(int));
    int *sa2 = malloc(sizeof(int) * (size_t)n);
    int *cnt = calloc((size_t)m + 2, sizeof(int));
    for (int i = 0; i < n; i++)
        rk[i] = s[i] + 1;
    for (int i = 0; i < n; i++)
        cnt[rk[i]]++;
    for (int i = 1; i <= m; i++)
        cnt[i] += cnt[i - 1];
    for (int i = n - 1; i >= 0; i--)
        sa[--cnt[rk[i]]] = i;
    for (int k = 1; k < n; k <<= 1) {
        int p = 0;
        for (int i = n - k; i < n; i++)
            sa2[p++] = i;
        for (int i = 0; i < n; i++)
            if (sa[i] >= k)
                sa2[p++] = sa[i] - k;
        memset(cnt, 0, sizeof(int) * ((size_t)m + 2));
        for (int i = 0; i < n; i++)
            cnt[rk[i]]++;
        for (int i = 1; i <= m; i++)
            cnt[i] += cnt[i - 1];
        for (int i = n - 1; i >= 0; i--)
            sa[--cnt[rk[sa2[i]]]] = sa2[i];
        tmp[sa[0]] = 1;
        int classes = 1;
        for (int i = 1; i < n; i++) {
            int a = sa[i - 1], b = sa[i];
            int same = rk[a] == rk[b] && (a + k < n ? rk[a + k] : 0) == (b + k < n ? rk[b + k] : 0);
            if (!same)
                classes++;
            tmp[b] = classes;
        }
        memcpy(rk, tmp, sizeof(int) * (size_t)n);
        if (classes == n)
            break;
    }
    free(rk);
    free(tmp);
    free(sa2);
    free(cnt);
}

static int cmp_prefix(const unsigned char *s, int n, int pos, const unsigned char *p, int m) {
    int rem = n - pos;
    int l = rem < m ? rem : m;
    int c = memcmp(s + pos, p, (size_t)l);
    if (c != 0)
        return c;
    if (rem < m)
        return -1;
    return 0;
}

/* Returns [lo, hi) range of suffixes starting with p. */
static void sa_range(const unsigned char *s, int n, const int *sa, const unsigned char *p, int m,
                     int *lo_out, int *hi_out, int *probes) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        (*probes)++;
        if (cmp_prefix(s, n, sa[mid], p, m) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    int first = lo;
    hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        (*probes)++;
        if (cmp_prefix(s, n, sa[mid], p, m) <= 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *lo_out = first;
    *hi_out = lo;
}

static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

int main(void) {
    enum { N = 5000 };
    static char text[N + 1];
    static int sa[N], occ[N], expect[N];
    rand_str(text, N, 3);
    build_sa_radix((const unsigned char *)text, N, sa);
    for (int i = 1; i < N; i++)
        check(strcmp(text + sa[i - 1], text + sa[i]) < 0, "sa sorted");
    printf("suffix array built for n=%d, sa[0..4] = %d %d %d %d %d\n", N, sa[0], sa[1], sa[2], sa[3],
           sa[4]);

    const char *pats[] = {"a", "ab", "abc", "cba", "aabbcc", "bcbcbcbc", "abcabcabcabcabc"};
    for (int c = 0; c < 7; c++) {
        int m = (int)strlen(pats[c]), lo, hi, probes = 0;
        sa_range((const unsigned char *)text, N, sa, (const unsigned char *)pats[c], m, &lo, &hi,
                 &probes);
        int cnt = 0;
        for (int i = 0; i + m <= N; i++)
            if (memcmp(text + i, pats[c], (size_t)m) == 0)
                expect[cnt++] = i;
        check(hi - lo == cnt, "range size equals count");
        for (int i = lo; i < hi; i++)
            occ[i - lo] = sa[i];
        qsort(occ, (size_t)cnt, sizeof(int), cmp_int);
        for (int i = 0; i < cnt; i++)
            check(occ[i] == expect[i], "occurrence positions");
        printf("%-16s count=%-5d sa range [%d,%d) probes=%d", pats[c], cnt, lo, hi, probes);
        if (cnt)
            printf(" first=%d", occ[0]);
        printf("\n");
    }
    /* tiny known case */
    const char *b = "banana";
    int sb[6], lo, hi, pr = 0;
    build_sa_radix((const unsigned char *)b, 6, sb);
    sa_range((const unsigned char *)b, 6, sb, (const unsigned char *)"ana", 3, &lo, &hi, &pr);
    printf("banana sa: %d %d %d %d %d %d; \"ana\" range [%d,%d)\n", sb[0], sb[1], sb[2], sb[3],
           sb[4], sb[5], lo, hi);
    check(hi - lo == 2, "banana ana twice");
    return 0;
}
