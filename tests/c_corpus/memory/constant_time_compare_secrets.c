/*
 * title: Constant-time comparison and bounded secret handling
 * topic: memory
 * covers: branch-free compare, early-exit leak measured by work counters, length hiding, select masks, memcmp equivalence
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* We cannot time anything portably, so we count "work": the number of byte comparisons executed.
 * An early-exit compare's work depends on the secret; the constant-time compare's does not. */
static long work_naive, work_ct;

static int naive_eq(const unsigned char *a, const unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        work_naive++;
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static int ct_eq(const unsigned char *a, const unsigned char *b, size_t n) {
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) {
        work_ct++;
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    /* map diff==0 to 1 and any non-zero to 0 without branching */
    return (int)(((unsigned)diff - 1u) >> 31) & 1;
}

/* constant-time select: returns a if flag==1 else b */
static uint32_t ct_select(uint32_t flag, uint32_t a, uint32_t b) {
    uint32_t mask = 0u - (flag & 1u);
    return (a & mask) | (b & ~mask);
}

/* constant-time three-way compare of equal-length buffers: -1, 0, 1 */
static int ct_cmp(const unsigned char *a, const unsigned char *b, size_t n) {
    int gt = 0, lt = 0;
    for (size_t i = 0; i < n; i++) {
        int x = a[i], y = b[i];
        int d = x - y;                 /* -255..255 */
        int is_gt = (int)((unsigned)(-d) >> 31);   /* 1 if d > 0 */
        int is_lt = (int)((unsigned)d >> 31);      /* 1 if d < 0 */
        gt |= is_gt & ~(gt | lt);
        lt |= is_lt & ~(gt | lt);
    }
    return gt - lt;
}

/* length-hiding compare: compares against a fixed-size padded buffer of `cap` bytes */
static int ct_eq_padded(const unsigned char *a, size_t alen, const unsigned char *b, size_t blen, size_t cap) {
    unsigned char diff = (unsigned char)(alen != blen);
    for (size_t i = 0; i < cap; i++) {
        unsigned char x = i < alen ? a[i] : 0;
        unsigned char y = i < blen ? b[i] : 0;
        diff |= (unsigned char)(x ^ y);
    }
    return diff == 0;
}

static uint32_t s = 20240229u;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

int main(void) {
    unsigned char secret[16], guess[16];
    for (int i = 0; i < 16; i++) secret[i] = (unsigned char)rnd();

    /* work profile as the guess matches longer and longer prefixes */
    printf("matching prefix -> work naive / constant-time\n");
    for (int k = 0; k <= 16; k += 4) {
        memcpy(guess, secret, 16);
        if (k < 16) guess[k] ^= 0x40;
        long wn0 = work_naive, wc0 = work_ct;
        int e1 = naive_eq(secret, guess, 16), e2 = ct_eq(secret, guess, 16);
        printf("  prefix %2d: eq=%d/%d work %2ld / %2ld\n", k, e1, e2, work_naive - wn0, work_ct - wc0);
    }

    /* equivalence with memcmp over random data, including single-bit differences */
    long agree_eq = 0, agree_cmp = 0, trials = 20000;
    for (long t = 0; t < trials; t++) {
        unsigned char a[8], b[8];
        for (int i = 0; i < 8; i++) a[i] = (unsigned char)rnd();
        memcpy(b, a, 8);
        uint32_t mode = rnd() % 4;
        if (mode == 1) {
            unsigned pos = rnd() % 8;
            b[pos] ^= (unsigned char)(1u << (rnd() % 8));
        }
        else if (mode == 2) for (int i = 0; i < 8; i++) b[i] = (unsigned char)rnd();
        else if (mode == 3) { size_t p = rnd() % 8; b[p] = (unsigned char)(a[p] + 1 + rnd() % 3); }
        int m = memcmp(a, b, 8);
        int msign = (m > 0) - (m < 0);
        agree_eq += ct_eq(a, b, 8) == (m == 0);
        agree_cmp += ct_cmp(a, b, 8) == msign;
    }
    printf("ct_eq agrees with memcmp: %ld/%ld, ct_cmp sign agrees: %ld/%ld\n", agree_eq, trials, agree_cmp, trials);
    if (agree_eq != trials || agree_cmp != trials) return 1;

    /* select and padded compare */
    printf("select: %u %u\n", (unsigned)ct_select(1, 111, 222), (unsigned)ct_select(0, 111, 222));
    const unsigned char k1[] = "token-abc", k2[] = "token-abcd", k3[] = "token-abd";
    printf("padded eq: same=%d longer=%d differs=%d empty-vs-empty=%d\n",
           ct_eq_padded(k1, 9, k1, 9, 32), ct_eq_padded(k1, 9, k2, 10, 32), ct_eq_padded(k1, 9, k3, 9, 32), ct_eq_padded(k1, 0, k2, 0, 32));
    /* a trailing zero is not the same as a shorter string: the length term catches it */
    unsigned char z1[4] = {1, 2, 3, 0}, z2[3] = {1, 2, 3};
    printf("zero-padding trap: %d\n", ct_eq_padded(z1, 4, z2, 3, 8));
    return 0;
}
