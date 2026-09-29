/*
 * title: KMP prefix function search
 * topic: algorithms
 * covers: prefix function, KMP, overlapping matches, brute-force cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 0x9e3779b97f4a7c15ULL;

static unsigned rnd(void) {
    unsigned long long z = (rng_s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return (unsigned)((z ^ (z >> 31)) >> 16);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void rand_str(char *s, int n, int alpha) {
    for (int i = 0; i < n; i++)
        s[i] = (char)('a' + rnd() % (unsigned)alpha);
    s[n] = 0;
}

static int brute_find_all(const char *t, const char *p, int *out) {
    int n = (int)strlen(t), m = (int)strlen(p), c = 0;
    for (int i = 0; i + m <= n; i++)
        if (memcmp(t + i, p, (size_t)m) == 0)
            out[c++] = i;
    return c;
}

static void prefix_function(const char *p, int m, int *pi) {
    pi[0] = 0;
    for (int i = 1; i < m; i++) {
        int k = pi[i - 1];
        while (k > 0 && p[i] != p[k])
            k = pi[k - 1];
        if (p[i] == p[k])
            k++;
        pi[i] = k;
    }
}

static int kmp_all(const char *t, const char *p, int *out, long *steps) {
    int n = (int)strlen(t), m = (int)strlen(p), c = 0, k = 0;
    int *pi = malloc(sizeof(int) * (size_t)m);
    prefix_function(p, m, pi);
    for (int i = 0; i < n; i++) {
        while (k > 0 && (k == m || t[i] != p[k])) {
            k = pi[k - 1];
            (*steps)++;
        }
        if (t[i] == p[k])
            k++;
        (*steps)++;
        if (k == m)
            out[c++] = i - m + 1;
    }
    free(pi);
    return c;
}

int main(void) {
    int pi[16];
    const char *demo = "abacabab";
    prefix_function(demo, 8, pi);
    printf("pi(%s):", demo);
    for (int i = 0; i < 8; i++)
        printf(" %d", pi[i]);
    printf("\n");
    int exp_pi[8] = {0, 0, 1, 0, 1, 2, 3, 2};
    for (int i = 0; i < 8; i++)
        check(pi[i] == exp_pi[i], "pi known");

    static char t[4001];
    static int a[4001], b[4001];
    const char *pats[] = {"aa", "abab", "abaab", "aaaaaa", "b", "abcabca"};
    int alphas[] = {2, 2, 3, 1, 4, 3};
    for (int c = 0; c < 6; c++) {
        rand_str(t, 4000, alphas[c]);
        long steps = 0;
        int na = kmp_all(t, pats[c], a, &steps);
        int nb = brute_find_all(t, pats[c], b);
        check(na == nb, "count");
        for (int i = 0; i < na; i++)
            check(a[i] == b[i], "position");
        check(steps <= 2L * 4000, "linear bound");
        printf("pattern %-8s alphabet %d: %4d matches", pats[c], alphas[c], na);
        if (na > 0)
            printf(", first at %d", a[0]);
        printf("\n");
    }
    /* overlapping matches in a periodic text */
    memset(t, 'a', 50);
    t[50] = 0;
    long steps = 0;
    int n = kmp_all(t, "aaaa", a, &steps);
    printf("aaaa in a^50: %d overlapping matches, %ld steps\n", n, steps);
    check(n == 47, "overlap count");
    return 0;
}
