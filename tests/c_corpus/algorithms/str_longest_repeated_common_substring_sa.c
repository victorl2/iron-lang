/*
 * title: Longest repeated and common substring with suffix array
 * topic: algorithms
 * covers: suffix array, LCP, longest repeated substring, longest common substring of two strings, sentinel
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

/* Longest repeated substring and longest common substring via SA + LCP. */
static int longest_repeated(const char *s, int n, const int *sa, const int *lcp, int *pos) {
    int best = 0;
    *pos = 0;
    for (int i = 1; i < n; i++)
        if (lcp[i] > best) {
            best = lcp[i];
            *pos = sa[i];
        }
    return best;
}

static int longest_common(const char *a, const char *b, int *pa, int *pb) {
    int na = (int)strlen(a), nb = (int)strlen(b), n = na + nb + 1;
    char *s = malloc((size_t)n + 1);
    memcpy(s, a, (size_t)na);
    s[na] = '{';
    memcpy(s + na + 1, b, (size_t)nb + 1);
    int *sa = malloc(sizeof(int) * (size_t)n);
    int *lcp = malloc(sizeof(int) * (size_t)n);
    build_sa(s, n, sa);
    build_lcp(s, n, sa, lcp);
    int best = 0;
    *pa = *pb = -1;
    for (int i = 1; i < n; i++) {
        int x = sa[i - 1], y = sa[i];
        int xa = x < na, ya = y < na;
        if (xa == ya || x == na || y == na)
            continue;
        if (lcp[i] > best) {
            int lo = xa ? x : y, hi = xa ? y : x;
            best = lcp[i];
            *pa = lo;
            *pb = hi - na - 1;
        }
    }
    free(s);
    free(sa);
    free(lcp);
    return best;
}

static int lcs_brute(const char *a, const char *b) {
    int na = (int)strlen(a), nb = (int)strlen(b), best = 0;
    for (int i = 0; i < na; i++)
        for (int j = 0; j < nb; j++) {
            int k = 0;
            while (i + k < na && j + k < nb && a[i + k] == b[j + k])
                k++;
            if (k > best)
                best = k;
        }
    return best;
}

static int lrs_brute(const char *s, int n) {
    int best = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            int k = 0;
            while (j + k < n && s[i + k] == s[j + k])
                k++;
            if (k > best)
                best = k;
        }
    return best;
}

int main(void) {
    static char s[300], a[200], b[200];
    static int sa[300], lcp[300];
    const char *texts[] = {"banana", "abcdefg", "aaaaa", "to be or not to be, that is"};
    for (int c = 0; c < 4; c++) {
        int n = (int)strlen(texts[c]), pos;
        build_sa(texts[c], n, sa);
        build_lcp(texts[c], n, sa, lcp);
        int len = longest_repeated(texts[c], n, sa, lcp, &pos);
        check(len == lrs_brute(texts[c], n), "lrs known");
        printf("lrs(\"%s\") = %d", texts[c], len);
        if (len > 0)
            printf(" \"%.*s\"", len, texts[c] + pos);
        printf("\n");
    }
    for (int c = 0; c < 8; c++) {
        int n = 100 + (int)(rnd() % 150);
        rand_str(s, n, 2 + c % 3);
        build_sa(s, n, sa);
        build_lcp(s, n, sa, lcp);
        int pos;
        int len = longest_repeated(s, n, sa, lcp, &pos);
        check(len == lrs_brute(s, n), "lrs random");
        printf("random lrs n=%d alpha=%d -> %d\n", n, 2 + c % 3, len);
    }
    struct {
        const char *x, *y;
    } pairs[] = {{"xabcdy", "zabcdw"}, {"abc", "def"}, {"GeeksforGeeks", "GeeksQuiz"}};
    for (int c = 0; c < 3; c++) {
        int pa, pb;
        int len = longest_common(pairs[c].x, pairs[c].y, &pa, &pb);
        check(len == lcs_brute(pairs[c].x, pairs[c].y), "lcsubstr known");
        printf("common(%s, %s) = %d", pairs[c].x, pairs[c].y, len);
        if (len > 0) {
            check(memcmp(pairs[c].x + pa, pairs[c].y + pb, (size_t)len) == 0, "common witness");
            printf(" \"%.*s\" at %d/%d", len, pairs[c].x + pa, pa, pb);
        }
        printf("\n");
    }
    for (int c = 0; c < 8; c++) {
        rand_str(a, 60 + (int)(rnd() % 100), 3);
        rand_str(b, 60 + (int)(rnd() % 100), 3);
        int pa, pb;
        int len = longest_common(a, b, &pa, &pb);
        check(len == lcs_brute(a, b), "lcsubstr random");
        printf("random common %d\n", len);
    }
    return 0;
}
