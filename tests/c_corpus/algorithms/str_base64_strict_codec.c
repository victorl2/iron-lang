/*
 * title: Base64 codec with strict decoding
 * topic: algorithms
 * covers: base64, URL-safe alphabet, optional padding, canonical trailing bits, RFC 4648 test vectors, bit accumulator
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

/* Base64 (RFC 4648) with padding control, URL-safe alphabet, strict decoding. */
static const char STD[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64_encode(const unsigned char *in, size_t n, char *out, const char *alpha, int pad) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n)
            v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n)
            v |= in[i + 2];
        out[o++] = alpha[v >> 18 & 63];
        out[o++] = alpha[v >> 12 & 63];
        if (i + 1 < n)
            out[o++] = alpha[v >> 6 & 63];
        else if (pad)
            out[o++] = '=';
        if (i + 2 < n)
            out[o++] = alpha[v & 63];
        else if (pad)
            out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

/* returns decoded length or -1; strict: canonical trailing bits must be zero */
static long b64_decode(const char *in, unsigned char *out, const char *alpha, int require_pad) {
    int rev[256];
    for (int i = 0; i < 256; i++)
        rev[i] = -1;
    for (int i = 0; i < 64; i++)
        rev[(unsigned char)alpha[i]] = i;
    size_t n = strlen(in), o = 0;
    size_t data = n;
    while (data > 0 && in[data - 1] == '=')
        data--;
    size_t padn = n - data;
    if (padn > 2)
        return -1;
    if (require_pad && n % 4 != 0)
        return -1;
    if (padn && (n % 4 != 0))
        return -1;
    if (data % 4 == 1)
        return -1;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < data; i++) {
        int v = rev[(unsigned char)in[i]];
        if (v < 0)
            return -1;
        acc = acc << 6 | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)(acc >> bits & 0xFF);
        }
    }
    if (bits && (acc & ((1u << bits) - 1)) != 0)
        return -1; /* non-canonical trailing bits */
    return (long)o;
}

int main(void) {
    static const char *vec[][2] = {{"", ""},           {"f", "Zg=="},        {"fo", "Zm8="},
                                   {"foo", "Zm9v"},    {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="},
                                   {"foobar", "Zm9vYmFy"}};
    char enc[64];
    unsigned char dec[64];
    for (size_t i = 0; i < sizeof vec / sizeof vec[0]; i++) {
        size_t n = b64_encode((const unsigned char *)vec[i][0], strlen(vec[i][0]), enc, STD, 1);
        check(strcmp(enc, vec[i][1]) == 0 && n == strlen(vec[i][1]), "RFC vector");
        long d = b64_decode(enc, dec, STD, 1);
        check(d == (long)strlen(vec[i][0]) && memcmp(dec, vec[i][0], (size_t)d) == 0, "decode vector");
        printf("\"%s\" -> \"%s\"\n", vec[i][0], enc);
    }
    /* URL-safe and unpadded on bytes that exercise + and / */
    unsigned char bin[6] = {0xFB, 0xFF, 0xBF, 0xFE, 0x3E, 0x3F};
    char a[16], b[16];
    b64_encode(bin, 6, a, STD, 1);
    b64_encode(bin, 6, b, URL, 0);
    printf("std %s url %s\n", a, b);
    b64_encode(bin, 5, a, URL, 0);
    printf("5 bytes unpadded url: %s (len %zu)\n", a, strlen(a));

    /* rejection tests */
    const char *bad[] = {"Zg=", "Zg", "Zh==", "Z===", "Zm9v!A==", "Zm9vY", "Zg==Zg=="};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        long r = b64_decode(bad[i], dec, STD, 1);
        check(r < 0, "must reject");
        printf("reject \"%s\"\n", bad[i]);
    }
    /* random roundtrips across lengths and both alphabets */
    static unsigned char src[300], back[300];
    static char big[500];
    long total = 0;
    for (int t = 0; t < 600; t++) {
        size_t n = rnd() % 200;
        for (size_t i = 0; i < n; i++)
            src[i] = (unsigned char)rnd();
        int url = t & 1, pad = (t >> 1) & 1;
        size_t el = b64_encode(src, n, big, url ? URL : STD, pad);
        check(el == (pad ? (n + 2) / 3 * 4 : (n * 8 + 5) / 6), "encoded length");
        long d = b64_decode(big, back, url ? URL : STD, pad);
        check(d == (long)n && memcmp(src, back, n) == 0, "random roundtrip");
        total += (long)el;
    }
    printf("random roundtrips: 600 ok, %ld chars total\n", total);
    return 0;
}
