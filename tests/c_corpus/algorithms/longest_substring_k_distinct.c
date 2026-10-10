/*
 * title: Longest substring with at most k distinct characters
 * topic: algorithms
 * covers: sliding window over strings, frequency table, distinct counter, exact-k via difference of at-most, byte alphabets
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 8675309u;
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

/* longest window with <= k distinct bytes */
static int at_most(const unsigned char *s, int n, int k, int *start) {
    int cnt[256] = {0}, distinct = 0, left = 0, best = 0;
    *start = 0;
    for (int r = 0; r < n; r++) {
        if (cnt[s[r]]++ == 0)
            distinct++;
        while (distinct > k) {
            if (--cnt[s[left]] == 0)
                distinct--;
            left++;
        }
        if (r - left + 1 > best) {
            best = r - left + 1;
            *start = left;
        }
    }
    return best;
}

/* number of substrings with at most k distinct bytes */
static long count_at_most(const unsigned char *s, int n, int k) {
    int cnt[256] = {0}, distinct = 0, left = 0;
    long total = 0;
    for (int r = 0; r < n; r++) {
        if (cnt[s[r]]++ == 0)
            distinct++;
        while (distinct > k) {
            if (--cnt[s[left]] == 0)
                distinct--;
            left++;
        }
        total += r - left + 1;
    }
    return total;
}

static int brute(const unsigned char *s, int n, int k) {
    int best = 0;
    for (int i = 0; i < n; i++) {
        int seen[256] = {0}, d = 0;
        for (int j = i; j < n; j++) {
            if (!seen[s[j]]++)
                d++;
            if (d > k)
                break;
            if (j - i + 1 > best)
                best = j - i + 1;
        }
    }
    return best;
}

int main(void) {
    unsigned char buf[400];
    for (int trial = 0; trial < 200; trial++) {
        int n = (int)(rnd() % 300);
        int alpha = 1 + (int)(rnd() % 8);
        for (int i = 0; i < n; i++)
            buf[i] = (unsigned char)('a' + rnd() % (unsigned)alpha);
        for (int k = 0; k <= 4; k++) {
            int st0;
            int got = at_most(buf, n, k, &st0);
            if (got != brute(buf, n, k))
                fail("at_most");
            if (got > 0) {
                int seen[256] = {0}, d = 0;
                for (int i = st0; i < st0 + got; i++)
                    if (!seen[buf[i]]++)
                        d++;
                if (d > k)
                    fail("window distinct");
            }
        }
    }
    printf("200 random strings verified\n");

    const char *words[] = {"eceba", "aaaaaa", "abcabcabc", "araaci", "aabbccddeeff", ""};
    for (int w = 0; w < 6; w++) {
        const unsigned char *s = (const unsigned char *)words[w];
        int n = (int)strlen(words[w]);
        int a2, a3;
        int l2 = at_most(s, n, 2, &a2);
        (void)at_most(s, n, 3, &a3);
        long exact2 = count_at_most(s, n, 2) - count_at_most(s, n, 1);
        printf("%-13s k=2 longest=%d \"%.*s\" exactly-2 substrings=%ld\n",
               n ? words[w] : "(empty)", l2, l2, words[w] + a2, exact2);
    }
    return 0;
}
