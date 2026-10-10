/*
 * title: Hirschberg linear-space LCS
 * topic: algorithms
 * covers: dynamic programming, divide and conquer, Hirschberg, forward and reverse score rows, O(min) memory, table LCS cross-check
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

static void score_row(const char *a, int n, const char *b, int m, int rev, int *out) {
    int *prev = calloc((size_t)m + 1, sizeof(int)), *cur = calloc((size_t)m + 1, sizeof(int));
    CHECK(prev && cur);
    for (int i = 0; i < n; i++) {
        char ca = rev ? a[n - 1 - i] : a[i];
        cur[0] = 0;
        for (int j = 0; j < m; j++) {
            char cb = rev ? b[m - 1 - j] : b[j];
            if (ca == cb) cur[j + 1] = prev[j] + 1;
            else cur[j + 1] = prev[j + 1] > cur[j] ? prev[j + 1] : cur[j];
        }
        int *t = prev; prev = cur; cur = t;
    }
    memcpy(out, prev, ((size_t)m + 1) * sizeof(int));
    free(prev); free(cur);
}

static int calls;

static int hirsch(const char *a, int n, const char *b, int m, char *out) {
    calls++;
    if (n == 0 || m == 0) return 0;
    if (n == 1) {
        for (int j = 0; j < m; j++) if (b[j] == a[0]) { out[0] = a[0]; return 1; }
        return 0;
    }
    int mid = n / 2;
    int *l = malloc(((size_t)m + 1) * sizeof(int)), *r = malloc(((size_t)m + 1) * sizeof(int));
    CHECK(l && r);
    score_row(a, mid, b, m, 0, l);
    score_row(a + mid, n - mid, b, m, 1, r);
    int bestj = 0, bestv = -1;
    for (int j = 0; j <= m; j++)
        if (l[j] + r[m - j] > bestv) { bestv = l[j] + r[m - j]; bestj = j; }
    free(l); free(r);
    int x = hirsch(a, mid, b, bestj, out);
    int y = hirsch(a + mid, n - mid, b + bestj, m - bestj, out + x);
    return x + y;
}

static int table_lcs(const char *a, int n, const char *b, int m) {
    int (*T)[80] = calloc((size_t)(n + 1), sizeof *T);
    CHECK(T);
    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= m; j++)
            T[i][j] = a[i - 1] == b[j - 1] ? T[i - 1][j - 1] + 1 : (T[i - 1][j] > T[i][j - 1] ? T[i - 1][j] : T[i][j - 1]);
    int r = T[n][m];
    free(T);
    return r;
}

static int is_subseq(const char *s, int k, const char *t, int n) {
    int p = 0;
    for (int i = 0; i < n && p < k; i++) if (t[i] == s[p]) p++;
    return p == k;
}

int main(void) {
    for (int t = 0; t < 8; t++) {
        char a[80], b[80], out[80];
        int n = 8 + rr(60), m = 8 + rr(60), alpha = 2 + rr(4);
        for (int i = 0; i < n; i++) a[i] = (char)('a' + rr(alpha));
        for (int i = 0; i < m; i++) b[i] = (char)('a' + rr(alpha));
        a[n] = b[m] = 0;
        calls = 0;
        memset(out, 0, sizeof out);
        int k = hirsch(a, n, b, m, out);
        out[k] = 0;
        CHECK(k == table_lcs(a, n, b, m));
        CHECK(is_subseq(out, k, a, n) && is_subseq(out, k, b, m));
        printf("case %d: n=%d m=%d alpha=%d lcs=%d calls=%d\n", t, n, m, alpha, k, calls);
    }
    return 0;
}
