/*
 * title: Manacher palindromic radii
 * topic: algorithms
 * covers: Manacher, odd and even palindromes, palindrome counting, longest palindromic substring, direct expansion check
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

/* Manacher: d1[i] = number of odd palindromes centered at i (radius incl. center),
 * d2[i] = number of even palindromes centered between i-1 and i. */
static void manacher(const char *s, int n, int *d1, int *d2) {
    for (int i = 0, l = 0, r = -1; i < n; i++) {
        int k = i > r ? 1 : (d1[l + r - i] < r - i + 1 ? d1[l + r - i] : r - i + 1);
        while (i - k >= 0 && i + k < n && s[i - k] == s[i + k])
            k++;
        d1[i] = k;
        if (i + k - 1 > r) {
            l = i - k + 1;
            r = i + k - 1;
        }
    }
    for (int i = 0, l = 0, r = -1; i < n; i++) {
        int k = i > r ? 0 : (d2[l + r - i + 1] < r - i + 1 ? d2[l + r - i + 1] : r - i + 1);
        while (i - k - 1 >= 0 && i + k < n && s[i - k - 1] == s[i + k])
            k++;
        d2[i] = k;
        if (i + k - 1 > r) {
            l = i - k;
            r = i + k - 1;
        }
    }
}

static int is_pal(const char *s, int l, int r) {
    while (l < r)
        if (s[l++] != s[r--])
            return 0;
    return 1;
}

static long count_brute(const char *s, int n) {
    long c = 0;
    for (int i = 0; i < n; i++)
        for (int j = i; j < n; j++)
            c += is_pal(s, i, j);
    return c;
}

int main(void) {
    static char s[1501];
    static int d1[1501], d2[1501];
    const char *fixed[] = {"babad", "abaaba", "forgeeksskeegfor", "aaaa", "abcde"};
    for (int c = 0; c < 5; c++) {
        int n = (int)strlen(fixed[c]);
        manacher(fixed[c], n, d1, d2);
        int best = 0, bs = 0;
        long cnt = 0;
        for (int i = 0; i < n; i++) {
            cnt += d1[i] + d2[i];
            if (2 * d1[i] - 1 > best) {
                best = 2 * d1[i] - 1;
                bs = i - d1[i] + 1;
            }
            if (2 * d2[i] > best) {
                best = 2 * d2[i];
                bs = i - d2[i];
            }
        }
        check(cnt == count_brute(fixed[c], n), "count known");
        printf("%-18s longest=%d \"%.*s\" palindromic substrings=%ld\n", fixed[c], best, best,
               fixed[c] + bs, cnt);
    }
    int alph[] = {1, 2, 2, 3, 26};
    for (int c = 0; c < 5; c++) {
        int n = 200 + (int)(rnd() % 1200);
        rand_str(s, n, alph[c]);
        manacher(s, n, d1, d2);
        long cnt = 0;
        int best = 0;
        for (int i = 0; i < n; i++) {
            cnt += d1[i] + d2[i];
            if (2 * d1[i] - 1 > best)
                best = 2 * d1[i] - 1;
            if (2 * d2[i] > best)
                best = 2 * d2[i];
            /* radius checks against direct expansion */
            int k = 0;
            while (i - k >= 0 && i + k < n && s[i - k] == s[i + k])
                k++;
            check(k == d1[i], "odd radius");
            k = 0;
            while (i - k - 1 >= 0 && i + k < n && s[i - k - 1] == s[i + k])
                k++;
            check(k == d2[i], "even radius");
        }
        if (n <= 700)
            check(cnt == count_brute(s, n), "count vs brute");
        printf("random n=%d alphabet=%d longest=%d count=%ld\n", n, alph[c], best, cnt);
    }
    return 0;
}
