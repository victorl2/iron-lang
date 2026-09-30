/*
 * title: Lucas-Lehmer test on binary bignums
 * topic: algorithms
 * covers: Lucas-Lehmer, Mersenne primes, reduction mod 2^p-1 by folding, binary limb squaring, bit manipulation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint64_t u64;

#define LIMBS 8 /* supports p up to 127 with room to square (2*LIMBS limbs) */

typedef struct { u32 w[2 * LIMBS]; } Wide;

static void square(const u32 *a, Wide *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < LIMBS; i++) {
        u64 carry = 0;
        for (int j = 0; j < LIMBS; j++) {
            u64 cur = (u64)out->w[i + j] + (u64)a[i] * a[j] + carry;
            out->w[i + j] = (u32)cur;
            carry = cur >> 32;
        }
        out->w[i + LIMBS] += (u32)carry;
    }
}

static int get_bit(const u32 *a, int i) { return (a[i / 32] >> (i % 32)) & 1u; }

/* x mod (2^p - 1): fold high bits onto low bits until it fits */
static void fold(const Wide *x, int p, u32 *r) {
    u32 acc[2 * LIMBS + 1];
    memcpy(acc, x->w, sizeof x->w);
    acc[2 * LIMBS] = 0;
    for (;;) {
        /* split acc = hi * 2^p + lo */
        u32 lo[2 * LIMBS + 1], hi[2 * LIMBS + 1];
        memset(lo, 0, sizeof lo);
        memset(hi, 0, sizeof hi);
        int any_hi = 0;
        for (int i = 0; i < 32 * (2 * LIMBS); i++) {
            int b = get_bit(acc, i);
            if (i < p) lo[i / 32] |= (u32)b << (i % 32);
            else if (b) { int k = i - p; hi[k / 32] |= 1u << (k % 32); any_hi = 1; }
        }
        if (!any_hi) { memcpy(acc, lo, sizeof lo); break; }
        u64 carry = 0;
        for (int i = 0; i < 2 * LIMBS + 1; i++) {
            u64 cur = (u64)lo[i] + hi[i] + carry;
            acc[i] = (u32)cur;
            carry = cur >> 32;
        }
    }
    /* acc may equal 2^p - 1 exactly, which is 0 mod M */
    int all_ones = 1;
    for (int i = 0; i < p; i++) if (!get_bit(acc, i)) { all_ones = 0; break; }
    for (int i = 0; i < LIMBS; i++) r[i] = all_ones ? 0 : acc[i];
}

static void sub2_mod(u32 *r, int p) {
    /* r = r - 2 mod (2^p - 1) */
    u32 m[LIMBS] = {0};
    for (int i = 0; i < p; i++) m[i / 32] |= 1u << (i % 32);
    int lt2 = 1;
    for (int i = 1; i < LIMBS; i++) if (r[i]) lt2 = 0;
    if (r[0] >= 2) lt2 = 0;
    if (lt2) { /* r + M - 2 */
        u64 carry = 0;
        for (int i = 0; i < LIMBS; i++) {
            u64 cur = (u64)r[i] + m[i] + carry;
            r[i] = (u32)cur;
            carry = cur >> 32;
        }
    }
    u64 borrow = 2;
    for (int i = 0; i < LIMBS; i++) {
        u64 sub = borrow;
        borrow = r[i] < sub ? 1 : 0;
        r[i] = (u32)((u64)r[i] - sub);
        if (borrow == 0) break;
    }
}

static int lucas_lehmer(int p) {
    if (p == 2) return 1;
    u32 s[LIMBS] = {4};
    for (int i = 0; i < p - 2; i++) {
        Wide sq;
        square(s, &sq);
        fold(&sq, p, s);
        sub2_mod(s, p);
    }
    for (int i = 0; i < LIMBS; i++) if (s[i]) return 0;
    return 1;
}

static int is_prime_small(int n) {
    if (n < 2) return 0;
    for (int d = 2; d * d <= n; d++) if (n % d == 0) return 0;
    return 1;
}

int main(void) {
    printf("Mersenne exponents found:");
    int found = 0;
    int prev_ok = 1;
    for (int p = 2; p <= 127; p++) {
        if (!is_prime_small(p)) continue;
        int ll = lucas_lehmer(p);
        if (ll) { printf(" %d", p); found++; }
        /* cross-check small ones by trial division of 2^p - 1 */
        if (p <= 31) {
            u64 m = (1ull << p) - 1;
            int prime = 1;
            for (u64 d = 2; d * d <= m; d++) if (m % d == 0) { prime = 0; break; }
            if (prime != ll) { prev_ok = 0; fprintf(stderr, "LL disagrees at p=%d\n", p); }
        }
    }
    printf("\ncount: %d\n", found);
    if (!prev_ok) return 1;
    int expect[] = {2, 3, 5, 7, 13, 17, 19, 31, 61, 89, 107, 127};
    int idx = 0;
    for (int p = 2; p <= 127; p++)
        if (is_prime_small(p) && lucas_lehmer(p)) { if (expect[idx++] != p) return 1; }
    if (idx != 12) return 1;
    /* composite Mersenne numbers with prime exponent and their smallest factor */
    int comps[] = {11, 23, 29, 37, 41, 43, 47};
    for (size_t i = 0; i < sizeof comps / sizeof comps[0]; i++) {
        u64 m = (1ull << comps[i]) - 1, f = 0;
        for (u64 d = 2 * (u64)comps[i] + 1; d * d <= m; d += 2 * (u64)comps[i])
            if (m % d == 0) { f = d; break; } /* factors of M_p are 2kp+1 */
        printf("M%d composite, smallest factor %llu = 2*k*%d+1\n", comps[i], (unsigned long long)f, comps[i]);
        if (!f) return 1;
    }
    /* perfect numbers from the Mersenne primes below 32 as a link back to number theory */
    for (int i = 0; i < 8; i++) {
        int p = expect[i];
        if (p > 31) break;
        unsigned long long perf = (1ull << (p - 1)) * ((1ull << p) - 1);
        printf("perfect number from p=%d: %llu\n", p, perf);
    }
    return 0;
}
