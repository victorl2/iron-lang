/*
 * title: Minimum window covering a multiset of characters
 * topic: algorithms
 * covers: sliding window with need/have counters, expand-then-contract, satisfied-count trick, multiset requirements
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 4711u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

/* smallest window of s containing every byte of t with multiplicity; -1 if none */
static int min_window(const unsigned char *s, int n, const unsigned char *t, int m, int *start) {
    int need[256] = {0}, have[256] = {0}, required = 0;
    for (int i = 0; i < m; i++)
        if (need[t[i]]++ == 0)
            required++;
    int formed = 0, left = 0, best = -1;
    for (int r = 0; r < n; r++) {
        if (++have[s[r]] == need[s[r]] && need[s[r]] > 0)
            formed++;
        while (formed == required && required > 0) {
            if (best < 0 || r - left + 1 < best) {
                best = r - left + 1;
                *start = left;
            }
            if (have[s[left]]-- == need[s[left]] && need[s[left]] > 0)
                formed--;
            left++;
        }
    }
    return best;
}

static int brute(const unsigned char *s, int n, const unsigned char *t, int m) {
    int need[256] = {0};
    for (int i = 0; i < m; i++)
        need[t[i]]++;
    int best = -1;
    for (int i = 0; i < n; i++) {
        int have[256] = {0};
        for (int j = i; j < n; j++) {
            have[s[j]]++;
            int ok = 1;
            for (int c = 0; c < 256 && ok; c++)
                if (have[c] < need[c])
                    ok = 0;
            if (ok) {
                if (best < 0 || j - i + 1 < best)
                    best = j - i + 1;
                break;
            }
        }
    }
    return best;
}

int main(void) {
    unsigned char s[200], t[10];
    int found = 0;
    for (int trial = 0; trial < 400; trial++) {
        int n = (int)(rnd() % 120), m = 1 + (int)(rnd() % 6);
        int alpha = 2 + (int)(rnd() % 6);
        for (int i = 0; i < n; i++)
            s[i] = (unsigned char)('a' + rnd() % (unsigned)alpha);
        for (int i = 0; i < m; i++)
            t[i] = (unsigned char)('a' + rnd() % (unsigned)alpha);
        int st0 = 0;
        int g = min_window(s, n, t, m, &st0), b = brute(s, n, t, m);
        if (g != b)
            fail("min_window");
        if (g >= 0)
            found++;
    }
    printf("400 random cases verified, %d had a covering window\n", found);

    struct {
        const char *s, *t;
    } cases[] = {{"ADOBECODEBANC", "ABC"}, {"a", "a"}, {"a", "aa"}, {"aaflslflsldkalskaaa", "aaa"},
                 {"ab", "b"}, {"xyz", "q"}, {"bbaa", "aba"}};
    for (int i = 0; i < 7; i++) {
        int s0 = 0;
        int len = min_window((const unsigned char *)cases[i].s, (int)strlen(cases[i].s),
                             (const unsigned char *)cases[i].t, (int)strlen(cases[i].t), &s0);
        if (len < 0)
            printf("%-22s need %-4s -> none\n", cases[i].s, cases[i].t);
        else
            printf("%-22s need %-4s -> \"%.*s\" (len %d at %d)\n", cases[i].s, cases[i].t, len,
                   cases[i].s + s0, len, s0);
    }
    return 0;
}
