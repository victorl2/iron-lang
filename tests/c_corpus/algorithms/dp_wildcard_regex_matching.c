/*
 * title: Wildcard and simple regex matching by DP
 * topic: algorithms
 * covers: dynamic programming, pattern matching, star and question mark wildcards, regex dot-star, table of prefixes, recursion cross-check, greedy two-pointer wildcard comparison
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

static int wild_dp(const char *s, const char *p) {
    int n = (int)strlen(s), m = (int)strlen(p);
    static unsigned char d[40][40];
    memset(d, 0, sizeof d);
    d[0][0] = 1;
    for (int j = 1; j <= m; j++) if (p[j - 1] == '*') d[0][j] = d[0][j - 1];
    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= m; j++) {
            if (p[j - 1] == '*') d[i][j] = d[i - 1][j] || d[i][j - 1];
            else d[i][j] = d[i - 1][j - 1] && (p[j - 1] == '?' || p[j - 1] == s[i - 1]);
        }
    return d[n][m];
}

static int wild_rec(const char *s, const char *p) {
    if (!*p) return !*s;
    if (*p == '*') return wild_rec(s, p + 1) || (*s && wild_rec(s + 1, p));
    return *s && (*p == '?' || *p == *s) && wild_rec(s + 1, p + 1);
}

static int wild_greedy(const char *s, const char *p) {
    const char *star = NULL, *mark = NULL;
    while (*s) {
        if (*p == '?' || *p == *s) { p++; s++; }
        else if (*p == '*') { star = p++; mark = s; }
        else if (star) { p = star + 1; s = ++mark; }
        else return 0;
    }
    while (*p == '*') p++;
    return !*p;
}

/* regex with '.' and 'x*' */
static int re_dp(const char *s, const char *p) {
    int n = (int)strlen(s), m = (int)strlen(p);
    static unsigned char d[40][40];
    memset(d, 0, sizeof d);
    d[0][0] = 1;
    for (int j = 2; j <= m; j++) if (p[j - 1] == '*') d[0][j] = d[0][j - 2];
    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= m; j++) {
            if (p[j - 1] == '*') {
                d[i][j] = j >= 2 && (d[i][j - 2] || ((p[j - 2] == '.' || p[j - 2] == s[i - 1]) && d[i - 1][j]));
            } else d[i][j] = d[i - 1][j - 1] && (p[j - 1] == '.' || p[j - 1] == s[i - 1]);
        }
    return d[n][m];
}

static int re_rec(const char *s, const char *p) {
    if (!*p) return !*s;
    int first = *s && (*p == '.' || *p == *s);
    if (p[1] == '*') return re_rec(s, p + 2) || (first && re_rec(s + 1, p));
    return first && re_rec(s + 1, p + 1);
}

int main(void) {
    struct { const char *s, *p; } w[] = {{"aa", "a"}, {"aa", "*"}, {"cb", "?a"}, {"adceb", "*a*b"}, {"acdcb", "a*c?b"},
                                          {"", "***"}, {"mississippi", "m??*ss*?i*pi"}, {"abcabc", "*abc"}};
    for (int i = 0; i < 8; i++) {
        int a = wild_dp(w[i].s, w[i].p), b = wild_rec(w[i].s, w[i].p), c = wild_greedy(w[i].s, w[i].p);
        CHECK(a == b && b == c);
        printf("wild \"%s\" ~ \"%s\": %d\n", w[i].s, w[i].p, a);
    }
    struct { const char *s, *p; } r[] = {{"aa", "a"}, {"aa", "a*"}, {"ab", ".*"}, {"aab", "c*a*b"}, {"mississippi", "mis*is*p*."},
                                          {"", "a*b*"}, {"abc", ".*c"}};
    for (int i = 0; i < 7; i++) {
        int a = re_dp(r[i].s, r[i].p), b = re_rec(r[i].s, r[i].p);
        CHECK(a == b);
        printf("regex \"%s\" ~ \"%s\": %d\n", r[i].s, r[i].p, a);
    }
    int hits = 0, hitr = 0;
    for (int t = 0; t < 400; t++) {
        char s[10], p[10], q[12];
        int n = rr(8), m = rr(7), k = 0;
        for (int i = 0; i < n; i++) s[i] = (char)('a' + rr(2));
        s[n] = 0;
        for (int i = 0; i < m; i++) { int c = rr(6); p[i] = c < 2 ? 'a' : c < 4 ? 'b' : c == 4 ? '?' : '*'; }
        p[m] = 0;
        int a = wild_dp(s, p);
        CHECK(a == wild_rec(s, p) && a == wild_greedy(s, p));
        hits += a;
        for (int i = 0; i < m; i++) {
            int c = rr(5);
            if (c == 4 && k > 0 && q[k - 1] != '*' && q[k - 1] != '.') q[k++] = '*';
            else q[k++] = c < 2 ? (char)('a' + c) : (c == 2 ? '.' : 'a');
        }
        q[k] = 0;
        if (q[0] == '*') q[0] = 'a';
        int b = re_dp(s, q);
        CHECK(b == re_rec(s, q));
        hitr += b;
    }
    printf("random wildcard matches=%d regex matches=%d of 400\n", hits, hitr);
    return 0;
}
