/*
 * title: LSD radix sort on 32-bit keys with skipped passes
 * topic: algorithms
 * covers: radix sort, byte digits, histogram in one scan, trivial pass skipping, signed key bias, bit ops
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t st = 2718281828u;
static uint32_t rng(void) {
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

/* Sorts unsigned keys; returns number of scatter passes executed. */
static int radix_sort_u32(uint32_t *a, uint32_t *tmp, size_t n) {
    size_t hist[4][256];
    memset(hist, 0, sizeof hist);
    for (size_t i = 0; i < n; i++)
        for (int d = 0; d < 4; d++)
            hist[d][(a[i] >> (8 * d)) & 0xff]++;
    int passes = 0;
    uint32_t *src = a, *dst = tmp;
    for (int d = 0; d < 4; d++) {
        int trivial = 0;
        for (int b = 0; b < 256; b++)
            if (hist[d][b] == n)
                trivial = 1;
        if (trivial || n == 0)
            continue;
        size_t sum = 0;
        for (int b = 0; b < 256; b++) {
            size_t c = hist[d][b];
            hist[d][b] = sum;
            sum += c;
        }
        for (size_t i = 0; i < n; i++)
            dst[hist[d][(src[i] >> (8 * d)) & 0xff]++] = src[i];
        uint32_t *t = src;
        src = dst;
        dst = t;
        passes++;
    }
    if (src != a)
        memcpy(a, src, n * sizeof(uint32_t));
    return passes;
}

static void sort_i32(int32_t *a, size_t n, uint32_t *u, uint32_t *tmp, int *passes) {
    for (size_t i = 0; i < n; i++)
        u[i] = (uint32_t)a[i] ^ 0x80000000u; /* flip sign bit: signed order == unsigned order */
    *passes = radix_sort_u32(u, tmp, n);
    for (size_t i = 0; i < n; i++)
        a[i] = (int32_t)(u[i] ^ 0x80000000u);
}

int main(void) {
    enum { N = 20000 };
    static uint32_t a[N], tmp[N], ref[N];
    static const uint32_t masks[] = {0xffffffffu, 0x00ffffffu, 0x0000ffffu, 0x000000ffu, 0x00ff00ffu, 0u};
    for (int m = 0; m < 6; m++) {
        for (int i = 0; i < N; i++)
            ref[i] = a[i] = rng() & masks[m];
        int p = radix_sort_u32(a, tmp, N);
        for (int i = 1; i < N; i++)
            check(a[i - 1] <= a[i], "sorted");
        /* checksum from independent counting: xor and sum of ref must match */
        uint64_t s1 = 0, s2 = 0;
        uint32_t x1 = 0, x2 = 0;
        for (int i = 0; i < N; i++) {
            s1 += a[i];
            s2 += ref[i];
            x1 ^= a[i];
            x2 ^= ref[i];
        }
        check(s1 == s2 && x1 == x2, "permutation");
        printf("mask=%08x passes=%d min=%u max=%u\n", (unsigned)masks[m], p, (unsigned)a[0], (unsigned)a[N - 1]);
    }
    static int32_t s[N];
    for (int i = 0; i < N; i++)
        s[i] = (int32_t)rng() / 4096;
    int passes;
    sort_i32(s, N, a, tmp, &passes);
    for (int i = 1; i < N; i++)
        check(s[i - 1] <= s[i], "signed sorted");
    int negatives = 0;
    for (int i = 0; i < N; i++)
        negatives += s[i] < 0;
    printf("signed passes=%d min=%d max=%d negatives=%d\n", passes, (int)s[0], (int)s[N - 1], negatives);
    return 0;
}
