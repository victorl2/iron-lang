/*
 * title: Index reduction schemes and structured key patterns
 * topic: data_structures
 * covers: fibonacci hashing, multiplicative hashing, modulo by power of two, prime modulus, structured keys, bucket collision statistics
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
typedef uint32_t (*Reduce)(uint32_t key, unsigned bits);

static uint32_t r_mask(uint32_t k, unsigned bits) { return k & ((1u << bits) - 1); }
static uint32_t r_prime(uint32_t k, unsigned bits) { return k % ((1u << bits) - 1); } /* Mersenne prime 8191 for 13 bits */
static uint32_t r_fib(uint32_t k, unsigned bits) { return (uint32_t)(((uint64_t)k * 2654435769u & 0xffffffffu) >> (32 - bits)); }
static uint32_t r_mix(uint32_t k, unsigned bits) { return mix32(k) >> (32 - bits); }

enum { NK = 4096, BITS = 13 };

static void gen(int pattern, uint32_t *keys) {
    for (int i = 0; i < NK; i++) {
        uint32_t u = (uint32_t)i;
        switch (pattern) {
        case 0: keys[i] = u; break;                            /* sequential */
        case 1: keys[i] = u * 8192u; break;                    /* multiples of the table size */
        case 2: keys[i] = u * 1000u + 7u; break;               /* decimal stride */
        case 3: keys[i] = (u << 16) | (u & 3u); break;         /* high-bit heavy */
        case 4: keys[i] = (uint32_t)rnd(); break;              /* random */
        default: keys[i] = 0x10000u + (u % 64u) * 4096u + u / 64u; break; /* 2-D array layout */
        }
    }
}

int main(void) {
    static const char *pat_names[6] = {"sequential", "stride 8192", "stride 1000", "high-bits", "random", "2-D layout"};
    static const char *red_names[4] = {"mask", "mod 8191", "fibonacci", "mix32"};
    Reduce reds[4] = {r_mask, r_prime, r_fib, r_mix};
    static uint32_t keys[NK];
    static uint16_t cnt[1 << BITS];
    printf("%-12s |", "pattern");
    for (int r = 0; r < 4; r++)
        printf(" %-9s(max/empty)", red_names[r]);
    printf("\n");
    for (int p = 0; p < 6; p++) {
        gen(p, keys);
        printf("%-12s |", pat_names[p]);
        for (int r = 0; r < 4; r++) {
            memset(cnt, 0, sizeof cnt);
            for (int i = 0; i < NK; i++) {
                uint32_t b = reds[r](keys[i], BITS);
                check(b < (1u << BITS), "index in range");
                cnt[b]++;
            }
            int mx = 0, empty = 0, total = 0;
            for (int b = 0; b < (1 << BITS); b++) {
                if (cnt[b] > mx)
                    mx = cnt[b];
                empty += cnt[b] == 0;
                total += cnt[b];
            }
            check(total == NK, "all keys counted");
            printf(" %9d/%-9d", mx, empty);
        }
        printf("\n");
    }
    /* consecutive keys: the cyclic gaps between successive indices take only a few distinct values (three-gap theorem) */
    uint32_t prev = r_fib(0, BITS), gmin = UINT32_MAX, gmax = 0;
    static uint8_t gapseen[1 << BITS];
    int distinct_gaps = 0;
    for (uint32_t k = 1; k < 8192; k++) {
        uint32_t cur = r_fib(k, BITS);
        uint32_t gap = (cur - prev) & ((1u << BITS) - 1);
        distinct_gaps += !gapseen[gap];
        gapseen[gap] = 1;
        if (gap < gmin)
            gmin = gap;
        if (gap > gmax)
            gmax = gap;
        prev = cur;
    }
    printf("fibonacci consecutive-key index gaps: %d distinct values, min %u max %u of 8192\n", distinct_gaps, gmin, gmax);
    check(distinct_gaps <= 3, "three-gap theorem");
    /* first 8192 consecutive keys must land in distinct buckets thanks to the three-gap theorem */
    static uint8_t seen[1 << BITS];
    memset(seen, 0, sizeof seen);
    int distinct = 0;
    for (uint32_t k = 0; k < 8192; k++) {
        uint32_t b = r_fib(k, BITS);
        distinct += !seen[b];
        seen[b] = 1;
    }
    printf("fibonacci: %d distinct buckets for 8192 consecutive keys in 8192 buckets\n", distinct);
    check(distinct > 5000, "fibonacci spreads consecutive keys");
    return 0;
}
