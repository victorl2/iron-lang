/*
 * title: Shortest palindrome by adding characters
 * topic: algorithms
 * covers: prefix function trick, longest palindromic prefix, reversal symmetry, insertion DP lower bound, brute force
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

/* Palindrome construction with the prefix function:
 *  - shortest palindrome by prepending characters (longest palindromic prefix via s + '#' + rev(s))
 *  - shortest palindrome by appending characters (symmetric)
 *  - minimum insertions overall is a DP, used here as the checker */
static void prefix_function(const char *s, int n, int *pi) {
    pi[0] = 0;
    for (int i = 1; i < n; i++) {
        int k = pi[i - 1];
        while (k > 0 && s[i] != s[k])
            k = pi[k - 1];
        pi[i] = s[i] == s[k] ? k + 1 : 0;
    }
}

static int longest_pal_prefix(const char *s, int n) {
    char *t = malloc((size_t)(2 * n + 2));
    int *pi = malloc(sizeof(int) * (size_t)(2 * n + 1));
    memcpy(t, s, (size_t)n);
    t[n] = '#';
    for (int i = 0; i < n; i++)
        t[n + 1 + i] = s[n - 1 - i];
    t[2 * n + 1] = 0;
    prefix_function(t, 2 * n + 1, pi);
    int r = pi[2 * n];
    free(t);
    free(pi);
    return r;
}

static void reverse_copy(const char *s, int n, char *out) {
    for (int i = 0; i < n; i++)
        out[i] = s[n - 1 - i];
    out[n] = 0;
}

/* shortest palindrome formed by adding characters in front */
static void prepend_shortest(const char *s, char *out) {
    int n = (int)strlen(s);
    int k = longest_pal_prefix(s, n);
    for (int i = 0; i < n - k; i++)
        out[i] = s[n - 1 - i];
    memcpy(out + (n - k), s, (size_t)n + 1);
}

/* shortest palindrome by adding characters at the end: reverse trick */
static void append_shortest(const char *s, char *out) {
    int n = (int)strlen(s);
    char r[256], tmp[512];
    reverse_copy(s, n, r);
    prepend_shortest(r, tmp);
    reverse_copy(tmp, (int)strlen(tmp), out);
}

static int is_pal(const char *s) {
    int n = (int)strlen(s);
    for (int i = 0; i < n / 2; i++)
        if (s[i] != s[n - 1 - i])
            return 0;
    return 1;
}

/* DP: minimum number of insertions anywhere to make s a palindrome */
static int min_insertions(const char *s) {
    int n = (int)strlen(s);
    int dp[64][64];
    memset(dp, 0, sizeof dp);
    for (int len = 2; len <= n; len++)
        for (int i = 0; i + len <= n; i++) {
            int j = i + len - 1;
            if (s[i] == s[j])
                dp[i][j] = dp[i + 1][j - 1];
            else {
                int a = dp[i + 1][j], b = dp[i][j - 1];
                dp[i][j] = 1 + (a < b ? a : b);
            }
        }
    return n > 0 ? dp[0][n - 1] : 0;
}

/* brute force: shortest prepend palindrome by trying each prefix removal */
static int brute_prepend_len(const char *s) {
    int n = (int)strlen(s);
    for (int k = n; k >= 0; k--) {
        int ok = 1;
        for (int i = 0; i < k / 2; i++)
            if (s[i] != s[k - 1 - i])
                ok = 0;
        if (ok)
            return 2 * n - k;
    }
    return 2 * n;
}

int main(void) {
    const char *fixed[] = {"aacecaaa", "abcd", "race", "a", "abab", "level", "aaaab", "abbacd"};
    char out[600], out2[600];
    for (int c = 0; c < 8; c++) {
        prepend_shortest(fixed[c], out);
        append_shortest(fixed[c], out2);
        check(is_pal(out) && is_pal(out2), "palindromes");
        check((int)strlen(out) == brute_prepend_len(fixed[c]), "prepend length");
        {
            char rv[64];
            reverse_copy(fixed[c], (int)strlen(fixed[c]), rv);
            check((int)strlen(out2) == brute_prepend_len(rv), "append length");
        }
        check(strlen(out) >= strlen(fixed[c]) &&
                  strcmp(out + strlen(out) - strlen(fixed[c]), fixed[c]) == 0,
              "prepend keeps suffix");
        check(strncmp(out2, fixed[c], strlen(fixed[c])) == 0, "append keeps prefix");
        printf("%-8s prepend -> %-14s append -> %-14s min insertions anywhere %d\n", fixed[c], out,
               out2, min_insertions(fixed[c]));
    }
    int gap = 0, tested = 0;
    for (int t = 0; t < 2000; t++) {
        char s[40];
        int n = 1 + (int)(rnd() % 20);
        rand_str(s, n, 2 + t % 3);
        prepend_shortest(s, out);
        append_shortest(s, out2);
        check(is_pal(out) && is_pal(out2), "random palindromes");
        check((int)strlen(out) == brute_prepend_len(s), "random prepend length");
        int mi = min_insertions(s);
        check(mi <= (int)strlen(out) - n && mi <= (int)strlen(out2) - n, "insertion lower bound");
        gap += (int)strlen(out) - n - mi;
        tested++;
    }
    printf("random: %d strings, total gap between end-only and anywhere insertions: %d\n", tested, gap);
    return 0;
}
