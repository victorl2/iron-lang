/*
 * title: Maximal repetitions (runs) via Lyndon roots
 * topic: algorithms
 * covers: runs theorem, Lyndon array, longest common extension, maximal periodicities, brute-force cross-check
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

/* Maximal repetitions ("runs"): substrings s[i..j] with minimal period p where
 * j - i + 1 >= 2p and the periodicity cannot be extended either way. Found by the
 * Lyndon-root method (Bannai et al.) using longest common extensions, and checked by brute force. */
typedef struct {
    int i, j, p; /* inclusive bounds and period */
} Run;

static int lce_f(const char *s, int n, int a, int b) {
    int k = 0;
    while (a + k < n && b + k < n && s[a + k] == s[b + k])
        k++;
    return k;
}

static int lce_b(const char *s, int a, int b) {
    int k = 0;
    while (a - k >= 0 && b - k >= 0 && s[a - k] == s[b - k])
        k++;
    return k;
}

static int cmp_run(const void *x, const void *y) {
    const Run *a = x, *b = y;
    if (a->i != b->i)
        return a->i - b->i;
    if (a->j != b->j)
        return a->j - b->j;
    return a->p - b->p;
}

/* lyn[i] = exclusive end of the longest Lyndon word starting at i, i.e. the next
 * position whose suffix is smaller than suffix i (under the normal or the inverted letter
 * order; a suffix that is a prefix of another is always the smaller one). */
static void lyndon_array(const char *s, int n, int inv, int *lyn) {
    int *stack = malloc(sizeof(int) * (size_t)(n + 1));
    int sp = 0;
    for (int i = n - 1; i >= 0; i--) {
        while (sp > 0) {
            int j = stack[sp - 1];
            int l = lce_f(s, n, i, j);
            int i_smaller;
            if (j + l >= n)
                i_smaller = 0; /* suffix j is a prefix of suffix i */
            else if (inv)
                i_smaller = (unsigned char)s[i + l] > (unsigned char)s[j + l];
            else
                i_smaller = (unsigned char)s[i + l] < (unsigned char)s[j + l];
            if (!i_smaller)
                break;
            sp--;
        }
        lyn[i] = sp > 0 ? stack[sp - 1] : n;
        stack[sp++] = i;
    }
    free(stack);
}

static int find_runs(const char *s, int n, Run *out) {
    int *lyn = malloc(sizeof(int) * (size_t)n);
    int cnt = 0;
    for (int inv = 0; inv < 2; inv++) {
        lyndon_array(s, n, inv, lyn);
        for (int i = 0; i < n; i++) {
            int j = lyn[i] - 1; /* inclusive end of Lyndon word */
            int p = j - i + 1;
            int right = lce_f(s, n, i, i + p);
            int left = i > 0 ? lce_b(s, i - 1, i + p - 1) : 0;
            if (left + right >= p) {
                Run r = {i - left, i + p + right - 1, p};
                int dup = 0;
                for (int k = 0; k < cnt; k++)
                    if (cmp_run(&out[k], &r) == 0)
                        dup = 1;
                if (!dup)
                    out[cnt++] = r;
            }
        }
    }
    free(lyn);
    qsort(out, (size_t)cnt, sizeof(Run), cmp_run);
    return cnt;
}

static int runs_brute(const char *s, int n, Run *out) {
    int cnt = 0;
    for (int p = 1; p <= n / 2; p++) {
        int i = 0;
        while (i + p < n) {
            if (s[i] != s[i + p]) {
                i++;
                continue;
            }
            int j = i;
            while (j + p < n && s[j] == s[j + p])
                j++;
            /* periodic segment [i, j+p-1] */
            int len = j + p - i;
            if (len >= 2 * p) {
                /* minimal period must be exactly p */
                int minimal = 1;
                for (int q = 1; q < p && minimal; q++) {
                    int ok = 1;
                    for (int k = i; k + q < i + len; k++)
                        if (s[k] != s[k + q]) {
                            ok = 0;
                            break;
                        }
                    if (ok)
                        minimal = 0;
                }
                if (minimal) {
                    Run r = {i, i + len - 1, p};
                    out[cnt++] = r;
                }
            }
            i = j + 1;
        }
    }
    qsort(out, (size_t)cnt, sizeof(Run), cmp_run);
    return cnt;
}

int main(void) {
    static char s[200];
    static Run a[4000], b[4000];
    const char *fixed[] = {"mississippi", "aabaabaaaabaabaaab", "abcabcabc", "aaaa", "abcdef"};
    for (int c = 0; c < 5; c++) {
        int n = (int)strlen(fixed[c]);
        int na = find_runs(fixed[c], n, a), nb = runs_brute(fixed[c], n, b);
        check(na == nb, "run count");
        for (int i = 0; i < na; i++)
            check(cmp_run(&a[i], &b[i]) == 0, "run equal");
        printf("%-20s runs=%d:", fixed[c], na);
        for (int i = 0; i < na && i < 6; i++)
            printf(" [%d,%d]p%d", a[i].i, a[i].j, a[i].p);
        printf("\n");
    }
    long total = 0;
    int mx = 0;
    for (int r = 0; r < 60; r++) {
        int n = 20 + (int)(rnd() % 150);
        rand_str(s, n, 1 + r % 3);
        int na = find_runs(s, n, a), nb = runs_brute(s, n, b);
        check(na == nb, "random run count");
        for (int i = 0; i < na; i++)
            check(cmp_run(&a[i], &b[i]) == 0, "random run equal");
        check(na < n, "runs theorem: fewer than n runs");
        total += na;
        if (na > mx)
            mx = na;
    }
    printf("random strings: total runs=%ld, max in one string=%d\n", total, mx);
    return 0;
}
