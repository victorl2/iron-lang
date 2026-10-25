/*
 * title: Borders, periods and Fine-Wilf
 * topic: algorithms
 * covers: prefix function, border enumeration, smallest period, prefix occurrence counts, Fine and Wilf theorem
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

enum { MAXN = 512 };

static void prefix_function(const char *s, int n, int *pi) {
    pi[0] = 0;
    for (int i = 1; i < n; i++) {
        int k = pi[i - 1];
        while (k > 0 && s[i] != s[k])
            k = pi[k - 1];
        pi[i] = s[i] == s[k] ? k + 1 : 0;
    }
}

/* All borders (proper prefixes that are also suffixes), longest first. */
static int borders(const int *pi, int n, int *out) {
    int c = 0;
    for (int k = pi[n - 1]; k > 0; k = pi[k - 1])
        out[c++] = k;
    return c;
}

static int is_border(const char *s, int n, int k) {
    return memcmp(s, s + n - k, (size_t)k) == 0;
}

/* number of occurrences of each prefix of s inside s */
static void prefix_occurrences(const int *pi, int n, long *occ) {
    for (int i = 0; i <= n; i++)
        occ[i] = 0;
    for (int i = 0; i < n; i++)
        occ[pi[i]]++;
    for (int i = n - 1; i > 0; i--)
        occ[pi[i - 1]] += occ[i];
    for (int i = 1; i <= n; i++)
        occ[i]++;
}

int main(void) {
    const char *words[] = {"abcabcabcab", "aabaaab", "abababab", "abcdef", "aaaaaa",
                           "abaabaabaab", "xyxyxyxyxy", "a"};
    int pi[MAXN], bs[MAXN];
    for (int w = 0; w < 8; w++) {
        const char *s = words[w];
        int n = (int)strlen(s);
        prefix_function(s, n, pi);
        int nb = borders(pi, n, bs);
        int period = n - pi[n - 1];
        int primitive = (n % period == 0);
        printf("%-12s period=%d %s borders:", s, period, primitive && period < n ? "(perfect power)" : "");
        for (int i = 0; i < nb; i++) {
            printf(" %d", bs[i]);
            check(is_border(s, n, bs[i]), "border valid");
        }
        /* brute force: every k in 1..n-1 that is a border must be listed */
        int expect = 0;
        for (int k = 1; k < n; k++)
            expect += is_border(s, n, k);
        check(expect == nb, "border count");
        /* smallest p with s[i]==s[i+p] for all i */
        int bp = 0;
        for (int p = 1; p <= n && !bp; p++) {
            int ok = 1;
            for (int i = 0; i + p < n; i++)
                if (s[i] != s[i + p])
                    ok = 0;
            if (ok)
                bp = p;
        }
        check(bp == period, "period brute");
        printf("\n");
    }
    /* prefix occurrence counts for one string */
    const char *s = "abababa";
    long occ[MAXN];
    int n = (int)strlen(s);
    prefix_function(s, n, pi);
    prefix_occurrences(pi, n, occ);
    printf("occurrences of prefixes of %s:", s);
    for (int k = 1; k <= n; k++) {
        long b = 0;
        for (int i = 0; i + k <= n; i++)
            b += memcmp(s, s + i, (size_t)k) == 0;
        check(b == occ[k], "prefix occurrences");
        printf(" %ld", occ[k]);
    }
    printf("\n");
    /* Fine and Wilf: two periods p,q with p+q-gcd <= n imply period gcd. count via random periodic strings */
    int fw_checked = 0;
    for (int t = 0; t < 300; t++) {
        int base = 1 + (int)(rnd() % 6), len = 8 + (int)(rnd() % 40);
        char seed[8], str[64];
        rand_str(seed, base, 2);
        for (int i = 0; i < len; i++)
            str[i] = seed[i % base];
        str[len] = 0;
        prefix_function(str, len, pi);
        int per = len - pi[len - 1];
        check(per <= base, "period at most generating length");
        int nb = borders(pi, len, bs);
        for (int i = 0; i < nb; i++) {
            int p2 = len - bs[i];
            if (per + p2 <= len) {
                check(p2 % per == 0, "Fine and Wilf");
                fw_checked++;
            }
        }
    }
    printf("Fine-Wilf instances verified: %d\n", fw_checked);
    return 0;
}
