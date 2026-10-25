/*
 * title: Cyclic rotation matching and classes
 * topic: algorithms
 * covers: rotation test via KMP on doubled string, distinct rotation count, canonical form, sorted rotations, necklace classes
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

/* Cyclic rotation toolkit: rotation test via KMP on s+s, number of distinct rotations,
 * canonical rotation classes, and the sorted order of all rotations obtained from a suffix
 * array of the doubled string (with ties resolved by index). */
static void prefix_function(const char *s, int n, int *pi) {
    pi[0] = 0;
    for (int i = 1; i < n; i++) {
        int k = pi[i - 1];
        while (k > 0 && s[i] != s[k])
            k = pi[k - 1];
        pi[i] = s[i] == s[k] ? k + 1 : 0;
    }
}

/* is b a rotation of a? search b inside a+a with KMP; returns shift or -1 */
static int rotation_shift(const char *a, const char *b) {
    int n = (int)strlen(a);
    if ((int)strlen(b) != n)
        return -1;
    char *dbl = malloc((size_t)(2 * n + 1));
    int *pi = malloc(sizeof(int) * (size_t)(n + 1));
    memcpy(dbl, a, (size_t)n);
    memcpy(dbl + n, a, (size_t)n);
    dbl[2 * n] = 0;
    prefix_function(b, n, pi);
    int k = 0, ans = -1;
    for (int i = 0; i < 2 * n - 1 && ans < 0; i++) {
        while (k > 0 && (k == n || dbl[i] != b[k]))
            k = pi[k - 1];
        if (dbl[i] == b[k])
            k++;
        if (k == n)
            ans = i - n + 1;
    }
    free(dbl);
    free(pi);
    return ans;
}

static int distinct_rotations(const char *s) {
    int n = (int)strlen(s), pi[128];
    prefix_function(s, n, pi);
    int p = n - pi[n - 1];
    return n % p == 0 ? p : n;
}

static const char *g_dbl;
static int g_len;

static int cmp_rot(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    int c = strncmp(g_dbl + a, g_dbl + b, (size_t)g_len);
    return c ? c : a - b;
}

static void sorted_rotations(const char *s, int *order) {
    static char dbl[256];
    int n = (int)strlen(s);
    memcpy(dbl, s, (size_t)n);
    memcpy(dbl + n, s, (size_t)n);
    dbl[2 * n] = 0;
    g_dbl = dbl;
    g_len = n;
    for (int i = 0; i < n; i++)
        order[i] = i;
    qsort(order, (size_t)n, sizeof(int), cmp_rot);
}

static void canonical(const char *s, char *out) {
    int n = (int)strlen(s), order[128];
    sorted_rotations(s, order);
    for (int k = 0; k < n; k++)
        out[k] = s[(order[0] + k) % n];
    out[n] = 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(void) {
    const char *a = "abcdefg";
    const char *tests[] = {"cdefgab", "abcdefg", "gabcdef", "abcdegf", "abcdef"};
    for (int i = 0; i < 5; i++)
        printf("%s vs %s: shift %d\n", a, tests[i], rotation_shift(a, tests[i]));
    check(rotation_shift(a, "cdefgab") == 2 && rotation_shift(a, "gabcdef") == 6, "known shifts");
    check(rotation_shift(a, "abcdegf") < 0 && rotation_shift(a, "abcdef") < 0, "non rotations");

    const char *words[] = {"abab", "abcabc", "aaaa", "abcd", "abaabaab"};
    for (int i = 0; i < 5; i++) {
        int d = distinct_rotations(words[i]);
        /* brute force: collect the rotations and count unique ones */
        int n = (int)strlen(words[i]), uniq = 0;
        char rot[16][16];
        for (int r = 0; r < n; r++) {
            for (int k = 0; k < n; k++)
                rot[r][k] = words[i][(r + k) % n];
            rot[r][n] = 0;
            int seen = 0;
            for (int q = 0; q < r; q++)
                seen |= strcmp(rot[q], rot[r]) == 0;
            uniq += !seen;
        }
        check(uniq == d, "distinct rotations");
        int order[16];
        sorted_rotations(words[i], order);
        printf("%-9s distinct rotations=%d smallest at %d, largest at %d\n", words[i], d, order[0],
               order[n - 1]);
    }
    /* group random words into rotation classes via canonical form, compare to pairwise KMP test */
    enum { W = 120 };
    static char w[W][10], canon[W][10];
    for (int i = 0; i < W; i++) {
        rand_str(w[i], 6, 2);
        canonical(w[i], canon[i]);
    }
    int agree = 0;
    for (int i = 0; i < W; i++)
        for (int j = i + 1; j < W; j++) {
            int same = strcmp(canon[i], canon[j]) == 0;
            check(same == (rotation_shift(w[i], w[j]) >= 0), "canonical vs KMP");
            agree++;
        }
    const char *ptrs[W];
    for (int i = 0; i < W; i++)
        ptrs[i] = canon[i];
    qsort(ptrs, W, sizeof(char *), cmp_str);
    int classes = 1;
    for (int i = 1; i < W; i++)
        classes += strcmp(ptrs[i], ptrs[i - 1]) != 0;
    printf("%d random binary words of length 6: %d rotation classes (%d pairs verified)\n", W, classes, agree);
    check(classes <= 14, "at most 14 binary necklaces of length 6");
    return 0;
}
