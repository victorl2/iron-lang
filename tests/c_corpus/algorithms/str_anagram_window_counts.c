/*
 * title: Anagram search with sliding letter counts
 * topic: algorithms
 * covers: sliding window, mismatch counter, letter-count signatures, sorted signature grouping, pair counting
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

/* Anagram-style windowed searches with a running "mismatch counter" on letter counts. */
enum { A = 26 };

/* all start indices where text[i..i+m) is a permutation of p */
static int anagram_starts(const char *t, const char *p, int *out) {
    int n = (int)strlen(t), m = (int)strlen(p), need[A] = {0}, have[A] = {0}, c = 0;
    if (m > n)
        return 0;
    int bad = 0; /* number of letters whose counts differ */
    for (int i = 0; i < m; i++)
        need[p[i] - 'a']++;
    for (int i = 0; i < m; i++)
        have[t[i] - 'a']++;
    for (int l = 0; l < A; l++)
        bad += have[l] != need[l];
    if (!bad)
        out[c++] = 0;
    for (int i = m; i < n; i++) {
        int in = t[i] - 'a', outc = t[i - m] - 'a';
        bad -= have[in] != need[in];
        have[in]++;
        bad += have[in] != need[in];
        bad -= have[outc] != need[outc];
        have[outc]--;
        bad += have[outc] != need[outc];
        if (!bad)
            out[c++] = i - m + 1;
    }
    return c;
}

static int anagram_brute(const char *t, const char *p, int *out) {
    int n = (int)strlen(t), m = (int)strlen(p), c = 0;
    for (int i = 0; i + m <= n; i++) {
        int cnt[A] = {0}, ok = 1;
        for (int k = 0; k < m; k++) {
            cnt[p[k] - 'a']++;
            cnt[t[i + k] - 'a']--;
        }
        for (int l = 0; l < A; l++)
            ok &= cnt[l] == 0;
        if (ok)
            out[c++] = i;
    }
    return c;
}

/* count of substrings that are anagrams of some earlier-or-equal substring: group by signature */
typedef struct {
    unsigned char sig[A];
} Sig;

static int cmp_sig(const void *a, const void *b) {
    return memcmp(a, b, A);
}

static long anagram_pair_count(const char *s, int len) {
    int n = (int)strlen(s), cnt = n - len + 1;
    Sig *v = malloc(sizeof(Sig) * (size_t)cnt);
    int cur[A] = {0};
    for (int i = 0; i < len; i++)
        cur[s[i] - 'a']++;
    for (int i = 0; i < cnt; i++) {
        if (i > 0) {
            cur[s[i - 1] - 'a']--;
            cur[s[i + len - 1] - 'a']++;
        }
        for (int l = 0; l < A; l++)
            v[i].sig[l] = (unsigned char)cur[l];
    }
    qsort(v, (size_t)cnt, sizeof(Sig), cmp_sig);
    long pairs = 0, run = 1;
    for (int i = 1; i <= cnt; i++) {
        if (i < cnt && memcmp(v[i].sig, v[i - 1].sig, A) == 0)
            run++;
        else {
            pairs += run * (run - 1) / 2;
            run = 1;
        }
    }
    free(v);
    return pairs;
}

int main(void) {
    static char t[5001];
    static int a[5001], b[5001];
    const char *demo_t = "cbaebabacd";
    int na = anagram_starts(demo_t, "abc", a);
    printf("anagrams of abc in %s:", demo_t);
    for (int i = 0; i < na; i++)
        printf(" %d", a[i]);
    printf("\n");
    check(na == 2 && a[0] == 0 && a[1] == 6, "demo");

    const char *pats[] = {"abc", "aabb", "abcde", "zzz", "abcabc"};
    int alph[] = {3, 2, 5, 26, 3};
    for (int c = 0; c < 5; c++) {
        rand_str(t, 5000, alph[c]);
        na = anagram_starts(t, pats[c], a);
        int nb = anagram_brute(t, pats[c], b);
        check(na == nb, "anagram count");
        for (int i = 0; i < na; i++)
            check(a[i] == b[i], "anagram pos");
        printf("pattern %-7s -> %d windows\n", pats[c], na);
    }
    for (int r = 0; r < 4; r++) {
        rand_str(t, 400, 4);
        long p3 = anagram_pair_count(t, 3), p5 = anagram_pair_count(t, 5);
        /* brute for length 3 */
        long bp = 0;
        for (int i = 0; i + 3 <= 400; i++)
            for (int j = i + 1; j + 3 <= 400; j++) {
                int cnt[A] = {0}, ok = 1;
                for (int k = 0; k < 3; k++) {
                    cnt[t[i + k] - 'a']++;
                    cnt[t[j + k] - 'a']--;
                }
                for (int l = 0; l < A; l++)
                    ok &= cnt[l] == 0;
                bp += ok;
            }
        check(bp == p3, "anagram pairs");
        printf("random anagram pairs: len3=%ld len5=%ld\n", p3, p5);
    }
    return 0;
}
