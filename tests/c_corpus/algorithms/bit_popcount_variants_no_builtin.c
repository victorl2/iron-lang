/*
 * title: Population count and parity without compiler builtins
 * topic: algorithms
 * covers: popcount, Kernighan loop, SWAR parallel sums, lookup tables, multiplication trick, parity folding, bit counting over ranges
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 0x243F6A8885A308D3ULL;

static uint64_t rnd64(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int pc_loop(uint64_t x) {
    int c = 0;
    while (x) {
        c += (int)(x & 1u);
        x >>= 1;
    }
    return c;
}

static int pc_kernighan(uint64_t x) {
    int c = 0;
    while (x) {
        x &= x - 1; /* clears the lowest set bit */
        c++;
    }
    return c;
}

static int pc_swar(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

static unsigned char tab8[256];
static unsigned char tab4[16];

static void build_tables(void) {
    tab8[0] = 0;
    for (int i = 1; i < 256; i++)
        tab8[i] = (unsigned char)(tab8[i >> 1] + (i & 1));
    for (int i = 0; i < 16; i++)
        tab4[i] = tab8[i];
}

static int pc_table8(uint64_t x) {
    int c = 0;
    for (int i = 0; i < 8; i++)
        c += tab8[(x >> (8 * i)) & 0xFF];
    return c;
}

static int pc_nibble(uint64_t x) {
    int c = 0;
    for (int i = 0; i < 16; i++)
        c += tab4[(x >> (4 * i)) & 0xF];
    return c;
}

/* Divide and conquer recursion: count(x) = count(hi half) + count(lo half). */
static int pc_recursive(uint64_t x, int bits) {
    if (bits == 1)
        return (int)(x & 1u);
    int half = bits / 2;
    uint64_t mask = (half == 32) ? 0xFFFFFFFFULL : ((1ULL << half) - 1);
    return pc_recursive(x & mask, half) + pc_recursive(x >> half, half);
}

static int parity_fold(uint64_t x) {
    x ^= x >> 32;
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return (int)(x & 1u);
}

/* Total number of set bits in 0..n inclusive, by the per-bit periodic pattern. */
static uint64_t bits_up_to(uint64_t n) {
    uint64_t total = 0, m = n + 1;
    for (int b = 0; b < 63; b++) {
        uint64_t period = 1ULL << (b + 1);
        uint64_t full = m / period, rem = m % period;
        total += full * (period / 2);
        if (rem > period / 2)
            total += rem - period / 2;
        if ((1ULL << b) > n)
            break;
    }
    return total;
}

/* Popcount of an array using a carry-save adder that folds two words into a running ones register. */
static void csa(uint64_t *h, uint64_t *l, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t u = a ^ b;
    *h = (a & b) | (u & c);
    *l = u ^ c;
}

static uint64_t pc_array_csa(const uint64_t *a, int n) {
    uint64_t ones = 0, total = 0;
    int i = 0;
    for (; i + 2 <= n; i += 2) {
        uint64_t twos;
        csa(&twos, &ones, ones, a[i], a[i + 1]); /* twos carries weight 2 */
        total += 2 * (uint64_t)pc_swar(twos);
    }
    total += (uint64_t)pc_swar(ones);
    if (i < n)
        total += (uint64_t)pc_swar(a[i]);
    return total;
}

int main(void) {
    build_tables();
    uint64_t specials[] = {0, 1, 2, 3, 0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, 0xAAAAAAAAAAAAAAAAULL,
                           0x00FF00FF00FF00FFULL, 0x123456789ABCDEF0ULL, 0x7FFFFFFFFFFFFFFFULL};
    printf("special values:\n");
    for (int i = 0; i < 10; i++) {
        uint64_t x = specials[i];
        int p = pc_swar(x);
        check(p == pc_loop(x) && p == pc_kernighan(x) && p == pc_table8(x) && p == pc_nibble(x) &&
                  p == pc_recursive(x, 64),
              "all methods agree on special value");
        check(parity_fold(x) == (p & 1), "parity matches popcount parity");
        printf("  %016llx popcount=%2d parity=%d\n", (unsigned long long)x, p, parity_fold(x));
    }
    long agree = 0, sum = 0;
    int hist[65] = {0};
    for (int t = 0; t < 20000; t++) {
        uint64_t x = rnd64();
        if (t % 3 == 1)
            x &= rnd64(); /* sparse */
        if (t % 3 == 2)
            x |= rnd64(); /* dense */
        int p = pc_swar(x);
        check(p == pc_loop(x) && p == pc_kernighan(x) && p == pc_table8(x) && p == pc_nibble(x) &&
                  p == pc_recursive(x, 64),
              "methods agree on random input");
        check((pc_swar(x & 0xFFFFFFFFULL) + pc_swar(x >> 32)) == p, "additive over halves");
        check(pc_swar(x) + pc_swar(~x) == 64, "complement");
        agree++;
        sum += p;
        hist[p]++;
    }
    printf("random words checked: %ld, total set bits: %ld\n", agree, sum);
    int mode = 0;
    for (int i = 0; i <= 64; i++)
        if (hist[i] > hist[mode])
            mode = i;
    printf("most common popcount: %d (%d words)\n", mode, hist[mode]);
    /* range totals against direct summation */
    uint64_t direct = 0;
    for (uint64_t n = 0; n <= 5000; n++) {
        direct += (uint64_t)pc_kernighan(n);
        if (n == 0 || n == 1 || n == 7 || n == 8 || n == 100 || n == 1023 || n == 5000)
            check(bits_up_to(n) == direct, "closed form range total");
    }
    printf("set bits in 0..1023 = %llu, in 0..5000 = %llu\n", (unsigned long long)bits_up_to(1023),
           (unsigned long long)bits_up_to(5000));
    check(bits_up_to(1023) == 10 * 512, "0..2^10-1 has 10*2^9 set bits");
    uint64_t buf[101];
    for (int i = 0; i < 101; i++)
        buf[i] = rnd64();
    uint64_t want = 0;
    for (int i = 0; i < 101; i++)
        want += (uint64_t)pc_loop(buf[i]);
    check(pc_array_csa(buf, 101) == want, "carry-save array popcount");
    printf("array of 101 words: %llu set bits\n", (unsigned long long)want);
    return 0;
}
