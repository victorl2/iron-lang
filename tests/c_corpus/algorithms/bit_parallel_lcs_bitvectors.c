/*
 * title: Bit-parallel longest common subsequence
 * topic: algorithms
 * covers: bit vectors, multiword addition with carry, Hyyro/Allison-Dix LCS, match masks, word boundaries, DP cross-check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t st = 0xACE1u;

static uint32_t rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* V + U over nw words, least significant word first. */
static void add_words(uint64_t *dst, const uint64_t *a, const uint64_t *b, int nw) {
    uint64_t carry = 0;
    for (int i = 0; i < nw; i++) {
        uint64_t s = a[i] + b[i];
        uint64_t c1 = s < a[i];
        uint64_t s2 = s + carry;
        uint64_t c2 = s2 < s;
        dst[i] = s2;
        carry = c1 | c2;
    }
}

/* LCS length of a and b using one machine word per 64 characters of a. */
static int lcs_bits(const unsigned char *a, int m, const unsigned char *b, int n, long *word_ops) {
    int nw = (m + 63) / 64;
    uint64_t *match[256];
    for (int c = 0; c < 256; c++)
        match[c] = NULL;
    for (int i = 0; i < m; i++) {
        if (!match[a[i]])
            match[a[i]] = calloc((size_t)nw, sizeof(uint64_t));
        match[a[i]][i / 64] |= 1ULL << (i % 64);
    }
    uint64_t *v = malloc(sizeof(uint64_t) * (size_t)nw);
    uint64_t *u = malloc(sizeof(uint64_t) * (size_t)nw);
    uint64_t *sum = malloc(sizeof(uint64_t) * (size_t)nw);
    memset(v, 0xFF, sizeof(uint64_t) * (size_t)nw);
    for (int j = 0; j < n; j++) {
        const uint64_t *mk = match[b[j]];
        if (!mk)
            continue; /* no match anywhere: V is unchanged */
        for (int i = 0; i < nw; i++)
            u[i] = v[i] & mk[i];
        add_words(sum, v, u, nw);
        for (int i = 0; i < nw; i++)
            v[i] = sum[i] | (v[i] & ~mk[i]);
        *word_ops += nw;
    }
    int zeros = 0;
    for (int i = 0; i < m; i++)
        zeros += !((v[i / 64] >> (i % 64)) & 1u);
    for (int c = 0; c < 256; c++)
        free(match[c]);
    free(v);
    free(u);
    free(sum);
    return zeros;
}

static int lcs_dp(const unsigned char *a, int m, const unsigned char *b, int n) {
    int *prev = calloc((size_t)m + 1, sizeof(int)), *cur = calloc((size_t)m + 1, sizeof(int));
    for (int j = 1; j <= n; j++) {
        for (int i = 1; i <= m; i++)
            cur[i] = a[i - 1] == b[j - 1] ? prev[i - 1] + 1 : (prev[i] > cur[i - 1] ? prev[i] : cur[i - 1]);
        int *t = prev;
        prev = cur;
        cur = t;
    }
    int r = prev[m];
    free(prev);
    free(cur);
    return r;
}

static void gen(unsigned char *s, int n, int alpha) {
    for (int i = 0; i < n; i++)
        s[i] = (unsigned char)('a' + rnd() % (uint32_t)alpha);
    s[n] = 0;
}

int main(void) {
    const unsigned char *x = (const unsigned char *)"AGGTAB", *y = (const unsigned char *)"GXTXAYB";
    long ops = 0;
    int r = lcs_bits(x, 6, y, 7, &ops);
    printf("AGGTAB vs GXTXAYB: lcs=%d\n", r);
    check(r == 4, "classic example");
    int lens[][2] = {{1, 1}, {10, 25}, {63, 70}, {64, 64}, {65, 64}, {128, 100}, {130, 300}, {200, 200}, {500, 450}};
    int alphas[] = {2, 4, 26};
    long total_ops = 0;
    long total_lcs = 0;
    for (int i = 0; i < 9; i++) {
        for (int a = 0; a < 3; a++) {
            int m = lens[i][0], n = lens[i][1];
            unsigned char *s = malloc((size_t)m + 1), *t = malloc((size_t)n + 1);
            gen(s, m, alphas[a]);
            gen(t, n, alphas[a]);
            long wo = 0;
            int fast = lcs_bits(s, m, t, n, &wo);
            int slow = lcs_dp(s, m, t, n);
            check(fast == slow, "bit-parallel equals DP");
            if (a == 1)
                printf("m=%3d n=%3d alphabet=%2d lcs=%3d word ops=%ld (DP cells %ld)\n", m, n, alphas[a], fast, wo,
                       (long)m * n);
            total_ops += wo;
            total_lcs += fast;
            free(s);
            free(t);
        }
    }
    printf("all 27 combinations verified: sum of lcs=%ld, word ops=%ld\n", total_lcs, total_ops);
    /* identical strings and disjoint alphabets */
    unsigned char same[200], other[200];
    gen(same, 199, 5);
    memcpy(other, same, 200);
    for (int i = 0; i < 199; i++)
        other[i] = (unsigned char)(other[i] - 'a' + 'A');
    long wo = 0;
    check(lcs_bits(same, 199, same, 199, &wo) == 199, "identical strings");
    check(lcs_bits(same, 199, other, 199, &wo) == 0, "disjoint alphabets");
    printf("identical=199 disjoint=0 verified\n");
    return 0;
}
