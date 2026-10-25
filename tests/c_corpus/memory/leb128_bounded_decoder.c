/*
 * title: LEB128 varint codec with bounded, overflow-safe decoding
 * topic: memory
 * covers: variable-length integers, truncated input, overlong encodings, 64-bit overflow, signed LEB128, round-trip fuzz
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { V_OK, V_TRUNC, V_OVERFLOW, V_OVERLONG };
static const char *vname[] = {"ok", "truncated", "overflow", "overlong"};

static size_t enc_u(uint64_t v, uint8_t *out) {
    size_t n = 0;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if (v) b |= 0x80;
        out[n++] = b;
    } while (v);
    return n;
}

static size_t enc_s(int64_t v, uint8_t *out) {
    size_t n = 0;
    int more = 1;
    while (more) {
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7; /* arithmetic shift on all supported compilers; sign-preserving */
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) more = 0; else b |= 0x80;
        out[n++] = b;
    }
    return n;
}

/* strict decoder: rejects >10 bytes, bits beyond 64, and non-canonical (overlong) encodings */
static int dec_u(const uint8_t *p, size_t n, uint64_t *v, size_t *used) {
    uint64_t r = 0;
    for (size_t i = 0; i < 10; i++) {
        if (i >= n) return V_TRUNC;
        uint8_t b = p[i];
        unsigned shift = (unsigned)(7 * i);
        uint64_t part = b & 0x7F;
        if (shift == 63 && part > 1) return V_OVERFLOW;   /* only 1 bit left in the tenth byte */
        r |= part << shift;
        if (!(b & 0x80)) {
            if (i > 0 && b == 0) return V_OVERLONG;       /* trailing zero group */
            *v = r; *used = i + 1;
            return V_OK;
        }
    }
    return V_OVERFLOW; /* continuation bit set on the tenth byte */
}

static int dec_s(const uint8_t *p, size_t n, int64_t *v, size_t *used) {
    uint64_t r = 0;
    unsigned shift = 0;
    for (size_t i = 0; i < 10; i++) {
        if (i >= n) return V_TRUNC;
        uint8_t b = p[i];
        r |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if (shift < 64 && (b & 0x40)) r |= ~(uint64_t)0 << shift;
            *v = (int64_t)r; *used = i + 1;
            return V_OK;
        }
    }
    return V_OVERFLOW;
}

static uint64_t sx = 0xCAFEF00DD15EA5E5ull;
static uint64_t rnd(void) { sx ^= sx << 13; sx ^= sx >> 7; sx ^= sx << 17; return sx; }

int main(void) {
    uint8_t buf[16];
    uint64_t probes[] = {0, 1, 127, 128, 300, 16383, 16384, 0xFFFFFFFFull, 0x100000000ull, UINT64_MAX};
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        size_t n = enc_u(probes[i], buf);
        printf("u %-20llu ->", (unsigned long long)probes[i]);
        for (size_t k = 0; k < n; k++) printf(" %02x", buf[k]);
        printf("\n");
    }
    int64_t sp[] = {0, -1, 63, 64, -64, -65, INT64_MAX, INT64_MIN};
    for (size_t i = 0; i < sizeof sp / sizeof sp[0]; i++) {
        size_t n = enc_s(sp[i], buf);
        printf("s %-20lld ->", (long long)sp[i]);
        for (size_t k = 0; k < n; k++) printf(" %02x", buf[k]);
        printf("\n");
    }

    /* round trip with exact-size heap buffers */
    long rt = 0;
    for (int i = 0; i < 4000; i++) {
        uint64_t v = rnd();
        v >>= rnd() % 64;
        int64_t sv = (int64_t)rnd();
        sv >>= rnd() % 64;
        size_t n = enc_u(v, buf);
        uint8_t *ex = malloc(n);
        if (!ex) return 1;
        memcpy(ex, buf, n);
        uint64_t d; size_t used;
        if (dec_u(ex, n, &d, &used) != V_OK || d != v || used != n) { fprintf(stderr, "u round trip\n"); return 1; }
        free(ex);
        n = enc_s(sv, buf);
        ex = malloc(n);
        if (!ex) return 1;
        memcpy(ex, buf, n);
        int64_t ds;
        if (dec_s(ex, n, &ds, &used) != V_OK || ds != sv || used != n) { fprintf(stderr, "s round trip\n"); return 1; }
        free(ex);
        rt++;
    }
    printf("round trips: %ld\n", rt);

    /* malformed inputs */
    struct { const char *what; uint8_t b[12]; size_t n; } bad[] = {
        {"empty", {0}, 0},
        {"dangling continuation", {0x80}, 1},
        {"overlong zero", {0x80, 0x00}, 2},
        {"overlong 1", {0x81, 0x80, 0x00}, 3},
        {"11 continuation bytes", {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}, 11},
        {"10th byte too big", {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02}, 10},
        {"max u64", {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}, 10},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint64_t v = 0; size_t used = 0;
        int r = dec_u(bad[i].b, bad[i].n, &v, &used);
        printf("%-22s -> %s\n", bad[i].what, vname[r]);
    }

    /* random garbage never reads past the buffer or claims more bytes than exist */
    int ok = 0, err[4] = {0};
    for (int i = 0; i < 5000; i++) {
        size_t n = rnd() % 12;
        uint8_t *g = malloc(n ? n : 1);
        if (!g) return 1;
        for (size_t k = 0; k < n; k++) g[k] = (uint8_t)rnd();
        uint64_t v; size_t used = 0;
        int r = dec_u(g, n, &v, &used);
        if (r == V_OK) { if (used > n || used == 0) return 1; ok++; } else err[r]++;
        free(g);
    }
    printf("garbage: ok=%d truncated=%d overflow=%d overlong=%d\n", ok, err[V_TRUNC], err[V_OVERFLOW], err[V_OVERLONG]);
    return 0;
}
