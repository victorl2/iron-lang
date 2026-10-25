/*
 * title: Edit distance with operation script and transpositions
 * topic: algorithms
 * covers: dynamic programming, Levenshtein, optimal string alignment, traceback of edit operations, weighted costs, symmetry checks
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

#define M 32
static int D[M][M];

static int lev(const char *a, const char *b, int cs, int ci, int cd, int transpose) {
    int n = (int)strlen(a), m = (int)strlen(b);
    for (int i = 0; i <= n; i++) D[i][0] = i * cd;
    for (int j = 0; j <= m; j++) D[0][j] = j * ci;
    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= m; j++) {
            int best = D[i - 1][j - 1] + (a[i - 1] == b[j - 1] ? 0 : cs);
            if (D[i - 1][j] + cd < best) best = D[i - 1][j] + cd;
            if (D[i][j - 1] + ci < best) best = D[i][j - 1] + ci;
            if (transpose && i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && D[i - 2][j - 2] + 1 < best)
                best = D[i - 2][j - 2] + 1;
            D[i][j] = best;
        }
    return D[n][m];
}

/* rebuild the plain unit-cost script from the table */
static void script(const char *a, const char *b, char *ops) {
    int i = (int)strlen(a), j = (int)strlen(b), k = 0;
    char tmp[128];
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && a[i - 1] == b[j - 1] && D[i][j] == D[i - 1][j - 1]) { tmp[k++] = '='; i--; j--; }
        else if (i > 0 && j > 0 && D[i][j] == D[i - 1][j - 1] + 1) { tmp[k++] = 'S'; i--; j--; }
        else if (i > 0 && D[i][j] == D[i - 1][j] + 1) { tmp[k++] = 'D'; i--; }
        else { tmp[k++] = 'I'; j--; }
    }
    for (int x = 0; x < k; x++) ops[x] = tmp[k - 1 - x];
    ops[k] = 0;
}

/* apply script to a to reproduce b */
static void apply(const char *a, const char *b, const char *ops, char *out) {
    int i = 0, j = 0, k = 0;
    for (const char *p = ops; *p; p++) {
        if (*p == '=') { out[k++] = a[i]; i++; j++; }
        else if (*p == 'S') { out[k++] = b[j]; i++; j++; }
        else if (*p == 'D') i++;
        else { out[k++] = b[j]; j++; }
    }
    out[k] = 0;
}

static int rec(const char *a, const char *b) {
    if (!*a) return (int)strlen(b);
    if (!*b) return (int)strlen(a);
    int x = rec(a + 1, b + 1) + (*a != *b);
    int y = rec(a + 1, b) + 1, z = rec(a, b + 1) + 1;
    if (y < x) x = y;
    if (z < x) x = z;
    return x;
}

int main(void) {
    const char *w[][2] = {{"kitten", "sitting"}, {"flaw", "lawn"}, {"intention", "execution"}, {"ca", "abc"},
                          {"", "abc"}, {"same", "same"}, {"sunday", "saturday"}};
    for (int p = 0; p < 7; p++) {
        int d = lev(w[p][0], w[p][1], 1, 1, 1, 0);
        int osa = lev(w[p][0], w[p][1], 1, 1, 1, 1);
        char ops[128], out[128];
        lev(w[p][0], w[p][1], 1, 1, 1, 0);
        script(w[p][0], w[p][1], ops);
        apply(w[p][0], w[p][1], ops, out);
        CHECK(strcmp(out, w[p][1]) == 0);
        int cost = 0;
        for (char *q = ops; *q; q++) cost += *q != '=';
        CHECK(cost == d);
        CHECK(lev(w[p][1], w[p][0], 1, 1, 1, 0) == d);
        if (strlen(w[p][0]) < 10 && strlen(w[p][1]) < 10) CHECK(rec(w[p][0], w[p][1]) == d);
        printf("%-9s -> %-9s lev=%d osa=%d ops=%s\n", w[p][0], w[p][1], d, osa, ops);
    }
    for (int t = 0; t < 6; t++) {
        char a[10], b[10];
        int n = 1 + rr(8), m = 1 + rr(8);
        for (int i = 0; i < n; i++) a[i] = (char)('a' + rr(3));
        for (int i = 0; i < m; i++) b[i] = (char)('a' + rr(3));
        a[n] = b[m] = 0;
        int d = lev(a, b, 1, 1, 1, 0);
        CHECK(d == rec(a, b));
        int wd = lev(a, b, 3, 2, 2, 0); /* substitution costs more than delete+insert of 2+2? no: 3 < 4 */
        printf("random %d: %s %s lev=%d weighted(3,2,2)=%d\n", t, a, b, d, wd);
    }
    return 0;
}
