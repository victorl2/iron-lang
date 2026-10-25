/*
 * title: Z-function search and period
 * topic: algorithms
 * covers: Z array, pattern search via concatenation, smallest period, naive cross-check
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

static inline int brute_find_all(const char *t, const char *p, int *out) {
    int n = (int)strlen(t), m = (int)strlen(p), c = 0;
    for (int i = 0; i + m <= n; i++)
        if (memcmp(t + i, p, (size_t)m) == 0)
            out[c++] = i;
    return c;
}

static void z_function(const char *s, int n, int *z) {
    z[0] = n;
    int l = 0, r = 0;
    for (int i = 1; i < n; i++) {
        z[i] = 0;
        if (i < r) {
            int k = z[i - l];
            z[i] = k < r - i ? k : r - i;
        }
        while (i + z[i] < n && s[z[i]] == s[i + z[i]])
            z[i]++;
        if (i + z[i] > r) {
            l = i;
            r = i + z[i];
        }
    }
}

static void z_naive(const char *s, int n, int *z) {
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (i + k < n && s[k] == s[i + k])
            k++;
        z[i] = k;
    }
}

/* Search via z of pattern + '#' + text. */
static int z_search(const char *t, const char *p, int *out) {
    int n = (int)strlen(t), m = (int)strlen(p);
    char *cat = malloc((size_t)(n + m + 2));
    int *z = malloc(sizeof(int) * (size_t)(n + m + 1));
    memcpy(cat, p, (size_t)m);
    cat[m] = '#';
    memcpy(cat + m + 1, t, (size_t)n + 1);
    z_function(cat, n + m + 1, z);
    int c = 0;
    for (int i = m + 1; i < n + m + 1; i++)
        if (z[i] >= m)
            out[c++] = i - m - 1;
    free(cat);
    free(z);
    return c;
}

int main(void) {
    static char s[3001];
    static int z[3001], zn[3001];
    int total_mismatch = 0;
    for (int round = 0; round < 12; round++) {
        int n = 50 + (int)(rnd() % 2900);
        rand_str(s, n, 1 + round % 3);
        z_function(s, n, z);
        z_naive(s, n, zn);
        for (int i = 0; i < n; i++)
            if (z[i] != zn[i])
                total_mismatch++;
    }
    check(total_mismatch == 0, "z matches naive");
    printf("z vs naive over 12 random strings: %d mismatches\n", total_mismatch);

    const char *demo = "aabxaabxcaabxaabxay";
    int n = (int)strlen(demo);
    z_function(demo, n, z);
    printf("z(%s):", demo);
    for (int i = 0; i < n; i++)
        printf(" %d", z[i]);
    printf("\n");

    static int a[3001], b[3001];
    rand_str(s, 3000, 2);
    const char *pats[] = {"abba", "aaa", "babab"};
    for (int c = 0; c < 3; c++) {
        int na = z_search(s, pats[c], a);
        int nb = brute_find_all(s, pats[c], b);
        check(na == nb, "search count");
        for (int i = 0; i < na; i++)
            check(a[i] == b[i], "search pos");
        printf("z_search %-6s -> %d hits\n", pats[c], na);
    }

    /* Smallest period from z: first p with p + z[p] == n. */
    const char *words[] = {"abcabcabc", "abcabcab", "aaaa", "abcd", "abaababaab"};
    for (int c = 0; c < 5; c++) {
        int len = (int)strlen(words[c]);
        z_function(words[c], len, z);
        int p = len;
        for (int i = 1; i < len; i++)
            if (i + z[i] == len) {
                p = i;
                break;
            }
        printf("period(%s) = %d\n", words[c], p);
    }
    return 0;
}
