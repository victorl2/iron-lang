/*
 * title: Shortest common supersequence and merge reconstruction
 * topic: algorithms
 * covers: dynamic programming, LCS relationship, supersequence length identity, traceback merge, three-string generalisation, subsequence validation
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

#define M 20

static int T[M][M];

static int scs_len_table(const char *a, const char *b) {
    int n = (int)strlen(a), m = (int)strlen(b);
    for (int i = 0; i <= n; i++)
        for (int j = 0; j <= m; j++) {
            if (i == 0) T[i][j] = j;
            else if (j == 0) T[i][j] = i;
            else if (a[i - 1] == b[j - 1]) T[i][j] = T[i - 1][j - 1] + 1;
            else T[i][j] = 1 + (T[i - 1][j] < T[i][j - 1] ? T[i - 1][j] : T[i][j - 1]);
        }
    return T[n][m];
}

static void build(const char *a, const char *b, char *out) {
    int i = (int)strlen(a), j = (int)strlen(b), k = T[i][j];
    out[k] = 0;
    while (i > 0 && j > 0) {
        if (a[i - 1] == b[j - 1]) { out[--k] = a[i - 1]; i--; j--; }
        else if (T[i - 1][j] < T[i][j - 1]) { out[--k] = a[i - 1]; i--; }
        else { out[--k] = b[j - 1]; j--; }
    }
    while (i > 0) out[--k] = a[--i];
    while (j > 0) out[--k] = b[--j];
}

static int is_subseq(const char *s, const char *t) {
    while (*t && *s) if (*s == *t++) s++;
    return !*s;
}

static int lcs(const char *a, const char *b) {
    int n = (int)strlen(a), m = (int)strlen(b);
    static int L[M][M];
    memset(L, 0, sizeof L);
    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= m; j++)
            L[i][j] = a[i - 1] == b[j - 1] ? L[i - 1][j - 1] + 1 : (L[i - 1][j] > L[i][j - 1] ? L[i - 1][j] : L[i][j - 1]);
    return L[n][m];
}

/* brute force: BFS over lengths is too big; instead try all strings over the alphabet of increasing length */
static int brute_scs(const char *a, const char *b) {
    int n = (int)strlen(a), m = (int)strlen(b);
    for (int len = (n > m ? n : m);; len++) {
        long total = 1;
        for (int i = 0; i < len; i++) total *= 3;
        for (long x = 0; x < total; x++) {
            char s[16];
            long y = x;
            for (int i = 0; i < len; i++) { s[i] = (char)('a' + y % 3); y /= 3; }
            s[len] = 0;
            if (is_subseq(a, s) && is_subseq(b, s)) return len;
        }
    }
}

int main(void) {
    const char *fixed[][2] = {{"abac", "cab"}, {"geek", "eke"}, {"AGGTAB", "GXTXAYB"}, {"abc", "abc"}, {"abc", "def"}, {"", "xyz"}};
    for (int i = 0; i < 6; i++) {
        int len = scs_len_table(fixed[i][0], fixed[i][1]);
        char out[64];
        build(fixed[i][0], fixed[i][1], out);
        CHECK((int)strlen(out) == len);
        CHECK(is_subseq(fixed[i][0], out) && is_subseq(fixed[i][1], out));
        CHECK(len == (int)(strlen(fixed[i][0]) + strlen(fixed[i][1])) - lcs(fixed[i][0], fixed[i][1]));
        printf("\"%s\" + \"%s\" -> %d \"%s\"\n", fixed[i][0], fixed[i][1], len, out);
    }
    for (int t = 0; t < 6; t++) {
        char a[8], b[8], out[32];
        int n = 2 + rr(4), m = 2 + rr(4);
        for (int i = 0; i < n; i++) a[i] = (char)('a' + rr(3));
        for (int i = 0; i < m; i++) b[i] = (char)('a' + rr(3));
        a[n] = b[m] = 0;
        int len = scs_len_table(a, b);
        build(a, b, out);
        CHECK(is_subseq(a, out) && is_subseq(b, out) && (int)strlen(out) == len);
        CHECK(len == brute_scs(a, b));
        printf("random %d: %s + %s -> %d %s\n", t, a, b, len, out);
    }
    /* three strings via 3-D DP */
    const char *x = "abcbdab", *y = "bdcaba", *z = "bcbad";
    int nx = (int)strlen(x), ny = (int)strlen(y), nz = (int)strlen(z);
    static int D[9][8][7];
    for (int i = 0; i <= nx; i++)
        for (int j = 0; j <= ny; j++)
            for (int k = 0; k <= nz; k++) {
                if (i == 0 && j == 0) { D[i][j][k] = k; continue; }
                if (i == 0 && k == 0) { D[i][j][k] = j; continue; }
                if (j == 0 && k == 0) { D[i][j][k] = i; continue; }
                int best = 1 << 20;
                for (int mask = 1; mask < 8; mask++) {
                    int di = mask & 1, dj = mask >> 1 & 1, dk = mask >> 2 & 1;
                    if (di > i || dj > j || dk > k) continue;
                    char c = 0;
                    int ok = 1;
                    if (di) c = x[i - 1];
                    if (dj) { if (c && c != y[j - 1]) ok = 0; c = y[j - 1]; }
                    if (dk) { if (c && c != z[k - 1]) ok = 0; c = z[k - 1]; }
                    if (ok && D[i - di][j - dj][k - dk] + 1 < best) best = D[i - di][j - dj][k - dk] + 1;
                }
                D[i][j][k] = best;
            }
    printf("three-string SCS of %s, %s, %s: %d\n", x, y, z, D[nx][ny][nz]);
    return 0;
}
