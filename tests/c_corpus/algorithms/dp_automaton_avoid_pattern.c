/*
 * title: Counting strings that avoid a pattern: KMP automaton DP and matrix power
 * topic: algorithms
 * covers: dynamic programming, KMP failure function, automaton states, transition matrix exponentiation, modular counting, brute-force enumeration
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

typedef unsigned long long u64;
#define MOD 1000000007ULL
#define MAXP 8

static int build(const char *pat, int alpha, int trans[][4]) {
    int m = (int)strlen(pat), fail[MAXP + 1] = {0};
    for (int i = 1, k = 0; i < m; i++) {
        while (k && pat[i] != pat[k]) k = fail[k - 1];
        if (pat[i] == pat[k]) k++;
        fail[i] = k;
    }
    for (int s = 0; s < m; s++)
        for (int c = 0; c < alpha; c++) {
            int k = s;
            while (k && pat[k] != 'a' + c) k = fail[k - 1];
            if (pat[k] == 'a' + c) k++;
            trans[s][c] = k; /* k == m means the pattern occurred (dead state) */
        }
    return m;
}

static u64 dp_count(int m, int alpha, int trans[][4], int len) {
    u64 cur[MAXP] = {0}, nxt[MAXP];
    cur[0] = 1;
    for (int i = 0; i < len; i++) {
        memset(nxt, 0, sizeof nxt);
        for (int s = 0; s < m; s++)
            for (int c = 0; c < alpha; c++)
                if (trans[s][c] < m) nxt[trans[s][c]] = (nxt[trans[s][c]] + cur[s]) % MOD;
        memcpy(cur, nxt, sizeof cur);
    }
    u64 t = 0;
    for (int s = 0; s < m; s++) t = (t + cur[s]) % MOD;
    return t;
}

typedef struct { u64 v[MAXP][MAXP]; } Mat;

static Mat mul(const Mat *a, const Mat *b, int m) {
    Mat r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < m; i++)
        for (int k = 0; k < m; k++) {
            if (!a->v[i][k]) continue;
            for (int j = 0; j < m; j++) r.v[i][j] = (r.v[i][j] + a->v[i][k] * b->v[k][j]) % MOD;
        }
    return r;
}

static u64 mat_count(int m, int alpha, int trans[][4], u64 len) {
    Mat base, res;
    memset(&base, 0, sizeof base);
    memset(&res, 0, sizeof res);
    for (int i = 0; i < m; i++) res.v[i][i] = 1;
    for (int s = 0; s < m; s++)
        for (int c = 0; c < alpha; c++)
            if (trans[s][c] < m) base.v[s][trans[s][c]]++;
    while (len) {
        if (len & 1) res = mul(&res, &base, m);
        base = mul(&base, &base, m);
        len >>= 1;
    }
    u64 t = 0;
    for (int j = 0; j < m; j++) t = (t + res.v[0][j]) % MOD;
    return t;
}

static u64 brute(const char *pat, int alpha, int len) {
    u64 total = 1, cnt = 0;
    for (int i = 0; i < len; i++) total *= (u64)alpha;
    for (u64 x = 0; x < total; x++) {
        char s[16];
        u64 y = x;
        for (int i = 0; i < len; i++) { s[i] = (char)('a' + y % (u64)alpha); y /= (u64)alpha; }
        s[len] = 0;
        if (!strstr(s, pat)) cnt++;
    }
    return cnt;
}

int main(void) {
    (void)rr;
    const char *pats[] = {"ab", "aa", "aba", "abab", "aab", "abc", "aabaa"};
    int alphas[] = {2, 2, 2, 2, 2, 3, 2};
    for (int p = 0; p < 7; p++) {
        int trans[MAXP][4];
        int m = build(pats[p], alphas[p], trans);
        for (int len = 0; len <= 10; len++) {
            u64 d = dp_count(m, alphas[p], trans, len);
            CHECK(d == mat_count(m, alphas[p], trans, (u64)len));
            if (len <= 9) CHECK(d == brute(pats[p], alphas[p], len));
        }
        printf("avoid \"%s\" over %d letters: len5=%llu len10=%llu len1000=%llu len10^15=%llu\n", pats[p], alphas[p],
               dp_count(m, alphas[p], trans, 5), dp_count(m, alphas[p], trans, 10), dp_count(m, alphas[p], trans, 1000),
               mat_count(m, alphas[p], trans, 1000000000000000ULL));
    }
    return 0;
}
