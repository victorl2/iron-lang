/*
 * title: Longest common subsequence with traceback
 * topic: algorithms
 * covers: dynamic programming, LCS table, traceback, counting distinct LCS strings, memoized recursion cross-check
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

#define M 24
static int memo[M][M];
static const char *A, *B;

static int lcs_rec(int i, int j) {
    if (i == 0 || j == 0) return 0;
    if (memo[i][j] >= 0) return memo[i][j];
    int r;
    if (A[i - 1] == B[j - 1]) r = 1 + lcs_rec(i - 1, j - 1);
    else { int x = lcs_rec(i - 1, j), y = lcs_rec(i, j - 1); r = x > y ? x : y; }
    return memo[i][j] = r;
}

/* collect all distinct LCS strings via recursion over the table */
static char found[64][M];
static int nfound;
static char cur[M];
static int T[M][M];

static void collect(int i, int j, int pos) {
    if (T[i][j] == 0) {
        for (int k = 0; k < nfound; k++) if (strcmp(found[k], cur + pos) == 0) return;
        strcpy(found[nfound++], cur + pos);
        return;
    }
    if (A[i - 1] == B[j - 1]) {
        cur[pos - 1] = A[i - 1];
        collect(i - 1, j - 1, pos - 1);
    } else {
        if (T[i - 1][j] == T[i][j]) collect(i - 1, j, pos);
        if (T[i][j - 1] == T[i][j]) collect(i, j - 1, pos);
    }
}

static int cmpstr(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int main(void) {
    const char *pairs[][2] = {
        {"AGGTAB", "GXTXAYB"}, {"ABCBDAB", "BDCABA"}, {"XMJYAUZ", "MZJAWXU"},
        {"AAAA", "AA"}, {"ABC", "DEF"}, {"", "ABC"}};
    for (int p = 0; p < 6; p++) {
        A = pairs[p][0]; B = pairs[p][1];
        int n = (int)strlen(A), m = (int)strlen(B);
        for (int i = 0; i <= n; i++)
            for (int j = 0; j <= m; j++) {
                memo[i][j] = -1;
                if (i == 0 || j == 0) T[i][j] = 0;
                else if (A[i - 1] == B[j - 1]) T[i][j] = T[i - 1][j - 1] + 1;
                else T[i][j] = T[i - 1][j] > T[i][j - 1] ? T[i - 1][j] : T[i][j - 1];
            }
        CHECK(T[n][m] == lcs_rec(n, m));
        nfound = 0;
        memset(cur, 0, sizeof cur);
        collect(n, m, T[n][m]);
        qsort(found, (size_t)nfound, M, cmpstr);
        printf("\"%s\" vs \"%s\": len=%d distinct=%d [", A, B, T[n][m], nfound);
        for (int k = 0; k < nfound; k++) printf("%s%s", k ? "," : "", found[k]);
        printf("]\n");
    }
    /* random strings: compare table and memo */
    for (int t = 0; t < 5; t++) {
        char sa[M], sb[M];
        int n = 5 + rr(15), m = 5 + rr(15);
        for (int i = 0; i < n; i++) sa[i] = (char)('a' + rr(3));
        for (int i = 0; i < m; i++) sb[i] = (char)('a' + rr(3));
        sa[n] = sb[m] = 0;
        A = sa; B = sb;
        for (int i = 0; i <= n; i++)
            for (int j = 0; j <= m; j++) {
                memo[i][j] = -1;
                if (i == 0 || j == 0) T[i][j] = 0;
                else if (A[i - 1] == B[j - 1]) T[i][j] = T[i - 1][j - 1] + 1;
                else T[i][j] = T[i - 1][j] > T[i][j - 1] ? T[i - 1][j] : T[i][j - 1];
            }
        CHECK(T[n][m] == lcs_rec(n, m));
        printf("random %d: %s %s -> %d\n", t, sa, sb, T[n][m]);
    }
    return 0;
}
