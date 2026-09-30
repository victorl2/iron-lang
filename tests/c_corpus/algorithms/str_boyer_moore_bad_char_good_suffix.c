/*
 * title: Boyer-Moore with both shift rules
 * topic: algorithms
 * covers: bad character rule, strong good suffix rule, comparison counting, ablation of rules
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

#define ALPHA 256

static void bad_char_table(const unsigned char *p, int m, int *bc) {
    for (int i = 0; i < ALPHA; i++)
        bc[i] = -1;
    for (int i = 0; i < m; i++)
        bc[p[i]] = i;
}

/* Good suffix preprocessing, strong rule (classic two-case border computation). */
static void good_suffix_table(const unsigned char *p, int m, int *shift) {
    int *border = malloc(sizeof(int) * (size_t)(m + 1));
    int i = m, j = m + 1;
    border[i] = j;
    for (int k = 0; k <= m; k++)
        shift[k] = 0;
    while (i > 0) {
        while (j <= m && p[i - 1] != p[j - 1]) {
            if (shift[j] == 0)
                shift[j] = j - i;
            j = border[j];
        }
        i--;
        j--;
        border[i] = j;
    }
    j = border[0];
    for (i = 0; i <= m; i++) {
        if (shift[i] == 0)
            shift[i] = j;
        if (i == j)
            j = border[j];
    }
    free(border);
}

static int bm_search(const unsigned char *t, int n, const unsigned char *p, int m, int *out,
                     long *cmps, int use_bc, int use_gs) {
    int bc[ALPHA];
    int *gs = malloc(sizeof(int) * (size_t)(m + 1));
    bad_char_table(p, m, bc);
    good_suffix_table(p, m, gs);
    int c = 0, s = 0;
    while (s <= n - m) {
        int j = m - 1;
        while (j >= 0) {
            (*cmps)++;
            if (p[j] != t[s + j])
                break;
            j--;
        }
        if (j < 0) {
            out[c++] = s;
            s += use_gs ? gs[0] : 1;
        } else {
            int a = use_bc ? j - bc[t[s + j]] : 1;
            int b = use_gs ? gs[j + 1] : 1;
            s += a > b ? a : b;
            if (a < 1 && b < 1)
                s++;
        }
    }
    free(gs);
    return c;
}

static int naive(const unsigned char *t, int n, const unsigned char *p, int m, int *out) {
    int c = 0;
    for (int s = 0; s + m <= n; s++)
        if (memcmp(t + s, p, (size_t)m) == 0)
            out[c++] = s;
    return c;
}

int main(void) {
    enum { N = 20000 };
    static unsigned char t[N + 1];
    static int a[N], b[N];
    const char *pats[] = {"abcabc", "aabaa", "dcbadcba", "eeee", "abcdefgh", "baab"};
    int alph[] = {3, 2, 4, 5, 26, 2};
    printf("%-9s %5s %8s %8s %8s %8s\n", "pattern", "hits", "bc+gs", "bc only", "gs only", "naive");
    for (int c = 0; c < 6; c++) {
        rand_str((char *)t, N, alph[c]);
        const unsigned char *p = (const unsigned char *)pats[c];
        int m = (int)strlen(pats[c]);
        long cm[4] = {0, 0, 0, 0};
        int nb = naive(t, N, p, m, b);
        int n0 = bm_search(t, N, p, m, a, &cm[0], 1, 1);
        check(n0 == nb, "bm both count");
        for (int i = 0; i < nb; i++)
            check(a[i] == b[i], "bm both pos");
        int n1 = bm_search(t, N, p, m, a, &cm[1], 1, 0);
        check(n1 == nb, "bm bc count");
        int n2 = bm_search(t, N, p, m, a, &cm[2], 0, 1);
        check(n2 == nb, "bm gs count");
        cm[3] = 0;
        int n3 = bm_search(t, N, p, m, a, &cm[3], 0, 0);
        check(n3 == nb, "plain count");
        printf("%-9s %5d %8ld %8ld %8ld %8ld\n", pats[c], nb, cm[0], cm[1], cm[2], cm[3]);
    }
    /* known good-suffix shifts for "abbabab" */
    int gs[8];
    good_suffix_table((const unsigned char *)"abbabab", 7, gs);
    printf("gs(abbabab):");
    for (int i = 0; i <= 7; i++)
        printf(" %d", gs[i]);
    printf("\n");
    return 0;
}
