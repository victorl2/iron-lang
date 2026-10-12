/*
 * title: Horspool and Sunday search
 * topic: algorithms
 * covers: Horspool, Sunday quick search, function pointers, shift tables, comparison counts
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

typedef int (*Searcher)(const unsigned char *, int, const unsigned char *, int, int *, long *);

static int horspool(const unsigned char *t, int n, const unsigned char *p, int m, int *out,
                    long *cmps) {
    int shift[256];
    for (int i = 0; i < 256; i++)
        shift[i] = m;
    for (int i = 0; i + 1 < m; i++)
        shift[p[i]] = m - 1 - i;
    int c = 0, s = 0;
    while (s + m <= n) {
        int j = m - 1;
        while (j >= 0 && (++*cmps, t[s + j] == p[j]))
            j--;
        if (j < 0)
            out[c++] = s;
        s += shift[t[s + m - 1]];
    }
    return c;
}

/* Sunday quick search: shift by the character just after the window. */
static int sunday(const unsigned char *t, int n, const unsigned char *p, int m, int *out,
                  long *cmps) {
    int shift[256];
    for (int i = 0; i < 256; i++)
        shift[i] = m + 1;
    for (int i = 0; i < m; i++)
        shift[p[i]] = m - i;
    int c = 0, s = 0;
    while (s + m <= n) {
        int j = 0;
        while (j < m && (++*cmps, t[s + j] == p[j]))
            j++;
        if (j == m)
            out[c++] = s;
        if (s + m >= n)
            break;
        s += shift[t[s + m]];
    }
    return c;
}

static int brute(const unsigned char *t, int n, const unsigned char *p, int m, int *out,
                 long *cmps) {
    int c = 0;
    for (int s = 0; s + m <= n; s++) {
        int j = 0;
        while (j < m && (++*cmps, t[s + j] == p[j]))
            j++;
        if (j == m)
            out[c++] = s;
    }
    return c;
}

int main(void) {
    enum { N = 30000 };
    static unsigned char t[N + 1];
    static int r[3][N];
    Searcher fn[3] = {horspool, sunday, brute};
    const char *names[3] = {"horspool", "sunday", "brute"};
    const char *pats[] = {"abcdefghij", "aaaaab", "abracadabra", "zzzz", "the", "ab"};
    int alph[] = {26, 2, 26, 26, 8, 2};
    for (int c = 0; c < 6; c++) {
        rand_str((char *)t, N, alph[c]);
        int m = (int)strlen(pats[c]);
        if (c == 2 || c == 4)
            for (int k = 100; k + m < N; k += 3777)
                memcpy(t + k, pats[c], (size_t)m);
        int cnt[3];
        long cm[3] = {0, 0, 0};
        for (int k = 0; k < 3; k++)
            cnt[k] = fn[k](t, N, (const unsigned char *)pats[c], m, r[k], &cm[k]);
        check(cnt[0] == cnt[2] && cnt[1] == cnt[2], "counts agree");
        for (int i = 0; i < cnt[2]; i++)
            check(r[0][i] == r[2][i] && r[1][i] == r[2][i], "positions agree");
        printf("%-12s hits=%-5d", pats[c], cnt[2]);
        for (int k = 0; k < 3; k++)
            printf(" %s=%ld", names[k], cm[k]);
        printf("\n");
    }
    return 0;
}
