/*
 * title: Endianness-independent readers and writers cross-checked
 * topic: memory
 * covers: little/big endian load-store, byte swap, sign extension of odd widths, host-order memcpy check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint64_t load_le(const unsigned char *p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static uint64_t load_be(const unsigned char *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

static void store_le(unsigned char *p, uint64_t v, int n) {
    for (int i = 0; i < n; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static void store_be(unsigned char *p, uint64_t v, int n) {
    for (int i = 0; i < n; i++)
        p[n - 1 - i] = (unsigned char)(v >> (8 * i));
}

static uint32_t bswap32(uint32_t x) {
    return (x >> 24) | ((x >> 8) & 0xFF00u) | ((x << 8) & 0xFF0000u) | (x << 24);
}

static uint64_t bswap64(uint64_t x) {
    return ((uint64_t)bswap32((uint32_t)x) << 32) | bswap32((uint32_t)(x >> 32));
}

/* sign-extend the low `bits` bits of v */
static int64_t sext(uint64_t v, int bits) {
    uint64_t m = (uint64_t)1 << (bits - 1);
    v &= ((uint64_t)1 << bits) - 1;
    return (int64_t)((v ^ m) - m);
}

static uint64_t s = 99;

static uint64_t rnd(void) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int main(void) {
    /* which order does this host use? Never printed: only used to pick the expected helper. */
    uint32_t probe = 0x01020304u;
    unsigned char pb[4];
    memcpy(pb, &probe, 4);
    int host_le = (pb[0] == 0x04);
    check(host_le || pb[0] == 0x01, "either endian");

    unsigned long checks = 0;
    for (int trial = 0; trial < 2000; trial++) {
        uint64_t v = rnd();
        for (int n = 1; n <= 8; n++) {
            uint64_t mask = (n == 8) ? ~(uint64_t)0 : (((uint64_t)1 << (8 * n)) - 1);
            unsigned char a[8], b[8];
            store_le(a, v, n);
            store_be(b, v, n);
            check(load_le(a, n) == (v & mask), "le round trip");
            check(load_be(b, n) == (v & mask), "be round trip");
            for (int i = 0; i < n; i++)
                check(a[i] == b[n - 1 - i], "le is reverse of be");
            checks += 4;
        }
        /* native memcpy agrees with the matching helper */
        uint32_t v32 = (uint32_t)v;
        unsigned char nat[4], le[4], be[4];
        memcpy(nat, &v32, 4);
        store_le(le, v32, 4);
        store_be(be, v32, 4);
        check(memcmp(nat, host_le ? le : be, 4) == 0, "native layout");
        uint64_t sw = bswap64(v);
        unsigned char x[8], y[8];
        store_le(x, v, 8);
        store_be(y, sw, 8);
        check(memcmp(x, y, 8) == 0, "bswap64 flips byte order");
        check(bswap64(sw) == v, "bswap64 involution");
        check(bswap32(bswap32(v32)) == v32, "bswap32 involution");
        checks += 5;
    }
    printf("cross checks passed: %lu\n", checks);

    /* fixed known-answer table */
    unsigned char buf[8];
    store_be(buf, 0x0102030405060708ull, 8);
    printf("be64 bytes:");
    for (int i = 0; i < 8; i++)
        printf(" %02x", buf[i]);
    printf("\n");
    store_le(buf, 0x0102030405060708ull, 8);
    printf("le64 bytes:");
    for (int i = 0; i < 8; i++)
        printf(" %02x", buf[i]);
    printf("\n");
    printf("bswap32(0x11223344)=0x%08x\n", (unsigned)bswap32(0x11223344u));
    printf("bswap64(0x0102030405060708)=0x%016llx\n", (unsigned long long)bswap64(0x0102030405060708ull));

    /* 24-bit signed samples, little-endian, as in some audio formats */
    unsigned char pcm[] = {0x00, 0x00, 0x80, 0xFF, 0xFF, 0x7F, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
    printf("24-bit samples:");
    int64_t total = 0;
    for (size_t i = 0; i + 3 <= sizeof pcm; i += 3) {
        int64_t sv = sext(load_le(pcm + i, 3), 24);
        printf(" %lld", (long long)sv);
        total += sv;
    }
    printf("\nsum %lld\n", (long long)total);
    check(sext(0xFFF, 12) == -1 && sext(0x7FF, 12) == 2047 && sext(0x800, 12) == -2048, "12-bit");
    check(sext(0x80, 8) == -128 && sext(0x7F, 8) == 127, "8-bit");

    /* mixed order: 32-bit words stored as two little-endian 16-bit halves, high half first */
    unsigned char mixed[4];
    uint32_t w = 0xAABBCCDDu;
    store_le(mixed, w >> 16, 2);
    store_le(mixed + 2, w & 0xFFFFu, 2);
    printf("middle-endian bytes: %02x %02x %02x %02x\n", mixed[0], mixed[1], mixed[2], mixed[3]);
    uint32_t back = (uint32_t)((load_le(mixed, 2) << 16) | load_le(mixed + 2, 2));
    check(back == w, "middle-endian round trip");
    return 0;
}
