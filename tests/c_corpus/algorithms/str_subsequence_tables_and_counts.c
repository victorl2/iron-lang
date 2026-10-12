/*
 * title: Subsequence tables and counting
 * topic: algorithms
 * covers: next-occurrence table, subsequence test, distinct subsequence DP, occurrence counting, monotonic stack smallest subsequence
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

/* Subsequence toolbox: next-occurrence table, subsequence test in O(|t|), counting distinct
 * subsequences, counting occurrences of t as a subsequence, and the lexicographically smallest
 * subsequence of a given length (monotonic stack). */
typedef unsigned long long u64;

/* nxt[i][c] = smallest j >= i with s[j] == c, or n if none */
static void build_next(const char *s, int n, int (*nxt)[26]) {
    for (int c = 0; c < 26; c++)
        nxt[n][c] = n;
    for (int i = n - 1; i >= 0; i--) {
        memcpy(nxt[i], nxt[i + 1], sizeof(int) * 26);
        nxt[i][s[i] - 'a'] = i;
    }
}

static int is_subsequence(int (*nxt)[26], int n, const char *t) {
    int pos = 0;
    for (; *t; t++) {
        if (pos >= n)
            return 0;
        int j = nxt[pos][*t - 'a'];
        if (j >= n)
            return 0;
        pos = j + 1;
    }
    return 1;
}

static int is_subsequence_scan(const char *s, const char *t) {
    while (*s && *t)
        if (*s++ == *t)
            t++;
    return *t == 0;
}

/* number of distinct non-empty subsequences, modulo 2^64 (exact for short strings) */
static u64 distinct_subsequences(const char *s) {
    u64 last[26] = {0}, total = 1; /* empty subsequence counted */
    for (; *s; s++) {
        int c = *s - 'a';
        u64 add = total - last[c];
        last[c] = total;
        total += add;
    }
    return total - 1;
}

static u64 distinct_brute(const char *s) {
    int n = (int)strlen(s);
    u64 count = 0;
    /* enumerate masks, keep those whose subsequence string is not seen earlier */
    enum { MAXM = 1 << 12 };
    static char store[MAXM][14];
    int ns = 0;
    for (unsigned m = 1; m < (1u << n); m++) {
        char buf[14];
        int k = 0;
        for (int i = 0; i < n; i++)
            if (m >> i & 1)
                buf[k++] = s[i];
        buf[k] = 0;
        int dup = 0;
        for (int i = 0; i < ns && !dup; i++)
            dup = strcmp(store[i], buf) == 0;
        if (!dup && ns < MAXM) {
            strcpy(store[ns++], buf);
            count++;
        }
    }
    return count;
}

/* number of index tuples i1<...<im with s[ik]==t[k] */
static u64 count_occurrences(const char *s, const char *t) {
    int m = (int)strlen(t);
    u64 dp[64] = {1};
    for (int i = 1; i <= m; i++)
        dp[i] = 0;
    for (; *s; s++)
        for (int j = m; j >= 1; j--)
            if (t[j - 1] == *s)
                dp[j] += dp[j - 1];
    return dp[m];
}

static void smallest_subsequence(const char *s, int k, char *out) {
    int n = (int)strlen(s), top = 0;
    for (int i = 0; i < n; i++) {
        while (top > 0 && out[top - 1] > s[i] && top - 1 + (n - i) >= k)
            top--;
        if (top < k)
            out[top++] = s[i];
    }
    out[top] = 0;
}

static void smallest_brute(const char *s, int k, char *out) {
    int n = (int)strlen(s);
    char best[32] = "", buf[32];
    int have = 0;
    for (unsigned m = 0; m < (1u << n); m++) {
        int pc = 0;
        for (int i = 0; i < n; i++)
            pc += (int)(m >> i & 1);
        if (pc != k)
            continue;
        int c = 0;
        for (int i = 0; i < n; i++)
            if (m >> i & 1)
                buf[c++] = s[i];
        buf[c] = 0;
        if (!have || strcmp(buf, best) < 0) {
            strcpy(best, buf);
            have = 1;
        }
    }
    strcpy(out, best);
}

int main(void) {
    static int nxt[61][26];
    static char s[61], t[32];
    /* known values */
    printf("distinct subsequences: abc=%llu aaa=%llu abab=%llu\n", distinct_subsequences("abc"),
           distinct_subsequences("aaa"), distinct_subsequences("abab"));
    check(distinct_subsequences("abc") == 7 && distinct_subsequences("aaa") == 3 &&
              distinct_subsequences("abab") == 11,
          "known distinct");
    printf("rabbbit contains rabbit: %llu ways; babgbag contains bag: %llu ways\n",
           count_occurrences("rabbbit", "rabbit"), count_occurrences("babgbag", "bag"));
    check(count_occurrences("rabbbit", "rabbit") == 3 && count_occurrences("babgbag", "bag") == 5, "known ways");

    int n = 60;
    rand_str(s, n, 6);
    build_next(s, n, nxt);
    int yes = 0;
    for (int q = 0; q < 3000; q++) {
        int len = 4 + (int)(rnd() % 16);
        rand_str(t, len, 6);
        int a = is_subsequence(nxt, n, t), b = is_subsequence_scan(s, t);
        check(a == b, "subsequence test");
        yes += a;
    }
    printf("random subsequence queries: %d of 3000 present\n", yes);

    for (int r = 0; r < 25; r++) {
        int len = 6 + (int)(rnd() % 7);
        char small[16];
        rand_str(small, len, 3);
        check(distinct_subsequences(small) == distinct_brute(small), "distinct vs brute");
    }
    printf("distinct subsequence counts verified against enumeration for 25 strings\n");

    char o1[32], o2[32];
    int matches = 0;
    for (int r = 0; r < 40; r++) {
        int len = 8 + (int)(rnd() % 8);
        char w[24];
        rand_str(w, len, 5);
        int k = 1 + (int)(rnd() % (unsigned)len);
        smallest_subsequence(w, k, o1);
        smallest_brute(w, k, o2);
        check(strcmp(o1, o2) == 0, "smallest subsequence");
        matches++;
        if (r < 4)
            printf("smallest %d-subsequence of %s = %s\n", k, w, o1);
    }
    printf("smallest-subsequence checks: %d\n", matches);
    return 0;
}
