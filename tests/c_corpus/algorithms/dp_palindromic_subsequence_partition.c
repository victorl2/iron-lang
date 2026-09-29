/*
 * title: Palindromic DP: longest subsequence, minimum insertions, minimum cuts
 * topic: algorithms
 * covers: dynamic programming, interval DP, longest palindromic subsequence, palindromic substring table, minimum palindrome partition cuts, LCS with reverse identity
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 88172645463325252ULL;
static unsigned long long rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static int rr(int n) { return (int)(rnd() % (unsigned long long)n); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); exit(1); } } while (0)

#define M 40

static int lcs_len(const char *a, const char *b, int n) {
    static int t[M][M];
    memset(t, 0, sizeof t);
    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= n; j++)
            t[i][j] = a[i - 1] == b[j - 1] ? t[i - 1][j - 1] + 1 : (t[i - 1][j] > t[i][j - 1] ? t[i - 1][j] : t[i][j - 1]);
    return t[n][n];
}

static int brute_lps(const char *s, int n) {
    int best = 0;
    for (unsigned m = 1; m < (1u << n); m++) {
        char sub[20];
        int k = 0;
        for (int i = 0; i < n; i++) if (m >> i & 1) sub[k++] = s[i];
        if (k <= best) continue;
        int ok = 1;
        for (int i = 0; i < k / 2; i++) if (sub[i] != sub[k - 1 - i]) ok = 0;
        if (ok) best = k;
    }
    return best;
}

int main(void) {
    const char *fixed[] = {"bbbab", "cbbd", "character", "abacdfgdcaba", "a", "abcde", "racecar"};
    char pool[16][M];
    int np = 0;
    for (int i = 0; i < 7; i++) strcpy(pool[np++], fixed[i]);
    for (int i = 0; i < 6; i++) {
        int n = 6 + rr(12);
        for (int k = 0; k < n; k++) pool[np][k] = (char)('a' + rr(3));
        pool[np][n] = 0;
        np++;
    }
    for (int p = 0; p < np; p++) {
        const char *s = pool[p];
        int n = (int)strlen(s);
        int L = 0;
        /* recompute with a clean recurrence */
        {
            static int d[M][M];
            for (int i = n - 1; i >= 0; i--) {
                d[i][i] = 1;
                for (int j = i + 1; j < n; j++) {
                    if (s[i] == s[j]) d[i][j] = (j - i == 1 ? 0 : d[i + 1][j - 1]) + 2;
                    else d[i][j] = d[i + 1][j] > d[i][j - 1] ? d[i + 1][j] : d[i][j - 1];
                }
            }
            L = d[0][n - 1];
        }
        char rev[M];
        for (int i = 0; i < n; i++) rev[i] = s[n - 1 - i];
        rev[n] = 0;
        CHECK(L == lcs_len(s, rev, n));
        if (n <= 18) CHECK(L == brute_lps(s, n));
        /* palindromic substring table and min cuts */
        static char pal[M][M];
        for (int i = n - 1; i >= 0; i--)
            for (int j = i; j < n; j++) pal[i][j] = s[i] == s[j] && (j - i < 2 || pal[i + 1][j - 1]);
        int cuts[M];
        long substrings = 0;
        for (int i = 0; i < n; i++) for (int j = i; j < n; j++) substrings += pal[i][j];
        for (int j = 0; j < n; j++) {
            cuts[j] = j;
            for (int i = 0; i <= j; i++)
                if (pal[i][j]) {
                    int c = i == 0 ? 0 : cuts[i - 1] + 1;
                    if (c < cuts[j]) cuts[j] = c;
                }
        }
        printf("%-14s lps=%2d insertions=%2d palin_substrings=%2ld min_cuts=%d\n", s, L, n - L, substrings, cuts[n - 1]);
    }
    return 0;
}
