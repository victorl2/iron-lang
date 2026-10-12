/*
 * title: Strict bounded UTF-8 decoder fuzzed against exact-size buffers
 * topic: memory
 * covers: multi-byte decoding, truncated sequences, overlong forms, surrogates, range checks, encode/decode round trip
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { U_OK, U_TRUNC, U_BADLEAD, U_BADCONT, U_OVERLONG, U_SURROGATE, U_RANGE, U_NERR };
static const char *un[U_NERR] = {"ok", "truncated", "bad-lead", "bad-continuation", "overlong", "surrogate", "out-of-range"};

/* decode one code point from s[0..n); on success *used is 1..4 */
static int decode(const uint8_t *s, size_t n, uint32_t *cp, size_t *used) {
    if (n == 0) return U_TRUNC;
    uint8_t b = s[0];
    size_t need;
    uint32_t v, min;
    if (b < 0x80) { *cp = b; *used = 1; return U_OK; }
    else if (b >= 0xC0 && b < 0xE0) { need = 2; v = b & 0x1F; min = 0x80; }
    else if (b >= 0xE0 && b < 0xF0) { need = 3; v = b & 0x0F; min = 0x800; }
    else if (b >= 0xF0 && b < 0xF8) { need = 4; v = b & 0x07; min = 0x10000; }
    else return U_BADLEAD;
    for (size_t i = 1; i < need; i++) {
        if (i >= n) return U_TRUNC;
        if ((s[i] & 0xC0) != 0x80) return U_BADCONT;
        v = (v << 6) | (s[i] & 0x3F);
    }
    if (v < min) return U_OVERLONG;
    if (v >= 0xD800 && v <= 0xDFFF) return U_SURROGATE;
    if (v > 0x10FFFF) return U_RANGE;
    *cp = v; *used = need;
    return U_OK;
}

static size_t encode(uint32_t cp, uint8_t *out) {
    if (cp < 0x80) { out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) { out[0] = (uint8_t)(0xC0 | cp >> 6); out[1] = (uint8_t)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (uint8_t)(0xE0 | cp >> 12); out[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F)); out[2] = (uint8_t)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (uint8_t)(0xF0 | cp >> 18); out[1] = (uint8_t)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (uint8_t)(0x80 | (cp >> 6 & 0x3F)); out[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

/* validate a whole string: number of code points, or -1 with position of the first error */
static long count_cps(const uint8_t *s, size_t n, size_t *errpos, int *errcode) {
    size_t i = 0;
    long c = 0;
    while (i < n) {
        uint32_t cp; size_t u;
        int r = decode(s + i, n - i, &cp, &u);
        if (r != U_OK) { *errpos = i; *errcode = r; return -1; }
        i += u; c++;
    }
    return c;
}

static uint32_t st = 0xABCDEF01u;
static uint32_t rnd(void) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; }

int main(void) {
    /* exhaustive round trip over every scalar value */
    long scalars = 0;
    for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        uint8_t b[4];
        size_t n = encode(cp, b);
        uint32_t back; size_t used;
        if (decode(b, n, &back, &used) != U_OK || back != cp || used != n) { fprintf(stderr, "round trip %x\n", (unsigned)cp); return 1; }
        /* every strict prefix must be reported truncated */
        for (size_t k = 1; k < n; k++)
            if (decode(b, k, &back, &used) != U_TRUNC) { fprintf(stderr, "prefix %x/%zu\n", (unsigned)cp, k); return 1; }
        scalars++;
    }
    printf("round-tripped %ld scalar values\n", scalars);

    /* classic malformed sequences */
    struct { const char *what; uint8_t b[4]; size_t n; } bad[] = {
        {"lone continuation", {0x80}, 1}, {"0xC0 0x80 (overlong NUL)", {0xC0, 0x80}, 2},
        {"0xE0 0x80 0x80 (overlong)", {0xE0, 0x80, 0x80}, 3}, {"0xED 0xA0 0x80 (surrogate)", {0xED, 0xA0, 0x80}, 3},
        {"0xF4 0x90 0x80 0x80 (>10FFFF)", {0xF4, 0x90, 0x80, 0x80}, 4}, {"0xF8 (5-byte lead)", {0xF8, 0x88, 0x80, 0x80}, 4},
        {"0xE2 0x82 (cut)", {0xE2, 0x82}, 2}, {"0xE2 0x28 0xA1", {0xE2, 0x28, 0xA1}, 3},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint32_t cp; size_t used;
        printf("%-32s -> %s\n", bad[i].what, un[decode(bad[i].b, bad[i].n, &cp, &used)]);
    }

    /* fuzz: random bytes biased toward lead/continuation values, in exact-size heap buffers */
    int hist[U_NERR] = {0};
    long total_cps = 0;
    for (int it = 0; it < 6000; it++) {
        size_t n = rnd() % 24;
        uint8_t *g = malloc(n ? n : 1);
        if (!g) return 1;
        for (size_t i = 0; i < n; i++) {
            uint32_t k = rnd() % 8;
            g[i] = k < 3 ? (uint8_t)(rnd() % 128) : k < 5 ? (uint8_t)(0x80 + rnd() % 64) : (uint8_t)(0xC0 + rnd() % 64);
        }
        size_t pos = 0; int code = 0;
        long c = count_cps(g, n, &pos, &code);
        if (c >= 0) { hist[U_OK]++; total_cps += c; }
        else { hist[code]++; if (pos >= n) return 1; }
        free(g);
    }
    for (int i = 0; i < U_NERR; i++) printf("%-17s %d\n", un[i], hist[i]);
    printf("code points in valid strings: %ld\n", total_cps);
    return 0;
}
