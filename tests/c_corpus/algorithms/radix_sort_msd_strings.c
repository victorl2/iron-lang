/*
 * title: MSD radix sort on strings with insertion sort cutoff
 * topic: algorithms
 * covers: MSD radix sort, string keys, end-of-string bucket, recursion by character position, cutoff
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 606060u;
static unsigned rng(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long char_probes;
static int max_depth;

static int at(const char *s, int d) {
    char_probes++;
    return s[d] ? (unsigned char)s[d] + 1 : 0; /* 0 = end of string bucket */
}

static void insertion_from(const char **a, int n, int d) {
    for (int i = 1; i < n; i++) {
        const char *x = a[i];
        int j = i - 1;
        while (j >= 0 && strcmp(a[j] + d, x + d) > 0) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = x;
    }
}

static void msd(const char **a, const char **aux, int n, int d, int cutoff) {
    if (d > max_depth)
        max_depth = d;
    if (n <= cutoff) {
        insertion_from(a, n, d);
        return;
    }
    int count[258] = {0};
    for (int i = 0; i < n; i++)
        count[at(a[i], d) + 1]++;
    for (int r = 0; r < 257; r++)
        count[r + 1] += count[r];
    int start[258];
    memcpy(start, count, sizeof start);
    for (int i = 0; i < n; i++)
        aux[count[at(a[i], d)]++] = a[i];
    memcpy(a, aux, sizeof(char *) * (size_t)n);
    /* bucket 0 (strings ending at d) needs no further work */
    for (int r = 1; r < 257; r++) {
        int lo = start[r], hi = start[r + 1];
        if (hi - lo > 1)
            msd(a + lo, aux, hi - lo, d + 1, cutoff);
    }
}

int main(void) {
    enum { N = 500 };
    static char store[N][12];
    const char *base[N], *a[N], *aux[N];
    static const char alpha[] = "abcd";
    for (int i = 0; i < N; i++) {
        /* common prefixes make deep recursion interesting */
        int len = 1 + (int)(rng() % 10);
        int pre = (int)(rng() % 3);
        for (int k = 0; k < len; k++)
            store[i][k] = k < pre ? 'a' : alpha[rng() % 4];
        store[i][len] = 0;
        base[i] = store[i];
    }
    static const int cutoffs[] = {1, 8, 32, 500};
    for (int c = 0; c < 4; c++) {
        memcpy(a, base, sizeof a);
        char_probes = 0;
        max_depth = 0;
        msd(a, aux, N, 0, cutoffs[c]);
        for (int i = 1; i < N; i++)
            check(strcmp(a[i - 1], a[i]) <= 0, "sorted");
        printf("cutoff=%-3d probes=%-6ld max_depth=%d\n", cutoffs[c], char_probes, max_depth);
    }
    /* known-answer tiny case */
    const char *tiny[] = {"she", "sells", "seashells", "by", "the", "sea", "shore", "the", "shells", "she", "sells", ""};
    const char *taux[12];
    msd(tiny, taux, 12, 0, 1);
    printf("tiny:");
    for (int i = 0; i < 12; i++)
        printf(" [%s]", tiny[i]);
    printf("\n");
    printf("first=%s last=%s\n", a[0], a[N - 1]);
    return 0;
}
