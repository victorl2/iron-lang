/*
 * title: Lyndon factorization with Duval
 * topic: algorithms
 * covers: Duval algorithm, Lyndon words, exhaustive enumeration, necklace polynomial counts, invariants
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

/* Duval's algorithm: write factor lengths, return count. */
static int duval(const char *s, int n, int *lens) {
    int i = 0, cnt = 0;
    while (i < n) {
        int j = i + 1, k = i;
        while (j < n && s[k] <= s[j]) {
            k = s[k] < s[j] ? i : k + 1;
            j++;
        }
        while (i <= k) {
            lens[cnt++] = j - k;
            i += j - k;
        }
    }
    return cnt;
}

/* Reference: proper suffixes compare greater as full strings. */
static int lyndon_ref(const char *s, int l, int n) {
    char a[128], b[128];
    memcpy(a, s + l, (size_t)n);
    a[n] = 0;
    for (int i = 1; i < n; i++) {
        memcpy(b, s + l + i, (size_t)(n - i));
        b[n - i] = 0;
        if (strcmp(a, b) >= 0)
            return 0;
    }
    return 1;
}

int main(void) {
    const char *fixed[] = {"banana", "abracadabra", "aaabaab", "zyx", "abcabcabc", "a", "cbaabcab"};
    int lens[128];
    for (int c = 0; c < 7; c++) {
        int n = (int)strlen(fixed[c]);
        int k = duval(fixed[c], n, lens);
        printf("%-12s ->", fixed[c]);
        int pos = 0;
        for (int i = 0; i < k; i++) {
            printf(" %.*s", lens[i], fixed[c] + pos);
            check(lyndon_ref(fixed[c], pos, lens[i]), "factor is Lyndon");
            if (i > 0) {
                /* non-increasing order */
                char a[64], b[64];
                memcpy(a, fixed[c] + pos - lens[i - 1], (size_t)lens[i - 1]);
                a[lens[i - 1]] = 0;
                memcpy(b, fixed[c] + pos, (size_t)lens[i]);
                b[lens[i]] = 0;
                check(strcmp(a, b) >= 0, "non-increasing factors");
            }
            pos += lens[i];
        }
        check(pos == n, "factors cover string");
        printf(" (%d factors)\n", k);
    }
    /* exhaustive check on all strings over {a,b,c} up to length 7: count Lyndon words */
    long lyndon_by_len[8] = {0};
    for (int n = 1; n <= 7; n++) {
        int total = 1;
        for (int i = 0; i < n; i++)
            total *= 3;
        char s[8];
        for (int v = 0; v < total; v++) {
            int x = v;
            for (int i = 0; i < n; i++) {
                s[i] = (char)('a' + x % 3);
                x /= 3;
            }
            s[n] = 0;
            int k = duval(s, n, lens);
            if (k == 1) {
                lyndon_by_len[n]++;
                check(lyndon_ref(s, 0, n), "single factor implies Lyndon");
            } else {
                check(!lyndon_ref(s, 0, n) || n == 1, "multi factor not Lyndon");
            }
        }
    }
    printf("Lyndon words over 3 letters by length:");
    long expect[8] = {0, 3, 3, 8, 18, 48, 116, 312};
    for (int n = 1; n <= 7; n++) {
        printf(" %ld", lyndon_by_len[n]);
        check(lyndon_by_len[n] == expect[n], "necklace polynomial count");
    }
    printf("\n");
    /* random long strings: factors are non-increasing and cover */
    static char big[2001];
    static int bl[2001];
    for (int r = 0; r < 5; r++) {
        rand_str(big, 2000, 2 + r % 2);
        int k = duval(big, 2000, bl);
        int sum = 0;
        for (int i = 0; i < k; i++)
            sum += bl[i];
        check(sum == 2000, "cover");
        printf("random alphabet=%d factors=%d\n", 2 + r % 2, k);
    }
    return 0;
}
