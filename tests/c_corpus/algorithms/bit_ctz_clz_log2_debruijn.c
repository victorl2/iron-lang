/*
 * title: Trailing zeros, leading zeros and log2 with de Bruijn tables
 * topic: algorithms
 * covers: count trailing zeros, count leading zeros, integer log2, de Bruijn multiplication, power-of-two rounding, lowest set bit
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 0x9E3779B97F4A7C15ULL;

static uint64_t rnd64(void) {
    rs ^= rs >> 12;
    rs ^= rs << 25;
    rs ^= rs >> 27;
    return rs * 0x2545F4914F6CDD1DULL;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* 32-bit de Bruijn sequence B(2,5) and its position table */
static const uint32_t DB32 = 0x077CB531u;
static int db32_index[32];
/* 64-bit de Bruijn sequence B(2,6) */
static const uint64_t DB64 = 0x03F79D71B4CB0A89ULL;
static int db64_index[64];

static void build(void) {
    for (int i = 0; i < 32; i++)
        db32_index[(uint32_t)(DB32 << i) >> 27] = i;
    for (int i = 0; i < 64; i++)
        db64_index[(uint64_t)(DB64 << i) >> 58] = i;
}

static int ctz32_db(uint32_t v) {
    return db32_index[(uint32_t)((v & (0u - v)) * DB32) >> 27];
}

static int ctz64_db(uint64_t v) {
    return db64_index[(uint64_t)((v & (0ULL - v)) * DB64) >> 58];
}

static int ctz64_loop(uint64_t v) {
    int c = 0;
    while (!(v & 1u)) {
        v >>= 1;
        c++;
    }
    return c;
}

/* binary search for the highest set bit */
static int floor_log2_64(uint64_t v) {
    int r = 0;
    if (v >> 32) { v >>= 32; r += 32; }
    if (v >> 16) { v >>= 16; r += 16; }
    if (v >> 8) { v >>= 8; r += 8; }
    if (v >> 4) { v >>= 4; r += 4; }
    if (v >> 2) { v >>= 2; r += 2; }
    if (v >> 1) { r += 1; }
    return r;
}

static int clz64(uint64_t v) {
    return v == 0 ? 64 : 63 - floor_log2_64(v);
}

/* de Bruijn based log2 for 32-bit: smear bits right, then look up */
static const int LOG2_TAB[32] = {0, 9,  1,  10, 13, 21, 2,  29, 11, 14, 16, 18, 22, 25, 3, 30,
                                 8, 12, 20, 28, 15, 17, 24, 7,  19, 27, 23, 6,  26, 5,  4, 31};

static int floor_log2_32_db(uint32_t v) {
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return LOG2_TAB[(uint32_t)(v * 0x07C4ACDDu) >> 27];
}

static uint64_t next_pow2(uint64_t v) {
    if (v <= 1)
        return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

static int is_pow2(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

int main(void) {
    build();
    /* the tables must be permutations, otherwise the sequence is not de Bruijn */
    int seen32[32] = {0}, seen64[64] = {0};
    for (int i = 0; i < 32; i++)
        seen32[db32_index[i]]++;
    for (int i = 0; i < 64; i++)
        seen64[db64_index[i]]++;
    for (int i = 0; i < 32; i++)
        check(seen32[i] == 1, "32-bit table is a permutation");
    for (int i = 0; i < 64; i++)
        check(seen64[i] == 1, "64-bit table is a permutation");
    printf("de Bruijn tables valid; first row of ctz32 table:");
    for (int i = 0; i < 8; i++)
        printf(" %d", db32_index[i]);
    printf("\n");
    for (int i = 0; i < 32; i++)
        check(ctz32_db(1u << i) == i, "ctz32 on powers of two");
    for (int i = 0; i < 64; i++) {
        check(ctz64_db(1ULL << i) == i, "ctz64 on powers of two");
        check(floor_log2_64(1ULL << i) == i, "log2 on powers of two");
        check(clz64(1ULL << i) == 63 - i, "clz on powers of two");
    }
    for (int i = 0; i < 32; i++)
        check(floor_log2_32_db(1u << i) == i, "table log2 on powers of two");
    int ctz_hist[65] = {0}, lg_hist[65] = {0};
    for (int t = 0; t < 30000; t++) {
        uint64_t v = rnd64() >> (rnd64() % 63);
        if (t % 4 == 0)
            v <<= (rnd64() % 20);
        if (v == 0)
            v = 1;
        check(ctz64_db(v) == ctz64_loop(v), "ctz64 de Bruijn vs loop");
        if ((uint32_t)v)
            check(ctz32_db((uint32_t)v) == ctz64_loop((uint32_t)v), "ctz32 de Bruijn vs loop");
        int lg = floor_log2_64(v);
        check(((v >> lg) == 1u) && lg <= 63, "log2 bracket");
        check(clz64(v) + lg == 63, "clz and log2 relation");
        uint32_t v32 = (uint32_t)v;
        if (v32)
            check(floor_log2_32_db(v32) == floor_log2_64(v32), "32-bit table log2");
        uint64_t p = next_pow2(v);
        if (v <= (1ULL << 62)) {
            check(is_pow2(p) && p >= v && (p == 1 || p / 2 < v), "next power of two");
        }
        ctz_hist[ctz64_db(v)]++;
        lg_hist[lg]++;
    }
    printf("ctz histogram (first 8 buckets):");
    for (int i = 0; i < 8; i++)
        printf(" %d", ctz_hist[i]);
    printf("\nlog2 histogram (buckets 0,8,16,...,56):");
    for (int i = 0; i < 64; i += 8)
        printf(" %d", lg_hist[i]);
    printf("\n");
    printf("clz(0)=%d ctz-based lowbit(0b101000)=%d next_pow2(1000)=%llu next_pow2(1024)=%llu\n", clz64(0),
           ctz32_db(40u), (unsigned long long)next_pow2(1000), (unsigned long long)next_pow2(1024));
    check(next_pow2(1000) == 1024 && next_pow2(1024) == 1024 && next_pow2(1025) == 2048, "known values");
    /* iterate the set bits of a word from low to high using lowest-set-bit isolation */
    uint64_t w = 0x8000000100200010ULL;
    printf("set bit positions of %016llx:", (unsigned long long)w);
    int prev = -1;
    for (uint64_t x = w; x; x &= x - 1) {
        int b = ctz64_db(x);
        check(b > prev, "ascending positions");
        prev = b;
        printf(" %d", b);
    }
    printf("\n");
    return 0;
}
