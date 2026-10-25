/*
 * title: Hash function known answers and avalanche measurement
 * topic: data_structures
 * covers: FNV-1a, djb2, one-at-a-time, murmur3, avalanche criterion, bit bias matrix, bucket uniformity chi-square
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED __attribute__((unused))

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static UNUSED uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static UNUSED void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}
static UNUSED uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static UNUSED uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Reference model: unordered array with linear scan. */
enum { REF_CAP = 1 << 14 };
static uint32_t ref_k[REF_CAP];
static int ref_v[REF_CAP];
static int ref_n;
static UNUSED int ref_find(uint32_t k) {
    for (int i = 0; i < ref_n; i++)
        if (ref_k[i] == k)
            return i;
    return -1;
}
static UNUSED int ref_put(uint32_t k, int v) { /* 1 if new */
    int i = ref_find(k);
    if (i >= 0) {
        ref_v[i] = v;
        return 0;
    }
    check(ref_n < REF_CAP, "ref capacity");
    ref_k[ref_n] = k;
    ref_v[ref_n++] = v;
    return 1;
}
static UNUSED int ref_del(uint32_t k) {
    int i = ref_find(k);
    if (i < 0)
        return 0;
    ref_k[i] = ref_k[ref_n - 1];
    ref_v[i] = ref_v[ref_n - 1];
    ref_n--;
    return 1;
}
static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}
static uint32_t djb2(const uint8_t *p, size_t n) {
    uint32_t h = 5381u;
    for (size_t i = 0; i < n; i++)
        h = h * 33u + p[i];
    return h;
}
static uint32_t oaat(const uint8_t *p, size_t n) {
    uint32_t h = 0;
    for (size_t i = 0; i < n; i++) {
        h += p[i];
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}
static uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static uint32_t murmur3(const uint8_t *p, size_t n, uint32_t seed) {
    uint32_t h = seed;
    size_t nblocks = n / 4;
    for (size_t i = 0; i < nblocks; i++) {
        uint32_t k = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
        k *= 0xcc9e2d51u;
        k = rotl32(k, 15);
        k *= 0x1b873593u;
        h ^= k;
        h = rotl32(h, 13);
        h = h * 5u + 0xe6546b64u;
    }
    uint32_t k = 0;
    const uint8_t *tail = p + nblocks * 4;
    switch (n & 3) {
    case 3: k ^= (uint32_t)tail[2] << 16; /* fallthrough */
    case 2: k ^= (uint32_t)tail[1] << 8; /* fallthrough */
    case 1:
        k ^= tail[0];
        k *= 0xcc9e2d51u;
        k = rotl32(k, 15);
        k *= 0x1b873593u;
        h ^= k;
    }
    h ^= (uint32_t)n;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

/* 4-byte little-endian views of a 32-bit integer for the avalanche runs */
static void le4(uint32_t x, uint8_t *b) {
    for (int i = 0; i < 4; i++)
        b[i] = (uint8_t)(x >> (8 * i));
}
static uint32_t w_fnv(uint32_t x) { uint8_t b[4]; le4(x, b); return fnv1a(b, 4); }
static uint32_t w_djb(uint32_t x) { uint8_t b[4]; le4(x, b); return djb2(b, 4); }
static uint32_t w_oaat(uint32_t x) { uint8_t b[4]; le4(x, b); return oaat(b, 4); }
static uint32_t w_mur(uint32_t x) { uint8_t b[4]; le4(x, b); return murmur3(b, 4, 0); }
static uint32_t w_mix(uint32_t x) { return mix32(x); }
static uint32_t w_weak(uint32_t x) { return x ^ (x >> 16); }

int main(void) {
    struct {
        const char *s;
        uint32_t fnv, djb, oa, mu;
    } kat[] = {{"", 0x811c9dc5u, 5381u, 0x0u, 0x0u},
               {"a", 0xe40c292cu, 177670u, 0xca2e9442u, 0x3c2569b2u},
               {"foobar", 0xbf9cf968u, 4259602622u, 0xf952fde7u, 0xa4c4d4bdu},
               {"hello", 0x4f9f2cabu, 261238937u, 0xc8fd181bu, 0x248bfa47u},
               {"The quick brown fox jumps over the lazy dog", 0x048fff90u, 885799134u, 0x519e91f5u, 0x2e4ff723u}};
    for (size_t i = 0; i < sizeof kat / sizeof kat[0]; i++) {
        const uint8_t *p = (const uint8_t *)kat[i].s;
        size_t n = strlen(kat[i].s);
        check(fnv1a(p, n) == kat[i].fnv, "fnv1a vector");
        check(djb2(p, n) == kat[i].djb, "djb2 vector");
        check(oaat(p, n) == kat[i].oa, "oaat vector");
        check(murmur3(p, n, 0) == kat[i].mu, "murmur3 vector");
    }
    printf("known-answer vectors passed for 4 functions x %zu inputs\n", sizeof kat / sizeof kat[0]);
    printf("fnv1a(\"hello\")=%08x murmur3(\"hello\")=%08x oaat(\"hello\")=%08x\n", fnv1a((const uint8_t *)"hello", 5),
           murmur3((const uint8_t *)"hello", 5, 0), oaat((const uint8_t *)"hello", 5));

    uint32_t (*fn[6])(uint32_t) = {w_fnv, w_djb, w_oaat, w_mur, w_mix, w_weak};
    const char *names[6] = {"fnv1a", "djb2", "oaat", "murmur3", "mix32", "x^x>>16"};
    enum { T = 3000 };
    for (int f = 0; f < 6; f++) {
        static int flips[32][32];
        memset(flips, 0, sizeof flips);
        long total = 0;
        for (int t = 0; t < T; t++) {
            uint32_t x = (uint32_t)rnd();
            uint32_t h = fn[f](x);
            for (int i = 0; i < 32; i++) {
                uint32_t d = h ^ fn[f](x ^ (1u << i));
                total += __builtin_popcount(d);
                for (int o = 0; o < 32; o++)
                    flips[i][o] += (int)((d >> o) & 1u);
            }
        }
        int worst = 0, weak_cells = 0;
        for (int i = 0; i < 32; i++)
            for (int o = 0; o < 32; o++) {
                int dev = flips[i][o] * 2 - T; /* deviation from T/2 scaled by 2 */
                if (dev < 0)
                    dev = -dev;
                if (dev > worst)
                    worst = dev;
                weak_cells += dev > T * 3 / 10; /* flip probability outside [0.35, 0.65] */
            }
        printf("%-8s mean output bits flipped=%.3f (ideal 16) worst bias=%.3f cells with |p-0.5|>0.15: %d/1024\n", names[f],
               (double)total / (double)(T * 32), (double)worst / (2.0 * T), weak_cells);
    }
    /* chi-square of bucket occupancy for sequential keys hashed to 64 buckets */
    for (int f = 0; f < 6; f++) {
        int c[64] = {0};
        for (uint32_t x = 0; x < 6400; x++)
            c[fn[f](x) & 63]++;
        double chi = 0;
        for (int b = 0; b < 64; b++)
            chi += ((double)c[b] - 100.0) * ((double)c[b] - 100.0) / 100.0;
        printf("%-8s chi-square over 64 low-bit buckets (expected ~63): %.1f\n", names[f], chi);
    }
    return 0;
}
