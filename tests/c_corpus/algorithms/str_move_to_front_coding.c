/*
 * title: Move-to-front transform
 * topic: algorithms
 * covers: move-to-front, list rotation, Elias gamma code length, locality effect, roundtrip
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

/* Move-to-front coding, plus a comparison against frequency-count coding. */
static void mtf_encode(const unsigned char *s, int n, int *out) {
    unsigned char list[256];
    for (int i = 0; i < 256; i++)
        list[i] = (unsigned char)i;
    for (int i = 0; i < n; i++) {
        int p = 0;
        while (list[p] != s[i])
            p++;
        out[i] = p;
        memmove(list + 1, list, (size_t)p);
        list[0] = s[i];
    }
}

static void mtf_decode(const int *in, int n, unsigned char *out) {
    unsigned char list[256];
    for (int i = 0; i < 256; i++)
        list[i] = (unsigned char)i;
    for (int i = 0; i < n; i++) {
        int p = in[i];
        unsigned char c = list[p];
        memmove(list + 1, list, (size_t)p);
        list[0] = c;
        out[i] = c;
    }
}

/* Elias gamma code length for value v >= 1 */
static int gamma_len(int v) {
    int b = 0;
    while (v >> (b + 1))
        b++;
    return 2 * b + 1;
}

static long coded_bits(const int *codes, int n) {
    long bits = 0;
    for (int i = 0; i < n; i++)
        bits += gamma_len(codes[i] + 1);
    return bits;
}

int main(void) {
    const char *demo = "bananaaa";
    int out[64];
    mtf_encode((const unsigned char *)demo, 8, out);
    printf("mtf(%s):", demo);
    for (int i = 0; i < 8; i++)
        printf(" %d", out[i]);
    printf("\n");
    static const int want[8] = {98, 98, 110, 1, 1, 1, 0, 0};
    for (int i = 0; i < 8; i++)
        check(out[i] == want[i], "known mtf");
    unsigned char back[64];
    mtf_decode(out, 8, back);
    check(memcmp(back, demo, 8) == 0, "known decode");

    /* Data with locality (few active symbols at a time) vs uniform */
    static unsigned char src[4000], dec[4000];
    static int codes[4000], raw[4000];
    const char *names[] = {"local (drifting alphabet)", "uniform 16 symbols", "single symbol"};
    for (int mode = 0; mode < 3; mode++) {
        int n = 4000, base = 0;
        for (int i = 0; i < n; i++) {
            if (mode == 0) {
                if (rnd() % 50 == 0)
                    base = (int)(rnd() % 200);
                src[i] = (unsigned char)(base + rnd() % 3);
            } else if (mode == 1)
                src[i] = (unsigned char)(rnd() % 16);
            else
                src[i] = 42;
        }
        mtf_encode(src, n, codes);
        mtf_decode(codes, n, dec);
        check(memcmp(src, dec, (size_t)n) == 0, "roundtrip");
        for (int i = 0; i < n; i++)
            raw[i] = src[i];
        long zeros = 0, big = 0;
        for (int i = 0; i < n; i++) {
            zeros += codes[i] == 0;
            big += codes[i] >= 16;
        }
        printf("%-26s zeros=%4ld big=%4ld gamma(mtf)=%6ld gamma(raw)=%6ld\n", names[mode], zeros, big,
               coded_bits(codes, n), coded_bits(raw, n));
    }
    return 0;
}
